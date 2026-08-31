#include <Network.h>
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include "version.h"
#include <knx.h>
#ifndef DISABLE_IMPROV
#include "ImprovWiFiLibrary.h"
#endif
#include "nvs_flash.h"

#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include "index_html.h"
#include <knx/bau091A.h>
#include <knx/ip_data_link_layer.h>

#include <TPUart/Interface/ESP32.h>
#include <TPUart/Types.h>   // U_INT_REG_RD_REQ_ACR0 / ACR0_FLAG_* for the ACR0 readback

#include <esp_mac.h>   // esp_read_mac() — every target, not just the W5500 ones

// One DHCP identity for the whole device. Without this a stick shows up in the
// router's lease list under the platform default: the Arduino ETH class sets no
// hostname at all, so the ethernet netif keeps lwIP's "espressif", and WiFi
// reports "esp32c6-…". The suffix is the one the fallback AP already uses
// ("TUL AP D501"), so the same stick is recognisable on the wire, over WiFi and
// in the AP list.
static const char* deviceHostname() {
    static char name[16] = {0};
    if (name[0] == 0) {
        uint8_t m[6] = {0};
        // ESP_MAC_WIFI_SOFTAP is what the AP name is built from (base MAC + 1).
        // Not esp_efuse_mac_get_default(): on the C6 that hands back the middle
        // of the EUI-64 form.
        if (esp_read_mac(m, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
            snprintf(name, sizeof(name), "tul-%02x%02x", m[4], m[5]);
        } else {
            snprintf(name, sizeof(name), "tul");
        }
    }
    return name;
}

#ifdef W5500_ETH
#include <ETH.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_eth.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>

// === Optional W5500 ethernet (TUL32 FPC add-on) =============================
// The module is optional, so this is a runtime probe, not a build variant:
// ETH.begin() reads the W5500 chip version through esp_eth_driver_install and
// returns false when no chip answers. One binary serves both boards.
//
// Netif priority matters here. ESP_NETIF_INHERENT_DEFAULT_WIFI_STA has
// route_prio 100 and ..._ETH only 50, so with both interfaces up the default
// netif — and with it the IGMP join behind NetworkUDP::beginMulticast(), which
// passes imr_interface = INADDR_ANY — would stay on WiFi even with the cable
// plugged in. Raising ETH above the STA makes the wire win whenever it has a
// link, and esp_netif falls back to WiFi by itself when it does not.
#define ETH_ROUTE_PRIO 120

// Boot-time windows for the cable decision. The link wait is paid on every
// boot of a module-equipped stick with no cable plugged, so keep it short; the
// DHCP wait only runs once a link is actually there.
#define ETH_LINK_WAIT_MS 3000
#define ETH_DHCP_WAIT_MS 8000
// How long a radio the web UI woke up stays on without a connect following.
#define WIFI_REPARK_MS   120000

static bool ethPresent = false;    // W5500 answered → driver installed

// Which interface the KNX stack is speaking on. Tracked rather than derived per
// call so a change can be acted on exactly once.
enum ActiveIf { IF_NONE, IF_ETH, IF_WIFI };
static ActiveIf activeIf    = IF_NONE;
static uint32_t lastIfCheck = 0;

static const char* activeIfName(ActiveIf i) {
    return i == IF_ETH ? "ethernet" : i == IF_WIFI ? "wifi" : "none";
}

// True while ethernet is the interface KNX should be speaking on. The KNX
// platform layer calls this (weak symbol in esp32_platform.cpp) to report the
// right IP/mask/gateway/MAC in search responses and HPAI structures.
bool knxUseEthernet() {
    return ethPresent && ETH.linkUp() && ETH.localIP() != IPAddress((uint32_t)0);
}

// "Carrying the gateway" means associated AND holding an address — a STA can
// sit in WL_CONNECTED with 0.0.0.0 after a lost DHCP lease.
static ActiveIf currentActiveIf() {
    if (knxUseEthernet()) {
        return IF_ETH;
    }
    if (WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0) {
        return IF_WIFI;
    }
    return IF_NONE;
}

// Give the ethernet interface the busware MAC burnt into eFuse CUSTOM_MAC
// (BLOCK3) during production test, instead of the address ETH.begin() derives
// from the Espressif base MAC (base+eth_index with the locally-administered bit
// set — a0:f2:… becomes a2:f2:…). Devices then appear on the wire under the
// OUI they were sold with, and DHCP reservations survive a firmware change.
// A board without a burnt CUSTOM_MAC keeps the derived address.
static bool applyBuswareEthMac() {
    // Read the 48-bit field straight out of BLOCK3. esp_efuse_mac_get_custom()
    // is the wrong door on the C6: it hands back the first six bytes of the
    // EUI-64 form, so a4:50:55:02:00:01 comes out as a4:50:55:ff:fe:02.
    uint8_t mac[6] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA_MAC_CUSTOM, mac, 48) != ESP_OK) {
        return false;
    }
    // An unburnt field reads as all-zero; so does a malformed one. Either way
    // there is no busware identity here, so keep the derived address.
    bool allZero = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i]) { allZero = false; break; }
    }
    if (allZero || (mac[0] & 0x01)) {   // no multicast bit in a device MAC
        return false;
    }
    esp_eth_handle_t h = ETH.handle();
    esp_netif_t* nif = ETH.netif();
    if (!h || !nif) {
        return false;
    }
    // Both layers have to agree: the MAC driver answers ARP from its own copy,
    // esp_netif hands its copy to lwIP. Neither accepts the change while the
    // interface is running.
    esp_eth_stop(h);
    bool ok = esp_eth_ioctl(h, ETH_CMD_S_MAC_ADDR, mac) == ESP_OK;
    ok = (esp_netif_set_mac(nif, mac) == ESP_OK) && ok;
    esp_eth_start(h);
    return ok;
}

static void initEthernet() {
    // A board without the module is the normal case, not a fault. Muting the
    // driver tags for the probe keeps three red esp_eth/w5500 chip-ID errors
    // out of every boot log on the far more common unpopulated board; the
    // plain-language line below says the same thing without alarming anyone.
    esp_log_level_set("w5500.mac", ESP_LOG_NONE);
    esp_log_level_set("esp_eth", ESP_LOG_NONE);

    // SPI2 is the only general-purpose SPI host on the C6; SPI0/1 serve flash.
    ethPresent = ETH.begin(ETH_PHY_W5500, 1, W5500_CS, W5500_INT, W5500_RST,
                           SPI2_HOST, W5500_SCLK, W5500_MISO, W5500_MOSI, 20);

    esp_log_level_set("w5500.mac", ESP_LOG_ERROR);
    esp_log_level_set("esp_eth", ESP_LOG_ERROR);

    if (!ethPresent) {
        // ETH.begin() has no spi_bus_free() on its error path, so the bus would
        // stay initialised for a chip that is not there. Hand it back.
        spi_bus_free(SPI2_HOST);
        Serial.println("Ethernet: no W5500 on the FPC header - WiFi only");
        return;
    }
    // The Arduino ETH class never sets one, so without this the lease shows
    // lwIP's "espressif". Set before the link comes up, i.e. before the first
    // DHCP DISCOVER goes out.
    ETH.setHostname(deviceHostname());
    ETH.setRoutePrio(ETH_ROUTE_PRIO);
    bool ownMac = applyBuswareEthMac();
    Serial.printf("Ethernet: W5500 detected, MAC %s (%s)\n",
                  ETH.macAddress().c_str(),
                  ownMac ? "busware eFuse" : "derived");
}
#endif // W5500_ETH

static bool     wifiOffForEth = false; // radio parked because the cable carries us
static uint32_t wifiResumedAt = 0;     // millis() of the last resume, 0 = none

// Watchdog state lives below loop(); resume needs to prime it.
extern bool     wasConnected;
extern uint32_t wifiDownSince;
extern uint32_t lastReconnectKick;

#include <esp_mac.h>   // esp_read_mac() — needed on every target, not just W5500 ones

// The device's station MAC. WiFi.macAddress() asks the (possibly powered-down)
// radio and answers 00:00:00:00:00:00 once it is parked; the address itself is
// a chip property, so read it from efuse and keep reporting it either way.
static String deviceMacString() {
    uint8_t m[6] = {0};
    if (esp_read_mac(m, ESP_MAC_WIFI_STA) != ESP_OK) {
        return WiFi.macAddress();
    }
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(buf);
}

// Radio off, credentials kept. The pair to wifiRadioResume().
static void wifiRadioPark(const char* why) {
    if (wifiOffForEth) return;
    Serial.printf("WiFi: radio off (%s)\n", why);
    WiFi.disconnect(true /*wifioff*/, false /*keep stored credentials*/);
    wifiOffForEth = true;
    wifiResumedAt = 0;
}

// Bring the radio back up after the cable was the only interface. Used when
// the link drops and when the web UI needs to scan or re-provision WiFi.
//
// autoConnect=false powers the radio without starting a connection. A caller
// that is about to call WiFi.begin(ssid, pass) itself MUST use it: begin()
// here would leave the STA in "connecting", and esp_wifi_set_config then
// refuses the new credentials ("sta is connecting, cannot set config") — the
// web UI would report success and reboot into the old network.
static void wifiRadioResume(const char* why, bool autoConnect = true) {
    if (!wifiOffForEth) return;
    wifiOffForEth = false;
    wifiResumedAt = millis();
    Serial.printf("WiFi: radio back on (%s)\n", why);
    // Prime the watchdog to "down, just started": the first sample then logs
    // the up-transition instead of a spurious "link down" warning.
    wasConnected      = false;
    wifiDownSince     = 0;
    lastReconnectKick = 0;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_NONE);   // C6 rule: before every begin()
    WiFi.setAutoReconnect(true);
    if (autoConnect) {
        WiFi.begin();              // stored credentials, if any
    }
}

AsyncWebServer server(80);
bool improvConnected = false;
#ifndef DISABLE_IMPROV
ImprovWiFi improvSerial(&Serial);

void onImprovWiFiErrorCb(ImprovTypes::Error err) {
    Serial.printf("Improv error: %d\n", err);
}

void onImprovWiFiConnectedCb(const char *ssid, const char *password) {
    Serial.printf("Improv Connected! SSID: %s\n", ssid);
    improvConnected = true;
}
#endif

uint32_t bootTime = 0;
bool isApMode = false;
// True only for the *involuntary* fallback AP (boot window expired with no
// WiFi). Onboarding (no creds) and button-started APs leave this false: they
// are deliberate and must not be auto-exited when a stale STA re-associates.
bool apModeIsFallback = false;
// Authorizes /api/wifi/connect while in AP mode. True for onboarding (no creds)
// and button-started APs; false for the involuntary fallback AP, where the open
// AP + stored creds would otherwise let an RF-range attacker relocate the device
// onto their own network. A physical button long-press re-authorizes it (proves
// presence) so a moved device can still be reprovisioned via the captive portal.
bool apConnectAuthorized = false;
bool pendingReboot = false;
uint32_t rebootTime = 0;
uint32_t buttonPressStart = 0;
bool buttonState = HIGH;

// Prog-button debounce. 30 ms is far above any electrical disturbance on the
// shared RXD0 line and far below a human press.
#define BTN_DEBOUNCE_MS   30UL
#define BTN_PRESS_MIN_MS  50UL
DNSServer dnsServer;
const byte DNS_PORT = 53;

// M5: outcome of the NCN self-test, surfaced in /api/status so a FAIL is
// visible beyond the serial console.
//
// This is no longer a boot snapshot: the TPUart stack retries the handshake for
// as long as it is uninitialized, so a stick that started without bus power
// connects on its own once the bus is live — and the status the user sees has to
// follow that. loop() refreshes the code once a second; the async_tcp status
// handler turns it into text. Kept as a byte rather than a char[40] precisely
// because of that cross-task access: a byte store is atomic on RV32, a 40-char
// copy read concurrently by the web task is not.
enum NcnSelfTest : uint8_t {
    NCN_ST_PENDING = 0,
    NCN_ST_OK,
    NCN_ST_OK_NO_VBUS,
    NCN_ST_NO_UART,
    NCN_ST_NO_DL,
};
volatile uint8_t ncnSelfTest = NCN_ST_PENDING;

