#include <Network.h>
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include "version.h"
#include <knx.h>
#include "ImprovWiFiLibrary.h"
#include "nvs_flash.h"

#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include "index_html.h"
#include <knx/bau091A.h>
#include <knx/ip_data_link_layer.h>

#include <TPUart/Interface/ESP32.h>

AsyncWebServer server(80);
ImprovWiFi improvSerial(&Serial);
bool improvConnected = false;

void onImprovWiFiErrorCb(ImprovTypes::Error err) {
    Serial.printf("Improv error: %d\n", err);
}

void onImprovWiFiConnectedCb(const char *ssid, const char *password) {
    Serial.printf("Improv Connected! SSID: %s\n", ssid);
    improvConnected = true;
}

bool connectWifi(const char *ssid, const char *password) {
    Serial.printf("Connecting to WiFi: %s\n", ssid);
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

void setup() {
    Serial.begin(115200);

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

    // Enable WiFi AP_STA mode so we can scan and host AP simultaneously
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(); // Clear any stale connection state

    // Check for stored credentials
    bool hasCredentials = WiFi.psk().length() > 0 || WiFi.SSID().length() > 0;
    Serial.print("WiFi SSID length: ");
    Serial.println(WiFi.SSID().length());
    Serial.print("WiFi PSK length: ");
    Serial.println(WiFi.psk().length());

    const uint32_t improvWindowMs = 120000;  // 120 seconds

    if (!hasCredentials) {
        Serial.println("No WiFi credentials stored - Starting AP immediately!");
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
        WiFi.begin();
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

    server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request){
        int n = WiFi.scanNetworks();
        String json = "[";
        // filter out duplicates if we want, or just let UI handle it.
        for (int i = 0; i < n; ++i) {
            if (i > 0) json += ",";
            json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *request){
        if(request->hasParam("ssid", true)) {
            String ssid = request->getParam("ssid", true)->value();
            String pass = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
            
            Serial.printf("Received WiFi config via Web Portal. SSID: %s\n", ssid.c_str());
            
            WiFi.persistent(true);
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
            json += "\"ssid\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("N/A")) + "\",";
            json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
            json += "\"mac\":\"" + WiFi.macAddress() + "\",";
            json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
        }
        json += "\"knx_configured\":" + String(knx.configured() ? "true" : "false") + ",";
        
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
            auto& stats = tpLayer->getTPUart().getStatistics();
            json += "\"rx_bytes\":" + String(stats.getRxBusBytes()) + ",";
            json += "\"tx_bytes\":" + String(stats.getTxFrameBytes()) + ",";
            json += "\"rx_frames\":" + String(stats.getRxFrames()) + ",";
            json += "\"tx_frames\":" + String(stats.getTxFrames()) + ",";
            json += "\"bus_load\":" + String(stats.getBusLoad());
        } else {
            json += "\"rx_bytes\":0,";
            json += "\"tx_bytes\":0,";
            json += "\"rx_frames\":0,";
            json += "\"tx_frames\":0,";
            json += "\"bus_load\":0";
        }

        // Build info
        json += ",\"build\":{";
        json += "\"version\":\"" + String(FIRMWARE_VERSION) + "\",";
        json += "\"number\":" + String(BUILD_NUMBER) + ",";
        json += "\"git\":\"" + String(BUILD_GIT) + "\"";
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
        improvSerial.handleSerial();
        if (isApMode) {
            dnsServer.processNextRequest();
        }

        bool currentButtonState = digitalRead(KNX_BUTTON);
        if (currentButtonState == LOW && buttonState == HIGH) {
            buttonPressStart = millis();
        } else if (currentButtonState == LOW && buttonState == LOW) {
            if (!isApMode && (millis() - buttonPressStart > 2000)) {
                Serial.println("Button held > 2s during boot - Starting Access Point!");
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

void loop() {
    knx.loop();
    
    // ImprovSerial always active - allows re-configuration at any time
    // via ESP WebFlasher or CLI. USB-JTAG does not reset on port open,
    // so a time-limited window would expire before the user connects.
    improvSerial.handleSerial();

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

    if (pendingReboot && (millis() - rebootTime > 2000)) {
        Serial.println("Rebooting to apply new WiFi credentials...");
        ESP.restart();
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
