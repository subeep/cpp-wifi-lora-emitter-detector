// Owned-AP disconnect reception check. No raw injection, flood or LoRa API.
// Boot is radio-idle. Only explicit serial commands start the dedicated AP.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

static bool apRunning = false;
static bool disconnectedBefore = false;
static uint32_t lastDisconnect = 0;
static String command;
static const char* ssid = "RFMON-LAB-D682C4";
static const char* password = "rfmon-test-29"; // Isolated test network only; no upstream access.

void setup() {
    Serial.begin(115200);
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    Serial.println("RFMON owned-AP reception test; idle; LoRa unused");
    Serial.println("Commands: status, ap_start, disconnect_once, ap_stop");
}
void handle(const String& c) {
    if (c == "status") {
        Serial.printf("AP=%s SSID=%s channel=11 clients=%u BSSID=%s\n",
            apRunning ? "on" : "off", ssid, apRunning ? WiFi.softAPgetStationNum() : 0,
            apRunning ? WiFi.softAPmacAddress().c_str() : "none");
    } else if (c == "ap_start") {
        if (apRunning) { Serial.println("AP already started"); return; }
        WiFi.mode(WIFI_AP);
        if (!WiFi.softAP(ssid, password, 11, false, 1)) {
            WiFi.mode(WIFI_OFF); Serial.println("AP start failed"); return;
        }
        // Reduced power is for the reception experiment, not RF isolation.
        esp_err_t result = esp_wifi_set_max_tx_power(8);
        if (result != ESP_OK) {
            WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
            Serial.printf("Power configuration failed: %s; stopped\n",esp_err_to_name(result)); return;
        }
        apRunning = true;
        Serial.printf("AP started: SSID=%s password=%s BSSID=%s channel=11 max_clients=1\n",
            ssid,password,WiFi.softAPmacAddress().c_str());
    } else if (c == "disconnect_once") {
        if (!apRunning || WiFi.softAPgetStationNum() != 1) {
            Serial.println("No single associated test client; no action"); return;
        }
        if (disconnectedBefore && uint32_t(millis()-lastDisconnect) < 10000) {
            Serial.println("Cooldown active; no action"); return;
        }
        lastDisconnect = millis(); disconnectedBefore = true;
        esp_err_t result = esp_wifi_deauth_sta(0); // Only this AP's associated station.
        Serial.printf("OWN_AP_DISCONNECT uptime_ms=%lu result=%s; RF reception not yet verified\n",
            (unsigned long)lastDisconnect,esp_err_to_name(result));
    } else if (c == "ap_stop") {
        WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF); apRunning = false;
        Serial.println("AP stopped");
    } else { Serial.println("Unknown command; no action"); }
}
void loop() {
    while (Serial.available()) {
        char c = char(Serial.read());
        if (c == '\n') { command.trim(); if (command.length()) handle(command); command = ""; }
        else if (c != '\r') {
            if (command.length() < 80) command += c;
            else command = "invalid";
        }
    }
    delay(10);
}