static const char* ncnSelfTestText(uint8_t st) {
    switch (st) {
        case NCN_ST_OK:         return "OK";
        case NCN_ST_OK_NO_VBUS: return "OK (no VBUS!)";
        case NCN_ST_NO_UART:    return "FAIL (no NCN UART response)";
        case NCN_ST_NO_DL:      return "FAIL (no DL layer)";
        default:                return "pending";
    }
}

// ACR0 readback. A VDD2 bit reading 0 is ambiguous: either the DC2 regulator is
// genuinely out of range, or DC2EN in ACR0 is simply off — and ACR0 survives a
// host reset, so a stuck bit looks exactly like broken hardware across reboots.
// Reading ACR0 back tells the two apart. The falling edge triggers a read on its
// own, because the interesting moment rarely coincides with someone watching.
// Single-byte/bool state, written by loop() and read by the async_tcp task:
// aligned 8/32-bit loads are atomic on RV32, no tearing.
volatile bool     acr0ReadRequested = false;   // a read is wanted
volatile bool     acr0FaultPending  = false;   // the pending read belongs to a VDD2 edge
volatile bool     acr0HaveReading   = false;
volatile uint8_t  acr0Value         = 0;
volatile unsigned long acr0ReadAtMs = 0;
// Kept apart from the rolling reading so a later manual or boot read cannot
// overwrite the one taken when VDD2 actually dropped.
volatile bool     acr0HaveFault     = false;
volatile uint8_t  acr0FaultValue    = 0;
volatile unsigned long acr0FaultAtMs = 0;
// A fault reading is only believed once two consecutive reads agree. The answer
// to a register read carries no marker of its own, so a single reading can in
// principle be some other byte that arrived first — and a wrong value frozen
// into fault_capture would be worse than no value at all, because it reads as
// hard evidence.
#define ACR0_FAULT_MAX_ATTEMPTS 12
volatile bool     acr0FaultHaveFirst  = false;
volatile uint8_t  acr0FaultFirstValue = 0;
volatile uint8_t  acr0FaultAttempts   = 0;

// Render the ACR0 block for /api/status. Split out so the "no data link layer"
// branch reports the same shape instead of omitting the key.
static String acr0StatusJson(unsigned int timeouts) {
    String j = "{\"valid\":";
    j += acr0HaveReading ? "true" : "false";
    if (acr0HaveReading) {
        const uint8_t v = acr0Value;
        char hex[8];
        snprintf(hex, sizeof(hex), "0x%02X", v);
        j += ",\"hex\":\"" + String(hex) + "\"";
        j += ",\"value\":" + String(v);
        j += ",\"dc2en\":"      + String((v & ACR0_FLAG_DC2EN)      ? "true" : "false");
        j += ",\"v20ven\":"     + String((v & ACR0_FLAG_V20VEN)     ? "true" : "false");
        j += ",\"xclken\":"     + String((v & ACR0_FLAG_XCLKEN)     ? "true" : "false");
        j += ",\"v20vclimit\":" + String(v & ACR0_MASK_V20VCLIMIT);
        j += ",\"trigen\":"     + String((v & ACR0_FLAG_TRIGEN)     ? "true" : "false");
        j += ",\"is_reset_value\":" + String(v == ACR0_RESET_VALUE ? "true" : "false");
        j += ",\"age_ms\":" + String((unsigned long)(millis() - acr0ReadAtMs));
    }
    j += ",\"timeouts\":" + String(timeouts);
    // The reading taken while VDD2 was low, kept for as long as the device runs.
    j += ",\"fault_capture\":";
    if (acr0HaveFault) {
        char fhex[8];
        snprintf(fhex, sizeof(fhex), "0x%02X", acr0FaultValue);
        j += "{\"hex\":\"" + String(fhex) + "\"";
        j += ",\"dc2en\":" + String((acr0FaultValue & ACR0_FLAG_DC2EN) ? "true" : "false");
        j += ",\"age_ms\":" + String((unsigned long)(millis() - acr0FaultAtMs)) + "}";
    } else {
        j += "null";
    }
    j += "}";
    return j;
}

#ifdef NCN_ACR0_WRITE_TEST
// Bench only, never in a shipped build: writing ACR0 can switch DC2 or the V20V
// regulator off, and the register survives a host reset. Every write therefore
// schedules its own restore in loop(), so the board recovers even if the write
// takes the network with it and no second request can be sent.
volatile bool     acr0WritePending  = false;
volatile uint8_t  acr0WriteValue    = 0;
volatile uint8_t  acr0RestoreValue  = ACR0_RESET_VALUE;
volatile unsigned long acr0RestoreAt = 0;   // millis deadline, 0 = nothing scheduled
#endif

// H2: KNX progMode is mutated only from loop() (the main task). Web handlers run
// in the async_tcp task; toggling the stack from there races knx.loop(). The
// /api/progmode handler sets these and loop() applies the change.
volatile bool progModeReqPending = false;
volatile bool progModeReqValue   = false;

// ESP-IDF private brownout-disable (no public Arduino header for ESP32-C6)
extern "C" void esp_brownout_disable(void);

// ============================================================================
// Online-Update — pulls firmware over HTTPS from install.busware.de/ip4knx/.
// Manifest contains version + per-chip { path, md5 }. setInsecure() because
// pinning a Let's-Encrypt root rotates faster than firmware does; MD5 from the
// manifest provides the integrity check that TLS cert validation would.
// ============================================================================
static const char* UPDATE_MANIFEST_URL = "https://install.busware.de/ip4knx/manifest.json";

#if defined(CONFIG_IDF_TARGET_ESP32C3)
  #define UPDATE_CHIP_KEY "ESP32-C3"
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  #define UPDATE_CHIP_KEY "ESP32-C6"
#else
  #define UPDATE_CHIP_KEY "ESP32"
#endif

enum UpdateState { UPD_IDLE, UPD_CHECKING, UPD_AVAILABLE, UPD_INSTALLING, UPD_DONE, UPD_ERROR };

struct UpdateInfo {
    // state/progress/total are read by /api/update/status (async_tcp task) while
    // updateInstallTask (separate task) writes them -> volatile so the reader
    // never caches a stale value. 32-bit aligned scalars are atomic on RISC-V.
    volatile UpdateState state = UPD_IDLE;
    // latestVersion/url/md5 are written by the ota_check task (doUpdateCheck) but
    // read from other tasks: latestVersion by the status handler and url/md5 by
    // the install pre-check, both on async_tcp. Fixed char[] (not String) so a
    // concurrent read cannot land on a realloc'd/freed String buffer (a
    // use-after-free); worst case is a torn read that self-corrects on the next
    // poll — same rationale as error[] below. Written via strlcpy.
    char latestVersion[24] = {0};
    char url[192] = {0};
    char md5[33] = {0};
    // error is written by updateInstallTask and read by the status handler in a
    // different task. A String there is a use-after-free (realloc frees the
    // reader's buffer); a fixed char[] degrades the worst case to a harmless
    // torn read that self-corrects on the next poll. Write via setUpdateError().
    char error[96] = {0};
    volatile size_t progress = 0;
    volatile size_t total = 0;
    uint32_t lastCheckMillis = 0;
};
static UpdateInfo updateInfo;

static void setUpdateError(const String& s) {
    strlcpy(updateInfo.error, s.c_str(), sizeof(updateInfo.error));
}

static const char* updateStateName(UpdateState s) {
    switch (s) {
        case UPD_IDLE:       return "idle";
        case UPD_CHECKING:   return "checking";
        case UPD_AVAILABLE:  return "available";
        case UPD_INSTALLING: return "installing";
        case UPD_DONE:       return "done";
        case UPD_ERROR:      return "error";
    }
    return "?";
}

// Compare MAJOR.MINOR.BUILD numerically. Returns >0 if a > b.
static int versionCompare(const String& a, const String& b) {
    int aM=0, an=0, ap=0, bM=0, bn=0, bp=0;
    sscanf(a.c_str(), "%d.%d.%d", &aM, &an, &ap);
    sscanf(b.c_str(), "%d.%d.%d", &bM, &bn, &bp);
    if (aM != bM) return aM - bM;
    if (an != bn) return an - bn;
    return ap - bp;
}

// Naive JSON string extractor — adequate for our flat, well-formed manifest.
static String jsonGetStr(const String& s, const String& key) {
    String pat = "\"" + key + "\"";
    int k = s.indexOf(pat);
    if (k < 0) return "";
    int colon = s.indexOf(':', k);
    if (colon < 0) return "";
    int q1 = s.indexOf('"', colon);
    if (q1 < 0) return "";
    int q2 = s.indexOf('"', q1 + 1);
    if (q2 < 0) return "";
    return s.substring(q1 + 1, q2);
}

// Look up "ota":{ "<UPDATE_CHIP_KEY>":{ <field>: "..." } } so we don't collide
// with the same chip-family string appearing inside builds[].
static String jsonGetOtaField(const String& body, const String& field) {
    int ota = body.indexOf("\"ota\"");
    if (ota < 0) return "";
    String chipPat = String("\"") + UPDATE_CHIP_KEY + "\"";
    int chip = body.indexOf(chipPat, ota);
    if (chip < 0) return "";
    int objStart = body.indexOf('{', chip);
    int objEnd = body.indexOf('}', objStart);
    if (objStart < 0 || objEnd < 0) return "";
    return jsonGetStr(body.substring(objStart, objEnd + 1), field);
}

// Escape a string for safe inclusion as a JSON string value. SSIDs in range and
// KNX-bus-derived strings are attacker-influenced; an unescaped " or \ corrupts
// the JSON (provisioning DoS) or injects keys. (M1)
static String jsonEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// --- C2 mitigation: cross-origin (CSRF) + AP-mode gate for mutating routes ---
// A malicious web page opened in a browser on the LAN can fire cross-site form
// POSTs at our endpoints through any NAT/firewall — the victim's browser is the
// confused deputy. Browsers attach an Origin header to every cross-origin POST;
// our own UI is same-origin (Origin matches Host) and non-browser clients
// (curl, scripts) send none. Policy: no Origin → allow; Origin host equal to
// the Host header → allow; anything else (including "null") → 403.
static String urlHostOnly(String s) {
    int p = s.indexOf("://");
    if (p >= 0) s = s.substring(p + 3);
    p = s.indexOf('/');
    if (p >= 0) s = s.substring(0, p);
    if (s.startsWith("[")) {            // bracketed IPv6 literal: keep [..]
        p = s.indexOf(']');
        if (p >= 0) s = s.substring(0, p + 1);
    } else {                            // strip :port
        p = s.indexOf(':');
        if (p >= 0) s = s.substring(0, p);
    }
    s.toLowerCase();
    return s;
}

static bool originAllowed(AsyncWebServerRequest *request) {
    if (!request->hasHeader("Origin")) return true;
    return urlHostOnly(request->header("Origin")) == urlHostOnly(request->host());
}

// DNS-rebinding defense. originAllowed() alone is bypassable: a rebinding
// attacker controls both Origin and Host (attacker.com resolves to the device
// IP, so a victim's browser sends Origin: attacker.com AND Host: attacker.com —
// they match). Pin Host to the device's own identities instead; a browser
// cannot forge Host to the device's LAN IP / mDNS name from an attacker-served
// page. Empty Host (odd non-browser client) is allowed — a browser rebinding
// request always carries a Host, and the Origin gate still applies.
static bool hostAllowed(AsyncWebServerRequest *request) {
    String h = urlHostOnly(request->host());   // lowercased, port stripped
    if (h.length() == 0) return true;
    if (h == "tul.local" || h == "tul") return true;
    if (h == WiFi.localIP().toString()) return true;
#ifdef W5500_ETH
    if (ethPresent && h == ETH.localIP().toString()) return true;
#endif
    if (isApMode && h == WiFi.softAPIP().toString()) return true;
    return false;
}

