#include <Network.h>
#include <Arduino.h>
#include <WiFi.h>
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

void setup() {
    Serial.begin(115200);
    delay(2000);
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

    // Setup Improv callbacks
    improvSerial.onImprovConnected(onImprovWiFiConnectedCb);
    improvSerial.onImprovError(onImprovWiFiErrorCb);

    // Start 120s Improv window immediately at boot
    bootTime = millis();
    Serial.println("[Info] Improv WiFi provisioning active for 120 seconds...");
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    improvSerial.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32_C3, "TUL KNX/IP Gateway", "1.0.0", "TUL Gateway");
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    // Improv currently uses CF_ESP32 for generic fallback if C6 isn't present
    improvSerial.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32, "TUL32 KNX/IP Gateway", "1.0.0", "TUL32 Gateway");
#else
    improvSerial.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32, "TUL KNX/IP Gateway", "1.0.0", "TUL Gateway");
#endif

    // Enable WiFi STA mode to read stored credentials
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(); // Clear any stale connection state

    // Check for stored credentials
    bool hasCredentials = WiFi.psk().length() > 0 || WiFi.SSID().length() > 0;
    Serial.print("WiFi SSID length: ");
    Serial.println(WiFi.SSID().length());
    Serial.print("WiFi PSK length: ");
    Serial.println(WiFi.psk().length());

    // Wait for WiFi connection ONLY within the 120s Improv window
    // This allows re-configuration even if credentials are stored
    const uint32_t improvWindowMs = 120000;  // 120 seconds

    if (hasCredentials) {
        Serial.println("WiFi credentials found, attempting auto-reconnect...");
        WiFi.begin();

        // Wait for connection, but keep Improv active and respect 120s window
        while (WiFi.status() != WL_CONNECTED && (millis() - bootTime < improvWindowMs)) {
            improvSerial.handleSerial();
            delay(10);
        }
    } else {
        Serial.println("No WiFi credentials stored.");
    }

    // If not connected after initial attempt, wait for Improv provisioning
    // for the remainder of the 120s window
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Waiting for Improv WiFi provisioning...");

        while (WiFi.status() != WL_CONNECTED && (millis() - bootTime < improvWindowMs)) {
            improvSerial.handleSerial();
            delay(10);
        }

        if (improvConnected) {
            Serial.println("[Info] WiFi configured via Improv");
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected!");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
        digitalWrite(KNX_LED, LOW); // LED ON (Active Low)
    } else {
        Serial.println("[Warning] WiFi not connected - system will continue");
        digitalWrite(KNX_LED, LOW); // LED ON anyway
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
        request->send_P(200, "text/html", index_html);
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
        json += "\"ssid\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("N/A")) + "\",";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"mac\":\"" + WiFi.macAddress() + "\",";
        json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
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
        json += "\"number\":" + String(BUILD_NUMBER) + ",";
        json += "\"git\":\"" + String(BUILD_GIT) + "\"";
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
