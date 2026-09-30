// Bounded shield-box test: ESP32's own AP and one known phone only.
// Boot leaves Wi-Fi off. No LoRa API or automatic transmission.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstring>

static constexpr char kSsid[] = "RFMON-LAB-D682C4";
static constexpr char kPassword[] = "rfmon-test-29";
static constexpr uint8_t kPhone[6] = {0xf4, 0x30, 0x8b, 0xaf, 0x56, 0x69};
static constexpr unsigned kBurstLimit = 60;
static constexpr uint32_t kBurstIntervalMs = 25;
static bool apRunning = false;
static bool probeUsed = false;
static bool burstUsed = false;
static bool burstActive = false;
static unsigned burstAttempts = 0;
static unsigned burstAccepted = 0;
static uint32_t nextBurstMs = 0;
static uint8_t frame[26] = {};
static String command;

static bool makeFrame() {
    if (!apRunning) { Serial.println("BLOCKED AP_OFF"); return false; }
    wifi_sta_list_t stations = {};
    const esp_err_t listResult = esp_wifi_ap_get_sta_list(&stations);
    if (listResult != ESP_OK || stations.num != 1) {
        Serial.printf("BLOCKED CLIENT_COUNT result=%s count=%d\n",
                      esp_err_to_name(listResult), stations.num);
        return false;
    }
    if (memcmp(stations.sta[0].mac, kPhone, 6) != 0) {
        Serial.println("BLOCKED CLIENT_MAC_MISMATCH");
        return false;
    }
    uint8_t apMac[6] = {};
    if (esp_wifi_get_mac(WIFI_IF_AP, apMac) != ESP_OK) {
        Serial.println("BLOCKED AP_MAC_ERROR");
        return false;
    }
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xc0; // Management: deauthentication.
    memcpy(frame + 4, kPhone, 6);
    memcpy(frame + 10, apMac, 6);
    memcpy(frame + 16, apMac, 6);
    frame[24] = 2; // Previous authentication no longer valid.
    return true;
}

static void cancelBurst(const char* why) {
    if (!burstActive) return;
    burstActive = false;
    Serial.printf("BURST_END reason=%s attempts=%u api_ok=%u\n",
                  why, burstAttempts, burstAccepted);
}

static void handle(const String& c) {
    if (c == "status") {
        Serial.printf("AP=%s clients=%u BSSID=%s probe_used=%u burst_used=%u burst_active=%u attempts=%u api_ok=%u\n",
                      apRunning ? "on" : "off",
                      apRunning ? WiFi.softAPgetStationNum() : 0,
                      apRunning ? WiFi.softAPmacAddress().c_str() : "none",
                      probeUsed, burstUsed, burstActive, burstAttempts, burstAccepted);
    } else if (c == "ap_start") {
        if (apRunning) { Serial.println("AP already started"); return; }
        WiFi.mode(WIFI_AP);
        if (!WiFi.softAP(kSsid, kPassword, 11, false, 1)) {
            WiFi.mode(WIFI_OFF); Serial.println("AP start failed"); return;
        }
        const esp_err_t power = esp_wifi_set_max_tx_power(8);
        if (power != ESP_OK) {
            WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
            Serial.printf("AP power config failed: %s\n", esp_err_to_name(power));
            return;
        }
        apRunning = true;
        Serial.printf("AP_STARTED SSID=%s BSSID=%s channel=11 max_clients=1\n",
                      kSsid, WiFi.softAPmacAddress().c_str());
    } else if (c == "probe_once") {
        if (probeUsed) { Serial.println("BLOCKED PROBE_ALREADY_USED"); return; }
        if (!makeFrame()) return;
        probeUsed = true;
        const esp_err_t result = esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), true);
        Serial.printf("PROBE_RAW_DEAUTH api=%s; RF reception not yet verified\n",
                      esp_err_to_name(result));
    } else if (c == "flood_once") {
        if (burstUsed || burstActive) { Serial.println("BLOCKED BURST_ALREADY_USED"); return; }
        if (!probeUsed) { Serial.println("BLOCKED PROBE_REQUIRED"); return; }
        if (!makeFrame()) return;
        burstUsed = true;
        burstActive = true;
        burstAttempts = 0;
        burstAccepted = 0;
        nextBurstMs = millis();
        Serial.printf("BURST_START max_frames=%u interval_ms=%lu max_duration_ms=%lu\n",
                      kBurstLimit, (unsigned long)kBurstIntervalMs,
                      (unsigned long)(kBurstLimit * kBurstIntervalMs));
    } else if (c == "stop_test") {
        cancelBurst("operator_stop");
        Serial.println("TEST_STOPPED");
    } else if (c == "ap_stop") {
        cancelBurst("ap_stop");
        if (apRunning) { WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF); apRunning = false; }
        Serial.println("AP_STOPPED");
    } else {
        Serial.println("Unknown command; no action");
    }
}

void setup() {
    Serial.begin(115200);
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    Serial.println("RFMON isolated shield-box test; idle; LoRa unused");
    Serial.println("Commands: status, ap_start, probe_once, flood_once, stop_test, ap_stop");
}

void loop() {
    while (Serial.available()) {
        const char c = char(Serial.read());
        if (c == '\n') {
            command.trim();
            if (command.length()) handle(command);
            command = "";
        } else if (c != '\r') {
            if (command.length() < 80) command += c;
            else command = "invalid";
        }
    }
    if (burstActive && int32_t(millis() - nextBurstMs) >= 0) {
        const esp_err_t result = esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), true);
        ++burstAttempts;
        if (result == ESP_OK) ++burstAccepted;
        else {
            Serial.printf("BURST_API_ERROR attempt=%u result=%s\n",
                          burstAttempts, esp_err_to_name(result));
            cancelBurst("api_error");
        }
        if (burstActive && burstAttempts >= kBurstLimit) cancelBurst("limit");
        nextBurstMs += kBurstIntervalMs;
    }
    delay(1);
}