// Gate for state-changing endpoints. Sends the 403 itself; the caller just
// returns. The provisioning AP is OPEN (no PSK) — anyone in RF range can join.
// While it is active the web surface is onboarding-only: scan/connect/status
// work, everything else is locked. allowInApMode=true exempts the onboarding
// route itself (origin check still applies).
static bool mutationAllowed(AsyncWebServerRequest *request, bool allowInApMode = false) {
    if (!originAllowed(request)) {
        request->send(403, "application/json", "{\"error\":\"cross-origin request rejected\"}");
        return false;
    }
    // Pin Host for normal operation. Skip it for the AP-mode onboarding route:
    // the captive portal is reached via hijacked hostnames (dnsServer resolves
    // * -> softAP IP) so its Host is not the device IP, and the AP threat model
    // is RF proximity, not remote DNS rebinding.
    if (!(isApMode && allowInApMode) && !hostAllowed(request)) {
        request->send(403, "application/json", "{\"error\":\"host not recognized\"}");
        return false;
    }
    if (isApMode && !allowInApMode) {
        request->send(403, "application/json", "{\"error\":\"disabled in AP provisioning mode\"}");
        return false;
    }
    return true;
}

static bool doUpdateCheck() {
    updateInfo.state = UPD_CHECKING;
    setUpdateError("");
    updateInfo.lastCheckMillis = millis();

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    if (!https.begin(client, UPDATE_MANIFEST_URL)) {
        setUpdateError("HTTPS begin failed");
        updateInfo.state = UPD_ERROR;
        return false;
    }
    int code = https.GET();
    if (code != HTTP_CODE_OK) {
        setUpdateError("HTTP " + String(code));
        updateInfo.state = UPD_ERROR;
        https.end();
        return false;
    }
    String body = https.getString();
    https.end();

    String latest = jsonGetStr(body, "version");
    if (latest.length() == 0) {
        setUpdateError("no version in manifest");
        updateInfo.state = UPD_ERROR;
        return false;
    }
    strlcpy(updateInfo.latestVersion, latest.c_str(), sizeof(updateInfo.latestVersion));

    String path = jsonGetOtaField(body, "path");
    String md5  = jsonGetOtaField(body, "md5");
    if (path.length() == 0 || md5.length() != 32) {
        setUpdateError("no ota entry for " UPDATE_CHIP_KEY);
        updateInfo.state = UPD_ERROR;
        return false;
    }
    String base = String(UPDATE_MANIFEST_URL);
    int lastSlash = base.lastIndexOf('/');
    String fullUrl = base.substring(0, lastSlash + 1) + path;
    strlcpy(updateInfo.url, fullUrl.c_str(), sizeof(updateInfo.url));
    strlcpy(updateInfo.md5, md5.c_str(), sizeof(updateInfo.md5));

    if (versionCompare(latest, FIRMWARE_VERSION) > 0) {
        updateInfo.state = UPD_AVAILABLE;
    } else {
        updateInfo.state = UPD_IDLE;
    }
    Serial.printf("Update check: current=%s latest=%s state=%s\n",
                  FIRMWARE_VERSION, latest.c_str(), updateStateName(updateInfo.state));
    return true;
}

// doUpdateCheck() does a blocking HTTPS GET (seconds). Running it directly from
// the /api/update/check handler would freeze the async_tcp task — the same
// class of bug fixed for WiFi.scanNetworks() (H3). Offload to its own task; the
// frontend polls /api/update/status while state == "checking".
static void updateCheckTask(void* arg) {
    doUpdateCheck();
    vTaskDelete(NULL);
}

static bool kickOffUpdateCheck() {
    if (updateInfo.state == UPD_CHECKING || updateInfo.state == UPD_INSTALLING)
        return false;
    updateInfo.state = UPD_CHECKING;
    setUpdateError("");
    BaseType_t ok = xTaskCreate(updateCheckTask, "ota_check", 8192, NULL, 1, NULL);
    if (ok != pdPASS) {
        setUpdateError("check task spawn failed");
        updateInfo.state = UPD_ERROR;
        return false;
    }
    return true;
}

static void updateInstallTask(void* arg) {
    updateInfo.state = UPD_INSTALLING;
    setUpdateError("");
    updateInfo.progress = 0;
    updateInfo.total = 0;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    if (!https.begin(client, updateInfo.url)) {
        setUpdateError("HTTPS begin failed");
        updateInfo.state = UPD_ERROR;
        vTaskDelete(NULL);
        return;
    }
    int code = https.GET();
    if (code != HTTP_CODE_OK) {
        setUpdateError("HTTP " + String(code));
        updateInfo.state = UPD_ERROR;
        https.end();
        vTaskDelete(NULL);
        return;
    }
    int len = https.getSize();
    updateInfo.total = (len > 0) ? (size_t)len : 0;

    WiFiClient* stream = https.getStreamPtr();
    if (!Update.begin(len > 0 ? (size_t)len : UPDATE_SIZE_UNKNOWN)) {
        setUpdateError(String("Update.begin: ") + Update.errorString());
        updateInfo.state = UPD_ERROR;
        https.end();
        vTaskDelete(NULL);
        return;
    }
    if (!Update.setMD5(updateInfo.md5)) {
        setUpdateError("setMD5 rejected (bad format)");
        updateInfo.state = UPD_ERROR;
        Update.abort();
        https.end();
        vTaskDelete(NULL);
        return;
    }

    Update.onProgress([](size_t progress, size_t total) {
        updateInfo.progress = progress;
        if (total != 0 && total != (size_t)-1) {
            updateInfo.total = total;
        }
    });

    // Manual fetch loop bypasses Update.writeStream() — which calls
    // data.peek() before the read loop. On some ESP32-Arduino +
    // WiFiClientSecure versions that peek can desync the stream by one
    // byte, producing an MD5 mismatch even though all bytes appear to
    // be received. Reading explicitly with stream->readBytes() into our
    // own buffer + Update.write() avoids that path entirely. Also lets
    // us print MD5 telemetry on failure.
    uint8_t* buf = (uint8_t*)malloc(2048);
    if (!buf) {
        setUpdateError("malloc(2048) failed");
        updateInfo.state = UPD_ERROR;
        Update.abort();
        https.end();
        vTaskDelete(NULL);
        return;
    }
    size_t written = 0;
    int timeout_failures = 0;
    while ((int)written < len) {
        int toRead = (int)((len - written) < 2048 ? (len - written) : 2048);
        int got = stream->readBytes((char*)buf, toRead);
        if (got <= 0) {
            if (++timeout_failures >= 300) {
                setUpdateError("stream read timeout");
                free(buf);
                Update.abort();
                https.end();
                updateInfo.state = UPD_ERROR;
                vTaskDelete(NULL);
                return;
            }
            delay(100);
            continue;
        }
        timeout_failures = 0;
        size_t w = Update.write(buf, (size_t)got);
        if (w != (size_t)got) {
            setUpdateError(String("Update.write short: ") + Update.errorString());
            free(buf);
            Update.abort();
            https.end();
            updateInfo.state = UPD_ERROR;
            vTaskDelete(NULL);
            return;
        }
        written += got;
    }
    free(buf);
    updateInfo.progress = written;

    if (!Update.end(true)) {
        setUpdateError(String("Update.end: ") + Update.errorString());
        updateInfo.state = UPD_ERROR;
        https.end();
        vTaskDelete(NULL);
        return;
    }
    https.end();
    updateInfo.state = UPD_DONE;
    Serial.printf("Online-OTA: %u bytes written, MD5 ok — rebooting\n", (unsigned)written);
    delay(2000);  // let the client poll /status once more
    ESP.restart();
}

static bool kickOffUpdateInstall() {
    if (updateInfo.state == UPD_INSTALLING) return false;
    if (updateInfo.url[0] == '\0' || strlen(updateInfo.md5) != 32) {
        setUpdateError("no pending update — run /api/update/check first");
        updateInfo.state = UPD_ERROR;
        return false;
    }
    // Set state BEFORE spawning the task so the HTTP response we're about to
    // send already reflects "installing" — otherwise there's a race where the
    // POST handler returns state="available" before the new task gets to set
    // it, and the frontend never starts polling.
    updateInfo.state = UPD_INSTALLING;
    setUpdateError("");
    updateInfo.progress = 0;
    updateInfo.total = 0;
    BaseType_t ok = xTaskCreate(updateInstallTask, "ota_install", 8192, NULL, 1, NULL);
    if (ok != pdPASS) {
        setUpdateError("task spawn failed");
        updateInfo.state = UPD_ERROR;
        return false;
    }
    return true;
}

static String updateStatusJson() {
    String j = "{";
    j += "\"state\":\"" + String(updateStateName(updateInfo.state)) + "\",";
    j += "\"current\":\"" + String(FIRMWARE_VERSION) + "\",";
    j += "\"latest\":\"" + String(updateInfo.latestVersion) + "\",";
    j += "\"available\":" + String(updateInfo.state == UPD_AVAILABLE ? "true" : "false") + ",";
    j += "\"progress\":" + String((unsigned)updateInfo.progress) + ",";
    j += "\"total\":" + String((unsigned)updateInfo.total) + ",";
    j += "\"error\":\"" + jsonEscape(String(updateInfo.error)) + "\"";
    j += "}";
    return j;
}

// Debounced prog-button service, shared by the boot wait loop and loop().
// Returns the level to use for long-press (AP) logic and handles the short
// press itself — see setup() for why this is polled instead of interrupt-driven.
// Callers must assign the returned value to buttonState at the end of their
// iteration, as the existing AP logic already does.
static bool serviceProgButton() {
    static bool     raw      = HIGH;   // last raw sample
    static uint32_t rawSince = 0;      // when the raw level last changed
    static uint32_t rejected = 0;      // flickers that never stabilised
    static uint32_t reportAt = 0;

    bool now = digitalRead(KNX_BUTTON);
    if (now != raw) {
        raw = now;
        rawSince = millis();
        if (now != buttonState) rejected++;
    }
    // Report a noisy line instead of silently filtering it. On a stick whose
    // RXD0 is quiet this never prints.
    if (rejected > 0 && millis() - reportAt > 60000) {
        reportAt = millis();
        Serial.printf("Button: %lu unstable edges on GPIO9 in the last minute (ignored)\n",
                      (unsigned long)rejected);
        rejected = 0;
    }

    bool level = buttonState;
    if (raw != buttonState && millis() - rawSince >= BTN_DEBOUNCE_MS) {
        level = raw;
    }
    if (level == HIGH && buttonState == LOW) {
        // Released. A short, clean press is the programming-mode toggle; long
        // presses belong to the AP logic and must not also toggle.
        uint32_t held = millis() - buttonPressStart;
        if (held >= BTN_PRESS_MIN_MS && held < 2000) {
            Serial.println("Button: short press - toggling KNX programming mode");
            knx.toggleProgMode();
        }
    }
    return level;
}

