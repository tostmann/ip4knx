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

bool connectWifi(const char *ssid, const char *password) {
    Serial.printf("Connecting to WiFi: %s\n", ssid);
    WiFi.setSleep(WIFI_PS_NONE);   // C6: PS_NONE before every begin()
    WiFi.begin(ssid, password);
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
        return true;
    }
    
    Serial.println("\nWiFi connection failed");
    return false;
}

uint32_t bootTime = 0;
bool isApMode = false;
bool pendingReboot = false;
uint32_t rebootTime = 0;
uint32_t buttonPressStart = 0;
bool buttonState = HIGH;
DNSServer dnsServer;
const byte DNS_PORT = 53;

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
    String latestVersion;             // written only in async_tcp task -> safe
    String url;                       // "
    String md5;                       // "
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
    updateInfo.latestVersion = latest;

    String path = jsonGetOtaField(body, "path");
    String md5  = jsonGetOtaField(body, "md5");
    if (path.length() == 0 || md5.length() != 32) {
        setUpdateError("no ota entry for " UPDATE_CHIP_KEY);
        updateInfo.state = UPD_ERROR;
        return false;
    }
    String base = String(UPDATE_MANIFEST_URL);
    int lastSlash = base.lastIndexOf('/');
    updateInfo.url = base.substring(0, lastSlash + 1) + path;
    updateInfo.md5 = md5;

    if (versionCompare(latest, FIRMWARE_VERSION) > 0) {
        updateInfo.state = UPD_AVAILABLE;
    } else {
        updateInfo.state = UPD_IDLE;
    }
    Serial.printf("Update check: current=%s latest=%s state=%s\n",
                  FIRMWARE_VERSION, latest.c_str(), updateStateName(updateInfo.state));
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
    if (!Update.setMD5(updateInfo.md5.c_str())) {
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
    if (updateInfo.url.length() == 0 || updateInfo.md5.length() != 32) {
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
    j += "\"latest\":\"" + updateInfo.latestVersion + "\",";
    j += "\"available\":" + String(updateInfo.state == UPD_AVAILABLE ? "true" : "false") + ",";
    j += "\"progress\":" + String((unsigned)updateInfo.progress) + ",";
    j += "\"total\":" + String((unsigned)updateInfo.total) + ",";
    j += "\"error\":\"" + jsonEscape(String(updateInfo.error)) + "\"";
    j += "}";
    return j;
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
    esp_brownout_disable();

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

    if (!hasCredentials) {
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
                Serial.println(sys.vbus() ? "Result: OK" : "Result: OK (no VBUS!)");
            } else {
                Serial.println("Result: FAIL (no NCN UART response)");
            }
        }
    }
    Serial.println("======================\n");

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/html", index_html);
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
            if (len && !Update.hasError()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                }
            }
            if (final) {
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
        doUpdateCheck();
        request->send(200, "application/json", updateStatusJson());
    });
    server.on("/api/update/install", HTTP_POST, [](AsyncWebServerRequest *request){
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

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{";
        
        // uptime
        uint32_t secs = millis() / 1000;
        uint32_t d = secs / 86400;
        uint8_t h = (secs % 86400) / 3600;
        uint8_t m = (secs % 3600) / 60;
        uint8_t s = secs % 60;
        char upStr[64];
        sprintf(upStr, "%dd %02dh %02dm %02ds", d, h, m, s);
        
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
            json += "\"mac\":\"" + WiFi.macAddress() + "\",";
            json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
        }
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
            json += "\"thermal_warning\":" + String(sys.thermalWarning() ? "true" : "false");
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
                    "\"xtal\":false,\"thermal_warning\":false}";
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

    // Wait for connection OR wait for Improv/Button
    while (WiFi.status() != WL_CONNECTED && (millis() - bootTime < improvWindowMs)) {
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

        bool currentButtonState = digitalRead(KNX_BUTTON);
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

    if (WiFi.status() == WL_CONNECTED) {
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
        }
        digitalWrite(KNX_LED, LOW); // LED ON anyway
    }
}

uint32_t lastWifiCheck = 0;
uint32_t lastNtpSend = 0;
bool wasConnected = true;
bool otaValidationPending = true;

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
    bool currentButtonState = digitalRead(KNX_BUTTON);
    if (currentButtonState == LOW && buttonState == HIGH) {
        buttonPressStart = millis();
    } else if (currentButtonState == LOW && buttonState == LOW) {
        if (!isApMode && (millis() - buttonPressStart > 2000)) {
            Serial.println("Button held > 2s - Starting Access Point!");
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
            if (knx.progMode()) {
                knx.progMode(false);
            }
        }
    }
    buttonState = currentButtonState;

    if (isApMode) {
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

    // Monitor WiFi Connection
    if (millis() - lastWifiCheck > 5000) {
        lastWifiCheck = millis();
        bool isConnected = (WiFi.status() == WL_CONNECTED);
        
        if (isConnected != wasConnected) {
            wasConnected = isConnected;
            if (isConnected) {
                Serial.println("WiFi reconnected!");
                Serial.print("IP Address: ");
                Serial.println(WiFi.localIP());
                digitalWrite(KNX_LED, LOW); // LED ON (Active Low)
            } else {
                Serial.println("WARNING: WiFi disconnected! Awaiting auto-reconnect...");
                digitalWrite(KNX_LED, HIGH); // LED OFF
            }
        }
        
        // Feed hardware-independent WDT manually if needed by checking system sanity here.
        // The Arduino core handles the FreeRTOS IDLE/Loop task WDT automatically.
    }
}