void setup() {
    // Bus-powered TULX32 hat <40 mA Bus-Strom-Budget (R6=10kΩ FANIN auf
    // NCN5130). 80 MHz CPU ist Kompromiss: 50% Strom-Reduktion vs 160 MHz
    // Default, aber UART-Baudrate-Init bleibt zuverlässig.
    setCpuFrequencyMhz(80);

    // Brownout-Detector deaktivieren: bus-powered V20-Pfad hat enge
    // Toleranzen, kurze WiFi-Init-Bursts können die VDD-Schiene unter
    // 2.51V (BOD-Default) drücken ohne dass die CPU tatsächlich Probleme
    // hat. BOD off → CPU läuft auch bei 2.3V VDD weiter, App-Logik wird
    // nicht durch transienten Spannungs-Dip neu gestartet. Erfordert dass
    // C13 (3.3V-Rail-Buffer) groß genug bemessen ist.
    // NUR für die bus-powered TULX32 (L4): auf USB-versorgten TUL/TUL32 bleibt
    // der BOD aktiv — dort gibt es keinen Strom-Engpass, und ein echter
    // Spannungseinbruch soll Flash-Writes weiterhin schützen.
#ifdef TULX32_BUSPOWERED
    esp_brownout_disable();
#endif

    Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.setTxTimeoutMs(10);  // bound HWCDC write-stall when USB host is gone
#endif

#ifndef DISABLE_IMPROV
    // Setup Improv IMMEDIATELY - ESP Web Tools has only ~2s to detect it
    // after opening the serial port (which resets the device via USB-JTAG)
    improvSerial.onImprovConnected(onImprovWiFiConnectedCb);
    improvSerial.onImprovError(onImprovWiFiErrorCb);
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    improvSerial.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32_C3, "TUL KNX/IP Gateway", FIRMWARE_VERSION, "TUL Gateway");
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    improvSerial.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32_C6, "TUL32 KNX/IP Gateway", FIRMWARE_VERSION, "TUL32 Gateway");
#else
    improvSerial.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32, "TUL KNX/IP Gateway", FIRMWARE_VERSION, "TUL Gateway");
#endif

    // Handle Improv during early boot - ESP Web Tools sends commands ~2s after port open
    for (int i = 0; i < 200; i++) {
        improvSerial.handleSerial();
        delay(10);
    }
#endif

    Serial.println("Starting TUL KNX/IP Gateway");
    ArduinoPlatform::SerialDebug = &Serial;

    pinMode(KNX_LED, OUTPUT);
    digitalWrite(KNX_LED, HIGH); // Active low

    // Initialize NVS - required for WiFi and Improv
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("NVS: erasing and re-initializing...");
        nvs_flash_erase();
        nvs_flash_init();
    }
    Serial.print("NVS init: ");
    Serial.println(err == ESP_OK ? "OK" : "FAILED");

    bootTime = millis();

    // Bus-powered: lass C13 (330µF @ 3.3V) und C6 (270µF @ VFILT/28V) sich
    // vor dem WiFi-RF-Cal-Burst voll laden. WiFi.mode(WIFI_STA) initialisiert
    // intern das RF-Subsystem und zieht dabei ~150-200mA für ein paar ms,
    // was DC1+VFILT-Tank überfordert wenn die Caps nicht voll sind.
    // Bei FANIN R=10kΩ → 40mA Bus-Limit braucht VFILT+C13 ca. 400ms zum
    // vollen Laden; mit Headroom warten wir 800ms in idle (40MHz CPU,
    // ESP-active-Strom <15mA → Tank lädt parallel).
    Serial.println("Cap charge wait: 1500ms");
    delay(1500);

#ifdef W5500_ETH
    // Probe before WiFi: a cabled TUL32 then already has its link (and its
    // route priority) in place when the KNX stack joins the routing group.
    initEthernet();
#endif

    // === WiFi-Init mit Markern + TX-Power-Reduktion VOR mode() ===
    // Bei bus-powered Setup (V20 → DC1 → 3.3V, 40mA Bus-Limit) ist der
    // RF-Cal-Burst beim WiFi-Hardware-Init kritisch. setTxPower() vor
    // mode() konfiguriert die PHY-Tabelle bevor die RF aktiv wird.

    Serial.println("[A] WiFi.persistent(true)");
    Serial.flush();
    delay(50);
    WiFi.persistent(true);

    Serial.println("[B] WiFi.setTxPower BEFORE mode()");
    Serial.flush();
    delay(50);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    // Global default for every netif created from here on; the STA netif reads
    // it when WiFi.mode() creates it, so this has to come first.
    WiFi.setHostname(deviceHostname());
    Serial.println("[C] WiFi.mode(WIFI_STA) — RF init, biggest burst");
    Serial.flush();
    delay(200);
    WiFi.mode(WIFI_STA);

    Serial.println("[D] WiFi.mode survived");
    Serial.flush();
    delay(50);

    Serial.println("[E] WiFi.setSleep(WIFI_PS_NONE)");
    Serial.flush();
    delay(50);
    // PS_NONE is mandatory on ESP32-C6 (and used on C3 too): WiFi-6 modem-sleep
    // defaults (WIFI_PS_MIN/MAX_MODEM) break the WPA2 4-way handshake on the C6
    // (reproducible 4WAY_HANDSHAKE_TIMEOUT / ASSOC_LEAVE reason 8) and drop KNX
    // multicast/broadcast routing telegrams during DTIM sleep windows. Must be
    // set before every WiFi.begin().
    WiFi.setSleep(WIFI_PS_NONE);

    // Belt-and-suspenders: arm the core STA auto-reconnect. The loop() watchdog
    // below is the real recovery path (handles silent drops + lost DHCP leases),
    // but this covers the plain clean-disassoc case without waiting for a grace
    // window.
    WiFi.setAutoReconnect(true);

    Serial.println("[F] read SSID/PSK");
    Serial.flush();
    delay(50);
    bool hasCredentials = WiFi.psk().length() > 0 || WiFi.SSID().length() > 0;
    Serial.print("WiFi SSID length: ");
    Serial.println(WiFi.SSID().length());
    Serial.print("WiFi PSK length: ");
    Serial.println(WiFi.psk().length());
    Serial.println("[G] WiFi early-init done");

    const uint32_t improvWindowMs = 120000;  // 120 seconds

    bool ethCarries = false;
#ifdef W5500_ETH
    // Decide before touching the radio, or the stick associates first and gets
    // its WiFi taken away a moment later. The W5500 does not report a link the
    // instant the driver is up, so give it a window: a short one for the PHY,
    // and only if a cable is actually there the longer one for DHCP.
    if (ethPresent) {
        uint32_t t0 = millis();
        while (!ETH.linkUp() && millis() - t0 < ETH_LINK_WAIT_MS) {
            delay(50);
        }
        if (ETH.linkUp()) {
            Serial.printf("Ethernet: link after %lu ms, waiting for DHCP\n",
                          (unsigned long)(millis() - t0));
            while (!knxUseEthernet() && millis() - t0 < ETH_LINK_WAIT_MS + ETH_DHCP_WAIT_MS) {
                delay(50);
            }
            Serial.printf("Ethernet: %s after %lu ms\n",
                          knxUseEthernet() ? "ready" : "no IPv4",
                          (unsigned long)(millis() - t0));
        }
    }
    ethCarries = knxUseEthernet();
#endif

    if (ethCarries) {
        // Cable wins, so the radio is parked: two addresses in one subnet make
        // the source address of KNX's INADDR_ANY socket a matter of lwIP netif
        // order rather than of route priority, and a device that is reachable
        // over the wire has no business opening an unprotected onboarding AP.
        // Credentials stay in NVS; wifiRadioResume() brings it back.
#ifdef W5500_ETH
        Serial.printf("Ethernet carries the gateway (%s)\n",
                      ETH.localIP().toString().c_str());
#endif
        wifiRadioPark("cable carries the gateway at boot");
    } else if (!hasCredentials) {
        Serial.println("No WiFi credentials stored - Starting AP immediately!");
        // AP_STA so the captive portal can scan() while broadcasting.
        WiFi.mode(WIFI_AP_STA);
        WiFi.disconnect();
        String mac = WiFi.softAPmacAddress();
        mac.replace(":", "");
        String apName = "TUL AP " + mac.substring(mac.length() - 4);
        WiFi.softAP(apName.c_str());
        dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
        isApMode = true;
        apConnectAuthorized = true;   // onboarding: nothing to protect yet
        Serial.print("AP IP Address: ");
        Serial.println(WiFi.softAPIP());
    } else {
        Serial.println("WiFi credentials found, attempting auto-reconnect...");
        // setTxPower / setSleep wurden bereits oben vor mode() gesetzt.
        Serial.println("[H] WiFi.begin() — connect to AP");
        Serial.flush();
        delay(50);
        WiFi.begin();
        Serial.println("[I] WiFi.begin returned");
    }



    // === KNX Setup ===
    
    // Setup KNX Hardware Interface for TUL/TUL32
    // using UART_NUM_1 to leave UART_NUM_0 alone if needed
    auto knxInterface = new TPUart::Interface::ESP32(KNX_RX_PIN, KNX_TX_PIN, UART_NUM_1);
    knx.platform().interface(knxInterface);
    
    knx.ledPin(KNX_LED);
    knx.buttonPin(KNX_BUTTON);

    // The prog button is NOT a plain button on this hardware: GPIO9 sits on
    // RXD0 through 1 kΩ so a host can hold it low at power-on to enter the
    // bootloader. The stack would attachInterrupt(CHANGE) and decide on edge
    // *spacing* alone — buttonEvent() never reads the level, so any two edges
    // 50–500 ms apart toggle programming mode. On a C6 whose console runs over
    // USB-CDC that line is free to pick up noise, and the result is a prog mode
    // that flips several times a second. Poll a debounced level in loop()
    // instead; pinMode is ours to set once the stack skips its own setup.
    knx.setButtonISRFunction(nullptr);
    pinMode(KNX_BUTTON, INPUT_PULLUP);

    // Initialize EEPROM and read config
    knx.readMemory();

    if (knx.configured()) {
        Serial.println("KNX Device configured via ETS.");
    } else {
        Serial.println("KNX Device NOT configured. Awaiting ETS programming.");
    }

    knx.start();
    Serial.printf("KNX Gateway running! (Build %lu, Git %s)\n", (unsigned long)BUILD_NUMBER, BUILD_GIT);

    // === NCN5130 Boot Self-Test ===
    // Pump knx.loop() so the stack completes tryInitialize() (U_RESET_REQ →
    // U_RESET_IND, sets baud) and then issues U_STATE_REQ + U_SYSTEM_STATE_REQ
    // whose responses fill SystemState. Proves ESP↔NCN UART link is alive.
    Serial.println("\n=== NCN Self-Test ===");
    {
        unsigned long st0 = millis();
        while (millis() - st0 < 500) {
            knx.loop();
            delay(10);
        }
        auto tpDl = ((Bau091A&)knx.bau()).getSecondaryDataLinkLayer();
        if (!tpDl) {
            ncnSelfTest = NCN_ST_NO_DL;
            Serial.println("Result: FAIL (no DL layer)");
        } else {
            auto& tp = tpDl->getTPUart();
            auto& sys = tp.getSystemState();
            Serial.printf("State: %s\n", tp.getBcuStateInfo());
            if (tp.isConnected()) {
                Serial.printf("Baud : %u  Mode: %s\n", tp.getBaudrate(), sys.modeString());
                Serial.printf("Rails: V20V%c VDD2%c VBUS%c VFILT%c XTAL%c TW%c\n",
                              sys.v20v()  ? '+' : '-',
                              sys.vdd2()  ? '+' : '-',
                              sys.vbus()  ? '+' : '-',
                              sys.vfilt() ? '+' : '-',
                              sys.xtal()  ? '+' : '-',
                              sys.thermalWarning() ? '!' : ' ');
                // Reference reading while the rails are healthy. A value taken
                // during a fault is only worth something next to a known-good
                // one, and boot is the one moment the line is reliably quiet.
                if (tp.requestInternalRegister(U_INT_REG_RD_REQ_ACR0)) {
                    unsigned long t0 = millis();
                    while (millis() - t0 < 100 && !tp.internalRegisterValid()) {
                        knx.loop();
                        delay(2);
                    }
                    if (tp.internalRegisterValid()) {
                        const uint8_t v = tp.internalRegisterValue();
                        acr0Value = v;
                        acr0ReadAtMs = tp.internalRegisterReadAt();
                        acr0HaveReading = true;
                        Serial.printf("ACR0 : 0x%02X (V20VEN%c DC2EN%c XCLKEN%c TRIGEN%c CLIMIT=%d)%s\n",
                                      v,
                                      (v & ACR0_FLAG_V20VEN) ? '+' : '-',
                                      (v & ACR0_FLAG_DC2EN)  ? '+' : '-',
                                      (v & ACR0_FLAG_XCLKEN) ? '+' : '-',
                                      (v & ACR0_FLAG_TRIGEN) ? '+' : '-',
                                      v & ACR0_MASK_V20VCLIMIT,
                                      v == ACR0_RESET_VALUE ? "  [reset value]" : "");
                    } else {
                        // Issued but unanswered — retry from loop(), same as a
                        // refused one, so the reference reading is not lost.
                        Serial.println("ACR0 : no answer within 100 ms, retrying from loop()");
                        acr0ReadRequested = true;
                    }
                } else {
                    // Busy line, quiet window, or link not connected yet — the
                    // loop() path retries, so the reference reading is not lost.
                    Serial.println("ACR0 : not issued yet, retrying from loop()");
                    acr0ReadRequested = true;
                }
                ncnSelfTest = sys.vbus() ? NCN_ST_OK : NCN_ST_OK_NO_VBUS;
                Serial.println(sys.vbus() ? "Result: OK" : "Result: OK (no VBUS!)");
            } else {
                ncnSelfTest = NCN_ST_NO_UART;
                Serial.println("Result: FAIL (no NCN UART response)");
                // Not a dead end any more: the stack keeps retrying the
                // handshake every 2 s, so plugging the bus in is enough.
                Serial.println("Hint: no KNX bus power? The gateway keeps retrying,");
                Serial.println("      connect the bus and it comes up without a reboot.");
            }
        }
    }
    Serial.println("======================\n");

#ifdef NCN_DC2_DIAG
    // Bench diagnosis only: does VDD2 react to DC2EN? The datasheet (NCN5130
    // p.19) makes DC2 optional — a board that does not need it ties VDD2MV to
    // VDD1 and DC2 can never report "in range". Switching DC2EN on tells the
    // two cases apart: if VDD2 goes high, DC2 was merely disabled; if it stays
    // low with VFILT set, this board does not run DC2 at all.
    {
        Serial.println("=== DC2 diagnosis ===");
        auto dl = ((Bau091A&)knx.bau()).getSecondaryDataLinkLayer();
        if (dl) {
            auto& tp = dl->getTPUart();
            auto& sys = tp.getSystemState();
            auto pump = [&](unsigned long ms) {
                unsigned long t0 = millis();
                while (millis() - t0 < ms) { knx.loop(); delay(5); }
            };
            // powerControl() reports false while the line is busy and writes
            // nothing at all in that case, so retry instead of assuming.
            auto call = [&](bool on) {
                for (int i = 0; i < 100; i++) {
                    if (dl->powerControl(on)) return true;
                    pump(20);
                }
                return false;
            };
            auto show = [&](const char *tag) {
                Serial.printf("%-16s ACR0=0x%02X%s | V20V=%d VDD2=%d VFILT=%d VBUS=%d mode=%s\n",
                              tag, tp.internalRegisterValue(),
                              tp.internalRegisterValid() ? "" : " (no fresh read)",
                              sys.v20v(), sys.vdd2(), sys.vfilt(), sys.vbus(), sys.modeString());
            };

            tp.requestInternalRegister(U_INT_REG_RD_REQ_ACR0);
            pump(300);
            show("as-found");

            bool offOk = call(false);   // clears DC2EN and V20VEN
            pump(2500);
            show(offOk ? "after off" : "off REFUSED");

            bool onOk = call(true);     // back to the reset value 0x74
            pump(2500);
            show(onOk ? "after on" : "on REFUSED");

            if (!onOk) {
                // Never leave the bench with the regulators off.
                Serial.println("!! restore failed, retrying until it takes");
                for (int i = 0; i < 200 && !dl->powerControl(true); i++) pump(50);
                pump(2000);
                show("after retry");
            }
        } else {
            Serial.println("no DL layer");
        }
        Serial.println("=====================\n");
    }
#endif

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        // Stream straight from PROGMEM via AsyncProgmemResponse. The const char*
        // overload routes to AsyncBasicResponse, which copies the whole 65 KB body
        // into a heap String and re-substrings it per ACK; on C3/C6 the post-first-
        // window reallocation fails on a fragmented heap and the page truncates at
        // ~one TCP window (GitHub #3). The (uint8_t*,len) overload memcpy_P's in
        // chunks with no large contiguous allocation.
        request->send(200, "text/html", (const uint8_t *)index_html, sizeof(index_html) - 1);
    });

    // Captive Portal Handlers
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request){
        request->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
    });
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request){
        request->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
    });
    server.on("/canonical.html", HTTP_GET, [](AsyncWebServerRequest *request){
        request->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
    });
    server.onNotFound([](AsyncWebServerRequest *request) {
        if (isApMode) {
            request->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
        } else {
            request->send(404, "text/plain", "Not found");
        }
    });

    // H3: async scan. WiFi.scanNetworks(true) returns immediately; results are
    // collected later via scanComplete(). A blocking WiFi.scanNetworks() here
    // would freeze the entire async_tcp task (all HTTP + TCP callbacks) for the
    // multi-second scan duration — an unauthenticated DoS. Protocol: GET ?start=1
    // kicks a fresh scan; plain GET polls. Returns {"scanning":true} while in
    // progress, else a JSON array of {ssid,rssi}. SSID is JSON-escaped (M1).
    server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request){
        // Reachable over the cable with the radio parked: switch it back on so
        // the user can still pick a network from the web UI.
        wifiRadioResume("web UI requested a scan");
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            request->send(200, "application/json", "{\"scanning\":true}");
            return;
        }
        if (request->hasParam("start") || n == WIFI_SCAN_FAILED) {
            WiFi.scanDelete();
            WiFi.scanNetworks(true /*async*/);
            request->send(200, "application/json", "{\"scanning\":true}");
            return;
        }
        String json = "[";
        for (int i = 0; i < n; ++i) {
            if (i > 0) json += ",";
            json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
        }
        json += "]";
        WiFi.scanDelete();
        request->send(200, "application/json", json);
    });

    server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *request){
        // No auto-connect: this handler sets the new credentials itself.
        wifiRadioResume("web UI is (re)configuring WiFi", false);
        // Onboarding route, but only exempt from the AP-mode lock when connect is
        // authorized (onboarding / button AP). In the involuntary fallback AP the
        // lock stays on so an RF-range attacker can't relocate the device; a
        // button long-press (physical presence) re-authorizes. STA mode is
        // unaffected (mutationAllowed only gates on isApMode).
        if (!mutationAllowed(request, apConnectAuthorized)) return;
        if(request->hasParam("ssid", true)) {
            String ssid = request->getParam("ssid", true)->value();
            String pass = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
            
            Serial.printf("Received WiFi config via Web Portal. SSID: %s\n", ssid.c_str());
            
            WiFi.persistent(true);
            WiFi.setSleep(WIFI_PS_NONE);   // C6: PS_NONE before every begin() (see setup)
            WiFi.begin(ssid.c_str(), pass.c_str());
            
            request->send(200, "application/json", "{\"status\":\"ok\"}");
            
            pendingReboot = true;
            rebootTime = millis();
        } else {
            request->send(400, "application/json", "{\"error\":\"missing ssid\"}");
        }
    });

    server.on("/api/wifi/ap_mode", HTTP_POST, [](AsyncWebServerRequest *request){
        // Locked in AP mode too: pointless there, and in *fallback* AP an RF
        // neighbor could otherwise wipe the stored credentials.
        if (!mutationAllowed(request)) return;
        Serial.println("Received request to clear WiFi credentials and start AP mode.");
        WiFi.disconnect(false, true); // Erase credentials
        request->send(200, "application/json", "{\"status\":\"ok\"}");
        pendingReboot = true;
        rebootTime = millis();
    });

    // OTA firmware upload. Multipart-form 'firmware' field carries the .bin.
    // Optional MD5 in header 'X-MD5' (32 hex chars, lowercase) — verified by
    // Update.h; on mismatch Update.end() returns false and the boot partition
    // is NOT switched, so a corrupt upload cannot brick the device.
    server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            // Gate first: the upload handler below already refused to start
            // Update for gated requests, so without this check the handler
            // would answer a rejected upload with 200/"ok".
            if (!mutationAllowed(request)) return;
            bool ok = !Update.hasError();
            String body = ok
                ? String("{\"status\":\"ok\"}")
                : String("{\"error\":\"") + Update.errorString() + "\"}";
            AsyncWebServerResponse *resp = request->beginResponse(ok ? 200 : 500, "application/json", body);
            resp->addHeader("Connection", "close");
            request->send(resp);
            if (ok) {
                Serial.println("OTA: success, scheduling reboot");
                pendingReboot = true;
                rebootTime = millis();
            }
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (index == 0) {
                // This body handler runs BEFORE the response handler above —
                // a gated request must never reach Update.begin(), otherwise
                // the image is already in flash when the 403 goes out. The
                // chunks below drain harmlessly (isRunning() stays false).
                if (!originAllowed(request) || isApMode) {
                    Serial.println("OTA: rejected (cross-origin or AP provisioning mode)");
                    return;
                }
                Serial.printf("OTA: upload start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.printError(Serial);
                    return;
                }
                if (request->hasHeader("X-MD5")) {
                    String md5 = request->header("X-MD5");
                    md5.trim();
                    md5.toLowerCase();
                    if (md5.length() == 32) {
                        if (!Update.setMD5(md5.c_str())) {
                            Serial.println("OTA: setMD5 rejected (bad format)");
                        } else {
                            Serial.printf("OTA: MD5 target = %s\n", md5.c_str());
                        }
                    } else {
                        Serial.printf("OTA: X-MD5 ignored (length=%u)\n", md5.length());
                    }
                } else {
                    Serial.println("OTA: no X-MD5 header — proceeding without checksum");
                }
            }
            // isRunning() guard: when begin() was never called (gated request)
            // the remaining chunks/final must no-op silently instead of
            // printError-spamming per chunk.
            if (len && Update.isRunning() && !Update.hasError()) {
                // M3: size cap. Update.begin(UPDATE_SIZE_UNKNOWN) set the limit
                // to the OTA partition's capacity; reject an oversized upload
                // early with a clean abort instead of letting it run until a
                // generic short-write error near the end.
                if ((index + len) > Update.size()) {
                    Serial.printf("OTA: image exceeds partition (%u > %u) — abort\n",
                                  (unsigned)(index + len), (unsigned)Update.size());
                    Update.abort();
                } else if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                }
            }
            if (final && Update.isRunning()) {
                if (!Update.end(true)) {
                    Update.printError(Serial);
                } else {
                    Serial.printf("OTA: %u bytes written, awaiting reboot\n", (unsigned)(index + len));
                }
            }
        }
    );

    // Online-Update: HTTPS-pull from install.busware.de/ip4knx/manifest.json.
    server.on("/api/update/check", HTTP_GET, [](AsyncWebServerRequest *request){
        // GET, but side-effecting (spawns an outbound HTTPS task), so gate it
        // like the mutating routes — otherwise a cross-origin page could churn
        // the update state / spam install.busware.de. Same-origin frontend GETs
        // send no Origin and carry the device's own Host, so they pass.
        if (!mutationAllowed(request)) return;
        // Async: spawn the blocking HTTPS check on its own task and return the
        // current status immediately (state="checking"). Frontend polls
        // /api/update/status until it flips to available/idle/error.
        kickOffUpdateCheck();
        request->send(200, "application/json", updateStatusJson());
    });
    server.on("/api/update/install", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!mutationAllowed(request)) return;
        bool ok = kickOffUpdateInstall();
        AsyncWebServerResponse *resp = request->beginResponse(ok ? 202 : 409,
            "application/json", updateStatusJson());
        request->send(resp);
    });
    server.on("/api/update/status", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "application/json", updateStatusJson());
    });

    // ProgMode toggle: accepts ?state=on|off|toggle (default: toggle).
    // Returns the new state after the operation.
    server.on("/api/progmode", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!mutationAllowed(request)) return;
        String state = "toggle";
        if (request->hasParam("state", true)) {
            state = request->getParam("state", true)->value();
        } else if (request->hasParam("state")) {
            state = request->getParam("state")->value();
        }
        bool newState;
        if (state == "on")        newState = true;
        else if (state == "off")  newState = false;
        else                      newState = !knx.progMode();
        // H2: don't mutate the KNX stack from the async_tcp task — defer the
        // write to loop(). The UI re-syncs to the real state via /api/status.
        progModeReqValue = newState;
        progModeReqPending = true;
        Serial.printf("ProgMode requested via web: %s (applied in loop)\n", newState ? "ON" : "OFF");
        String json = "{\"prog_mode\":";
        json += newState ? "true" : "false";
        json += "}";
        request->send(200, "application/json", json);
    });

    // Manual ACR0 readback. The read itself has to happen on the main task
    // (it touches the NCN link), so this only asks; /api/status carries the
    // answer one poll cycle later.
    server.on("/api/ncn/acr0", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!mutationAllowed(request)) return;
        acr0ReadRequested = true;
        Serial.println("ACR0 readback requested via web (applied in loop)");
        request->send(202, "application/json",
                      "{\"queued\":true,\"hint\":\"read acr0 from /api/status\"}");
    });

#ifdef NCN_ACR0_WRITE_TEST
    // Bench harness: write ACR0, then let loop() put it back. restore_ms is
    // capped and never optional — an unattended board must not be left with a
    // regulator switched off.
    // Deliberately not "/api/ncn/acr0/write": AsyncCallbackWebHandler treats a
    // registered URI as a prefix (url.startsWith(_uri + "/")), so the readback
    // handler above would answer this path instead — measured, not assumed.
    server.on("/api/ncn/acr0_write", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!mutationAllowed(request)) return;
        if (!request->hasParam("value", true) && !request->hasParam("value")) {
            request->send(400, "application/json", "{\"error\":\"value required\"}");
            return;
        }
        String vs = request->hasParam("value", true)
                        ? request->getParam("value", true)->value()
                        : request->getParam("value")->value();
        long v = strtol(vs.c_str(), nullptr, 0);
        if (v < 0 || v > 255) {
            request->send(400, "application/json", "{\"error\":\"value out of range\"}");
            return;
        }
        long restore = 10000;
        if (request->hasParam("restore_ms", true))
            restore = request->getParam("restore_ms", true)->value().toInt();
        else if (request->hasParam("restore_ms"))
            restore = request->getParam("restore_ms")->value().toInt();
        if (restore < 1000)  restore = 1000;
        if (restore > 60000) restore = 60000;

        // Restore to what is actually there now, falling back to the datasheet
        // reset value if nothing has been read back yet.
        acr0RestoreValue = acr0HaveReading ? acr0Value : ACR0_RESET_VALUE;
        acr0WriteValue = (uint8_t)v;
        acr0RestoreAt = millis() + (unsigned long)restore;
        acr0WritePending = true;
        Serial.printf("ACR0 write test requested: 0x%02X for %ld ms, restore to 0x%02X\n",
                      (uint8_t)v, restore, acr0RestoreValue);
        String j = "{\"queued\":true,\"value\":" + String(v) +
                   ",\"restore_ms\":" + String(restore) +
                   ",\"restore_value\":" + String(acr0RestoreValue) + "}";
        request->send(202, "application/json", j);
    });
#endif

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{";
        
        // uptime
        uint32_t secs = millis() / 1000;
        uint32_t d = secs / 86400;
        uint8_t h = (secs % 86400) / 3600;
        uint8_t m = (secs % 3600) / 60;
        uint8_t s = secs % 60;
        char upStr[64];
        snprintf(upStr, sizeof(upStr), "%lud %02dh %02dm %02ds", (unsigned long)d, h, m, s);
        
        json += "\"uptime\":\"" + String(upStr) + "\",";
        json += "\"is_ap_mode\":" + String(isApMode ? "true" : "false") + ",";
        if (isApMode) {
            String mac = WiFi.softAPmacAddress();
            mac.replace(":", "");
            String apName = "TUL AP " + mac.substring(mac.length() - 4);
            json += "\"ssid\":\"" + apName + "\",";
            json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
            json += "\"mac\":\"" + WiFi.softAPmacAddress() + "\",";
            json += "\"wifi_connected\":true,";
        } else {
            json += "\"ssid\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("N/A")) + "\",";
            json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
            json += "\"mac\":\"" + deviceMacString() + "\",";
            json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
        }
#ifdef W5500_ETH
        json += "\"wifi_off_for_eth\":" + String(wifiOffForEth ? "true" : "false") + ",";
        json += "\"eth\":{";
        json += "\"present\":" + String(ethPresent ? "true" : "false") + ",";
        json += "\"link\":" + String(ethPresent && ETH.linkUp() ? "true" : "false") + ",";
        json += "\"active\":" + String(knxUseEthernet() ? "true" : "false") + ",";
        json += "\"ip\":\"" + String(ethPresent ? ETH.localIP().toString() : String("0.0.0.0")) + "\",";
        json += "\"mac\":\"" + String(ethPresent ? ETH.macAddress() : String("")) + "\",";
        json += "\"speed\":" + String(ethPresent && ETH.linkUp() ? ETH.linkSpeed() : 0);
        json += "},";
#endif
        json += "\"knx_configured\":" + String(knx.configured() ? "true" : "false") + ",";
        json += "\"prog_mode\":" + String(knx.progMode() ? "true" : "false") + ",";

        uint16_t pa = knx.individualAddress();
        json += "\"knx_pa\":\"" + String((pa >> 12) & 0x0F) + "." + String((pa >> 8) & 0x0F) + "." + String(pa & 0xFF) + "\",";
        
        json += "\"knx_led_pin\":" + String(KNX_LED) + ",";
        json += "\"knx_btn_pin\":" + String(KNX_BUTTON) + ",";
#ifdef KNX_TUNNELING
        json += "\"knx_max_tunnels\":" + String(KNX_TUNNELING) + ",";
#else
        json += "\"knx_max_tunnels\":0,";
#endif

        uint8_t activeClients = 0;
        auto dl = ((Bau091A&)knx.bau()).getPrimaryDataLinkLayer();
        if(dl) activeClients = dl->getActiveTunnelCount();
        json += "\"active_clients\":" + String(activeClients) + ",";
        
        auto tpLayer = ((Bau091A&)knx.bau()).getSecondaryDataLinkLayer();
        if (tpLayer) {
            auto& tp = tpLayer->getTPUart();
            auto& stats = tp.getStatistics();
            auto& sys = tp.getSystemState();
            json += "\"rx_bytes\":" + String(stats.getRxBusBytes()) + ",";
            json += "\"tx_bytes\":" + String(stats.getTxFrameBytes()) + ",";
            json += "\"rx_frames\":" + String(stats.getRxFrames()) + ",";
            json += "\"tx_frames\":" + String(stats.getTxFrames()) + ",";
            json += "\"bus_load\":" + String(stats.getBusLoad()) + ",";
            json += "\"ncn\":{";
            json += "\"type\":\"NCN5120/5121/5130\",";
            json += "\"state\":\"" + String(tp.getBcuStateInfo()) + "\",";
            json += "\"connected\":" + String(tp.isConnected() ? "true" : "false") + ",";
            json += "\"baud\":" + String(tp.getBaudrate()) + ",";
            json += "\"mode\":\"" + String(sys.modeString()) + "\",";
            json += "\"v20v\":"  + String(sys.v20v()  ? "true" : "false") + ",";
            json += "\"vdd2\":"  + String(sys.vdd2()  ? "true" : "false") + ",";
            json += "\"vbus\":"  + String(sys.vbus()  ? "true" : "false") + ",";
            json += "\"vfilt\":" + String(sys.vfilt() ? "true" : "false") + ",";
            json += "\"xtal\":"  + String(sys.xtal()  ? "true" : "false") + ",";
            json += "\"thermal_warning\":" + String(sys.thermalWarning() ? "true" : "false") + ",";
            json += "\"self_test\":\"" + String(ncnSelfTestText(ncnSelfTest)) + "\",";
            json += "\"rx_discarded\":" + String(stats.getRxDiscardedBytes()) + ",";
            json += "\"acr0\":" + acr0StatusJson(tp.internalRegisterTimeouts());
            json += "}";
        } else {
            json += "\"rx_bytes\":0,";
            json += "\"tx_bytes\":0,";
            json += "\"rx_frames\":0,";
            json += "\"tx_frames\":0,";
            json += "\"bus_load\":0,";
            json += "\"ncn\":{\"type\":\"NCN5120/5121/5130\",\"state\":\"NoLayer\","
                    "\"connected\":false,\"baud\":0,\"mode\":\"-\","
                    "\"v20v\":false,\"vdd2\":false,\"vbus\":false,\"vfilt\":false,"
                    "\"xtal\":false,\"thermal_warning\":false,";
            json += "\"self_test\":\"" + String(ncnSelfTestText(ncnSelfTest)) + "\",";
            json += "\"rx_discarded\":0,";
            json += "\"acr0\":" + acr0StatusJson(0) + "}";
        }

        // Build info
        json += ",\"build\":{";
        json += "\"version\":\"" + String(FIRMWARE_VERSION) + "\",";
        json += "\"number\":" + String(BUILD_NUMBER) + ",";
        json += "\"git\":\"" + String(BUILD_GIT) + "\",";
        const esp_partition_t* running_p = esp_ota_get_running_partition();
        const char* part_label = running_p ? running_p->label : "?";
        const char* state_str = "?";
        if (running_p) {
            esp_ota_img_states_t st;
            if (esp_ota_get_state_partition(running_p, &st) == ESP_OK) {
                switch (st) {
                    case ESP_OTA_IMG_NEW:            state_str = "new"; break;
                    case ESP_OTA_IMG_PENDING_VERIFY: state_str = "pending_verify"; break;
                    case ESP_OTA_IMG_VALID:          state_str = "valid"; break;
                    case ESP_OTA_IMG_INVALID:        state_str = "invalid"; break;
                    case ESP_OTA_IMG_ABORTED:        state_str = "aborted"; break;
                    case ESP_OTA_IMG_UNDEFINED:      state_str = "undefined"; break;
                }
            }
        }
        json += "\"partition\":\"" + String(part_label) + "\",";
        json += "\"ota_state\":\"" + String(state_str) + "\"";
        json += "},";

        // Hardware info
        json += "\"hardware\":{";
        json += "\"chip_model\":\"" + String(ESP.getChipModel()) + "\",";
        json += "\"chip_rev\":" + String(ESP.getChipRevision()) + ",";
        json += "\"cpu_freq\":" + String(ESP.getCpuFreqMHz()) + ",";
        json += "\"heap_total\":" + String(ESP.getHeapSize()) + ",";
        json += "\"heap_free\":" + String(ESP.getFreeHeap());
        json += "}";

        json += "}";
        request->send(200, "application/json", json);
    });

    server.begin();
    Serial.println("Webserver started.");

    if (MDNS.begin("tul")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS responder started: http://tul.local");
    }

    // Wait for connection OR wait for Improv/Button. With the cable up there is
    // nothing to wait for — Improv keeps being served from loop().
    while (!wifiOffForEth && WiFi.status() != WL_CONNECTED &&
           (millis() - bootTime < improvWindowMs)) {
#ifndef DISABLE_IMPROV
        improvSerial.handleSerial();
#endif
        // Keep the KNX stack alive: NCN5130 sends U_State/U_SystemStat replies
        // every second. Without knx.loop(), the receiver's _lastReceivedTime
        // stale-detect (5s) flips BCU to DISCONNECTED, which silently drops
        // every L_Data.req in TpUartDataLinkLayer::sendFrame.
        knx.loop();
        if (isApMode) {
            dnsServer.processNextRequest();
        }

        bool currentButtonState = serviceProgButton();
        if (currentButtonState == LOW && buttonState == HIGH) {
            buttonPressStart = millis();
        } else if (currentButtonState == LOW && buttonState == LOW) {
            if (!isApMode && (millis() - buttonPressStart > 2000)) {
                Serial.println("Button held > 2s during boot - Starting Access Point!");
                WiFi.mode(WIFI_AP_STA);
                String mac = WiFi.softAPmacAddress();
                mac.replace(":", "");
                String apName = "TUL AP " + mac.substring(mac.length() - 4);
                WiFi.softAP(apName.c_str());
                dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
                isApMode = true;
                apConnectAuthorized = true;   // button = physical presence
                Serial.print("AP IP Address: ");
                Serial.println(WiFi.softAPIP());

                // Optional: clear credentials so it stays in AP mode if it was failing?
                // Let's just start AP.
            }
        }
        buttonState = currentButtonState;
        
        delay(10);
    }

    if (improvConnected) {
        Serial.println("[Info] WiFi configured via Improv");
    }

#ifdef W5500_ETH
    // knx.start() already joined the routing group on the right interface, so
    // record where we are instead of letting loop() see a change and rejoin for
    // nothing.
    activeIf = currentActiveIf();
#endif

    if (wifiOffForEth) {
        digitalWrite(KNX_LED, LOW);   // LED ON: the gateway is up, over the wire
    } else if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected!");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
        if (isApMode) {
            Serial.println("Shutting down AP as Station connected successfully.");
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            isApMode = false;
        }
        digitalWrite(KNX_LED, LOW); // LED ON (Active Low)
    } else {
#ifdef W5500_ETH
        // A gateway that is reachable over the cable needs no recovery AP;
        // opening one would only put an unprotected SSID on the air.
        if (!isApMode && knxUseEthernet()) {
            Serial.print("WiFi not connected, but ethernet is up: ");
            Serial.println(ETH.localIP());
        } else
#endif
        if (!isApMode) {
            Serial.println("[Warning] WiFi not connected - Starting Fallback Access Point!");
            WiFi.mode(WIFI_AP_STA);
            String mac = WiFi.softAPmacAddress();
            mac.replace(":", "");
            String apName = "TUL AP " + mac.substring(mac.length() - 4);
            WiFi.softAP(apName.c_str());
            Serial.print("AP IP Address: ");
            Serial.println(WiFi.softAPIP());
            dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
            isApMode = true;
            apModeIsFallback = true;
        }
        digitalWrite(KNX_LED, LOW); // LED ON anyway
    }
}

#ifdef W5500_ETH
// Re-arm the KNXnet/IP routing group on whatever interface carries us now.
// enabled(false/true) is closeMultiCast() + setupMultiCast() on the IP data
// link layer; only call it when an interface is actually up (see loop()).
static void knxRejoinRouting(const char* why) {
    auto ipDl = ((Bau091A&)knx.bau()).getPrimaryDataLinkLayer();
    if (ipDl && ipDl->enabled()) {
        Serial.printf("KNX: rejoining routing group on %s\n", why);
        ipDl->enabled(false);
        ipDl->enabled(true);
    }
}
#endif

uint32_t lastWifiCheck = 0;
bool wasConnected = true;
bool otaValidationPending = true;

// WiFi active-reconnect watchdog. The core auto-reconnect recovers a clean
// disassociation but NOT a silent drop or a lost DHCP lease; these track the
// down-duration so loop() can force a fresh re-association + DHCP itself.
#define WIFI_WATCHDOG_GRACE_MS  30000UL   // let core auto-reconnect try first
#define WIFI_WATCHDOG_RETRY_MS  30000UL   // then force begin() at this cadence
uint32_t wifiDownSince     = 0;           // 0 = link considered up
uint32_t lastReconnectKick = 0;           // last forced WiFi.begin() timestamp

void loop() {
    knx.loop();

    // H2: apply web-requested progMode change here (main task only).
    if (progModeReqPending) {
        bool v = progModeReqValue;
        progModeReqPending = false;
        knx.progMode(v);
    }

    // Anti-brick: arduino-esp32 v3's bootloader ships with
    // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y, so a freshly OTA'd partition
    // stays in OTA_IMG_PENDING_VERIFY until we explicitly mark it valid.
    // If we crash / reset before that, the bootloader reverts to the previous
    // slot on next boot. 30 s of successful loop() iterations is the gate.
    if (otaValidationPending && millis() > 30000) {
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t state;
        if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
            if (state == ESP_OTA_IMG_PENDING_VERIFY) {
                esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
                Serial.printf("OTA: %s app valid (was PENDING_VERIFY)\n",
                              err == ESP_OK ? "marked" : "FAILED to mark");
            }
        }
        otaValidationPending = false;
    }

    // BCU watchdog: if the secondary DLL has been disconnected for >10s,
    // force a reset() which sends U_RESET_REQ. The stack's passive recovery
    // (only triggered when _interface->available()) doesn't fire reliably
    // because the UART task drains the buffer before process() can see it.
    static unsigned long lastBcuRecoveryAttempt = 0;
    if (millis() - lastBcuRecoveryAttempt > 10000) {
        lastBcuRecoveryAttempt = millis();
        auto tpDl = ((Bau091A&)knx.bau()).getSecondaryDataLinkLayer();
        if (tpDl && !tpDl->getTPUart().isConnected()) {
            Serial.println("BCU disconnected, forcing TPUart reset");
            tpDl->getTPUart().reset();
        }
    }

#ifdef W5500_ETH
    // Which interface carries KNX changes under us when the cable is plugged or
    // pulled, and the routing multicast group is joined on exactly one netif —
    // so the join has to follow. It may only follow to an interface that exists
    // AND has an address: setupMultiCast() calls fatalError() (an endless blink
    // loop) on a missing netif, and a parked radio has no STA netif at all.
    if (millis() - lastIfCheck > 1000) {
        lastIfCheck = millis();
        ActiveIf now = currentActiveIf();
        if (now != activeIf) {
            Serial.printf("Interface: %s -> %s\n", activeIfName(activeIf), activeIfName(now));
            activeIf = now;
            if (now == IF_ETH) {
                if (isApMode && apModeIsFallback) {
                    // The fallback AP exists only because no network was
                    // reachable. One is now — close it, same as the STA
                    // re-association exit below does.
                    Serial.println("Fallback-AP: ethernet carries the gateway, leaving AP mode");
                    WiFi.softAPdisconnect(true);
                    WiFi.mode(WIFI_STA);
                    dnsServer.stop();
                    isApMode = false;
                    apModeIsFallback = false;
                    digitalWrite(KNX_LED, LOW); // LED ON (Active Low)
                }
                // A deliberate AP (onboarding, button) is left alone —
                // somebody may be using it right now.
                if (!isApMode && WiFi.getMode() != WIFI_OFF) {
                    wifiRadioPark("ethernet took over");
                }
                knxRejoinRouting("ethernet");
            } else if (now == IF_WIFI) {
                knxRejoinRouting("wifi");
            } else {
                // Nothing carries the gateway any more (cable pulled). Get the
                // radio back; the rejoin follows once it has an address, on the
                // next transition through this same check.
                wifiRadioResume("ethernet link lost");
            }
        } else if (now == IF_ETH && wifiResumedAt && !isApMode && !pendingReboot &&
                   WiFi.getMode() != WIFI_OFF &&
                   WiFi.scanComplete() != WIFI_SCAN_RUNNING &&
                   millis() - wifiResumedAt > WIFI_REPARK_MS) {
            // The web UI woke the radio for a scan but nobody followed up with
            // a connect (that path reboots). Back to cable-only, or the two
            // interfaces share the subnet again for the rest of the uptime.
            wifiRadioPark("web UI finished with the radio");
        }
    }
#endif

    // Track the NCN link so /api/status reports what is true now, not what was
    // true at boot. Without this a stick that started with no bus attached kept
    // reporting the boot FAIL after the stack had already reconnected itself.
    static unsigned long lastNcnPoll = 0;
    if (millis() - lastNcnPoll > 1000) {
        lastNcnPoll = millis();
        auto tpDl = ((Bau091A&)knx.bau()).getSecondaryDataLinkLayer();
        uint8_t now = NCN_ST_NO_DL;
        if (tpDl) {
            auto& tp = tpDl->getTPUart();
            auto& sys = tp.getSystemState();
            now = !tp.isConnected()               ? NCN_ST_NO_UART
                : sys.vbus()                      ? NCN_ST_OK
                                                  : NCN_ST_OK_NO_VBUS;

            // --- ACR0 readback, see the globals for why ---------------------
            // Arm on the falling edge of VDD2. Per DS p.23 a VFILT brown-out
            // takes DC2 and V20V down together, so V20V staying high already
            // points away from the supply; ACR0 then says whether DC2EN is off.
            static bool vdd2Seen = false;
            static bool vdd2Last = true;
            if (tp.isConnected()) {
                const bool v = sys.vdd2();
                // Two ways in. The falling edge is the obvious one. The other is
                // finding VDD2 already low on the first look — which is exactly
                // how the original report presented itself (low across several
                // reboots, ACR0 surviving each one) and which an edge detector
                // by construction never sees.
                // SystemState starts out all-zero, so vdd2() reads false before
                // the first U_SystemStat.ind has been processed — on a stick
                // whose bus arrives late that would look like a fault. A real
                // low has the neighbouring rails reporting in, so require that.
                const bool railsReporting = sys.vbus() || sys.vfilt() || sys.v20v();
                const bool edge    = vdd2Seen && vdd2Last && !v;
                const bool bootLow = !vdd2Seen && !v && railsReporting;
                if ((edge || bootLow) && !acr0HaveFault && !acr0FaultPending) {
                    Serial.printf("VDD2 %s (V20V=%d VFILT=%d VBUS=%d) - reading ACR0 back\n",
                                  edge ? "dropped" : "low on first look",
                                  sys.v20v(), sys.vfilt(), sys.vbus());
                    acr0FaultPending = true;
                    acr0FaultHaveFirst = false;
                    acr0FaultAttempts = 0;
                    acr0ReadRequested = true;
                }
                // Only a populated SystemState counts as "seen". Recording the
                // all-zero default would latch vdd2Seen=true with vdd2Last=false,
                // after which the edge (wants vdd2Last true) and the boot-low
                // path (wants vdd2Seen false) can both never fire again — the
                // very case this is here to catch would be lost for the whole
                // uptime.
                if (railsReporting) {
                    vdd2Last = v;
                    vdd2Seen = true;
                }
            }

            // Collect an answered read. internalRegisterReadAt() changes per
            // answer, which is what distinguishes a fresh value from the last.
            if (tp.internalRegisterValid() &&
                tp.internalRegisterRequest() == U_INT_REG_RD_REQ_ACR0) {
                const unsigned long at = tp.internalRegisterReadAt();
                if (!acr0HaveReading || at != acr0ReadAtMs) {
                    const uint8_t v = tp.internalRegisterValue();
                    acr0Value = v;
                    acr0ReadAtMs = at;
                    acr0HaveReading = true;
                    Serial.printf("ACR0 = 0x%02X (V20VEN=%d DC2EN=%d XCLKEN=%d TRIGEN=%d V20VCLIMIT=%d)\n",
                                  v, !!(v & ACR0_FLAG_V20VEN), !!(v & ACR0_FLAG_DC2EN),
                                  !!(v & ACR0_FLAG_XCLKEN), !!(v & ACR0_FLAG_TRIGEN),
                                  v & ACR0_MASK_V20VCLIMIT);
                    if (acr0FaultPending) {
                        if (sys.vdd2()) {
                            // The rail recovered before the reading could be
                            // confirmed; it says nothing about the fault.
                            acr0FaultPending = false;
                            acr0FaultHaveFirst = false;
                        } else if (!acr0FaultHaveFirst) {
                            acr0FaultFirstValue = v;
                            acr0FaultHaveFirst = true;
                            acr0ReadRequested = true;   // confirm before believing
                            Serial.printf("  ^ VDD2 low, first reading 0x%02X - confirming\n", v);
                        } else if (!(v & ACR0_FLAG_V20VEN) && sys.v20v()) {
                            // The value says the 20 V regulator is disabled while
                            // the rail reports it in range — those cannot both be
                            // true, so this byte is not ACR0. Catches the one
                            // collision that agreement alone would not: U_State.ind
                            // is deterministically 0x07, so two of them would
                            // confirm each other.
                            Serial.printf("  ^ reading 0x%02X contradicts the V20V rail - discarding\n", v);
                            acr0FaultHaveFirst = false;
                        } else if (v == acr0FaultFirstValue) {
                            acr0FaultValue = v;
                            acr0FaultAtMs = at;
                            acr0HaveFault = true;
                            acr0FaultPending = false;
                            acr0FaultHaveFirst = false;
                            Serial.printf("  ^ confirmed 0x%02X with VDD2 low: DC2 is %s\n", v,
                                          (v & ACR0_FLAG_DC2EN) ? "enabled, so the regulator itself is out of range"
                                                                : "DISABLED in ACR0 - something switched DC2EN off");
                        } else {
                            // Two different values means at least one of them was
                            // not the register. Throw both away.
                            Serial.printf("  ^ readings disagree (0x%02X vs 0x%02X) - discarding both\n",
                                          acr0FaultFirstValue, v);
                            acr0FaultHaveFirst = false;
                        }
                    }
                }
            }

#ifdef NCN_ACR0_WRITE_TEST
            // Restore first: if a write is still queued behind it, the restore
            // deadline of that write is what counts, not this one.
            if (acr0RestoreAt != 0 && (long)(millis() - acr0RestoreAt) >= 0) {
                if (tp.writeInternalRegister(U_INT_REG_WR_REQ_ACR0, acr0RestoreValue)) {
                    Serial.printf("ACR0 restore -> 0x%02X (rails now V20V=%d VDD2=%d VFILT=%d)\n",
                                  acr0RestoreValue, sys.v20v(), sys.vdd2(), sys.vfilt());
                    acr0RestoreAt = 0;
                    acr0ReadRequested = true;   // prove the restore landed
                }
            }
            if (acr0WritePending) {
                if (tp.writeInternalRegister(U_INT_REG_WR_REQ_ACR0, acr0WriteValue)) {
                    Serial.printf("ACR0 write -> 0x%02X (rails before V20V=%d VDD2=%d VFILT=%d)\n",
                                  acr0WriteValue, sys.v20v(), sys.vdd2(), sys.vfilt());
                    acr0WritePending = false;
                    acr0ReadRequested = true;   // did it land?
                }
            }
#endif
            // An outstanding fault reading keeps asking: a read that timed out
            // must not quietly forfeit the one measurement worth having. The
            // bound counts completed attempts; a request that stays parked
            // because the line never goes quiet retries silently and for free,
            // which is the behaviour we want on a busy bus.
            if (acr0FaultPending && !acr0ReadRequested && !tp.internalRegisterPending()) {
                if (acr0FaultAttempts < ACR0_FAULT_MAX_ATTEMPTS) {
                    acr0FaultAttempts++;
                    acr0ReadRequested = true;
                } else {
                    Serial.println("ACR0: giving up on the fault reading (no quiet line)");
                    acr0FaultPending = false;
                    acr0FaultHaveFirst = false;
                }
            }

        }
        if (now != ncnSelfTest) {
            Serial.printf("NCN state: %s -> %s\n",
                          ncnSelfTestText(ncnSelfTest), ncnSelfTestText(now));
            ncnSelfTest = now;
        }
    }

    // Issuing is attempted on every pass rather than once a second. The guards
    // refuse while the line is busy, and the stack's own state poll runs at 1 Hz
    // as well — retrying at 1 Hz could phase-lock against it and never find a
    // quiet window. At loop speed the next gap is milliseconds away.
    if (acr0ReadRequested) {
        auto acrDl = ((Bau091A&)knx.bau()).getSecondaryDataLinkLayer();
        if (acrDl) {
            auto& acrTp = acrDl->getTPUart();
            if (!acrTp.internalRegisterPending() &&
                acrTp.requestInternalRegister(U_INT_REG_RD_REQ_ACR0)) {
                acr0ReadRequested = false;
            }
        }
    }

    if (pendingReboot && (millis() - rebootTime > 2000)) {
        Serial.println("Rebooting to apply new WiFi credentials...");
        ESP.restart();
    }
    
#ifndef DISABLE_IMPROV
    // Improv-Serial: lib enforces a 120 s window after boot, then goes
    // hard-silent so the UART is free for application traffic.
    // Re-provisioning happens via reboot (the window opens unconditionally).
    improvSerial.handleSerial();
#endif

    // Button long-press logic for AP mode
    bool currentButtonState = serviceProgButton();
    if (currentButtonState == LOW && buttonState == HIGH) {
        buttonPressStart = millis();
    } else if (currentButtonState == LOW && buttonState == LOW) {
        if (!isApMode && (millis() - buttonPressStart > 2000)) {
            Serial.println("Button held > 2s - Starting Access Point!");
            // On a cable-carried gateway the radio is parked, and the mode
            // change below powers it up without the state ever saying so:
            // wifiOffForEth would stay true, /api/status would keep reporting
            // the radio as off for ethernet, and the web UI would show that
            // instead of the AP the user is standing in front of. It also
            // wedges wifiRadioPark(), which no-ops on an already-parked flag,
            // so the radio could never be parked again after the AP closes.
            // No-op when the radio was never parked.
            wifiRadioResume("button opened the access point", false);
            WiFi.disconnect();
            WiFi.mode(WIFI_AP_STA);
            String mac = WiFi.softAPmacAddress();
            mac.replace(":", "");
            String apName = "TUL AP " + mac.substring(mac.length() - 4);
            WiFi.softAP(apName.c_str());
            dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
            Serial.print("AP IP Address: ");
            Serial.println(WiFi.softAPIP());
            isApMode = true;
            apConnectAuthorized = true;   // button = physical presence
            if (knx.progMode()) {
                knx.progMode(false);
            }
        } else if (isApMode && apModeIsFallback && !apConnectAuthorized &&
                   (millis() - buttonPressStart > 2000)) {
            // Physical-presence override for a device stuck in the fallback AP
            // (e.g. moved to a new network): a long-press authorizes
            // /api/wifi/connect so it can be reprovisioned via the captive
            // portal. An RF-range attacker on the open AP cannot press the
            // button. Leaves apModeIsFallback set so the auto-exit still fires
            // if the original network happens to return.
            Serial.println("Button held > 2s in fallback AP - reprovisioning authorized");
            apConnectAuthorized = true;
        }
    }
    buttonState = currentButtonState;

    if (isApMode) {
        // The fallback AP is a recovery state, not a destination. It is entered
        // in setup() when the boot window expired with no WiFi, but the STA
        // stays armed the whole time (core auto-reconnect). If it re-associates
        // and gets an IP, tear the AP down and resume normal STA operation so
        // the WiFi watchdog is armed again — otherwise the gateway would sit in
        // the open captive-portal AP until a manual power-cycle even though the
        // network is back (the exact silent-until-power-cycle failure the
        // watchdog exists to prevent). Onboarding and button-started APs are
        // deliberate (apModeIsFallback == false) and are NOT auto-exited.
        if (apModeIsFallback &&
            WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0) {
            Serial.print("Fallback-AP: STA re-associated, leaving AP mode. IP: ");
            Serial.println(WiFi.localIP());
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            dnsServer.stop();
            isApMode = false;
            apModeIsFallback = false;
            digitalWrite(KNX_LED, LOW); // LED ON (Active Low)
            // Prime the watchdog to the "up" state so the next 5 s sample sees
            // steady-connected and does not log a spurious down-transition.
            wasConnected = true;
            wifiDownSince = 0;
            lastReconnectKick = 0;
            // Fall through to normal WiFi monitoring this iteration.
        } else {
            dnsServer.processNextRequest();
            // Double flash pattern for AP mode: 100ms ON, 100ms OFF, 100ms ON, 700ms OFF
            uint32_t t = millis() % 1000;
            if (t < 100 || (t > 200 && t < 300)) {
                digitalWrite(KNX_LED, LOW); // ON (Active Low)
            } else {
                digitalWrite(KNX_LED, HIGH); // OFF
            }
            return; // Skip normal WiFi monitoring while in AP mode
        }
    }

    // Monitor WiFi + active reconnect watchdog. The core auto-reconnect recovers
    // a clean disassociation, but not a silent drop or a lost DHCP lease (the STA
    // can even report WL_CONNECTED while holding 0.0.0.0). We treat "associated
    // AND has an IP" as the real up-state and force re-association + DHCP once the
    // grace window passes — otherwise the gateway goes silent until a power-cycle.
    if (!wifiOffForEth && millis() - lastWifiCheck > 5000) {
        lastWifiCheck = millis();
        bool associated = (WiFi.status() == WL_CONNECTED);
        bool hasIp      = ((uint32_t)WiFi.localIP() != 0);
        bool isConnected = associated && hasIp;
        
        if (isConnected != wasConnected) {
            wasConnected = isConnected;
            if (isConnected) {
                Serial.print("WiFi up! IP Address: ");
                Serial.println(WiFi.localIP());
                digitalWrite(KNX_LED, LOW); // LED ON (Active Low)
                wifiDownSince = 0;
                lastReconnectKick = 0;
            } else {
                Serial.printf("WARNING: WiFi link down (assoc=%d ip=%d) - watchdog active\n",
                              associated, hasIp);
                digitalWrite(KNX_LED, HIGH); // LED OFF
                wifiDownSince = millis();
            }
        }

        // Active recovery: once down past the grace window, force a re-assoc +
        // fresh DHCP every WIFI_WATCHDOG_RETRY_MS until the link is really back.
        if (!isConnected) {
            if (wifiDownSince == 0) wifiDownSince = millis();
            uint32_t downFor = millis() - wifiDownSince;
            if (downFor >= WIFI_WATCHDOG_GRACE_MS &&
                (lastReconnectKick == 0 ||
                 millis() - lastReconnectKick >= WIFI_WATCHDOG_RETRY_MS)) {
                lastReconnectKick = millis();
                Serial.printf("WiFi down %lus - forcing reconnect (disconnect + begin)\n",
                              (unsigned long)(downFor / 1000));
                WiFi.setSleep(WIFI_PS_NONE);   // keep PS off across re-assoc (C6 rule)
                // reconnect() = esp_wifi_disconnect()+connect() WITHOUT set_config,
                // so it won't collide with an in-flight core auto-reconnect (avoids
                // "sta is connecting, cannot set config"); reuses stored creds and
                // restarts DHCP on re-association.
                WiFi.reconnect();
            }
        }
        
        // Feed hardware-independent WDT manually if needed by checking system sanity here.
        // The Arduino core handles the FreeRTOS IDLE/Loop task WDT automatically.
    }
}
