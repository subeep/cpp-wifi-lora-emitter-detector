// Bounded evil-twin beacon source for validating the RF identity detector
// (docs/WIFI_RF_IDENTITY_2026-11-04.md), rf_identity_two_transmitters. It
// brings up a real low-power softAP whose BSSID is set to that of an access
// point we own, on the same channel. The X310 then hears beacons under one
// BSSID from two radios (the genuine AP and this ESP), with two carrier
// offsets - the "two transmitters, one identity" the rule flags.
//
// Safety bounds: Wi-Fi off at boot; BSSID/channel/SSID set over serial and
// echoed; the clone AP only starts on `clone_start`; TX power held low. For
// our own access points only.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstring>

static constexpr char kPassword[] = "rfmon-test-29";  // WPA2; our own devices won't auto-join another PSK
static constexpr int8_t kMaxTxPowerQdbm = 8;          // ~2 dBm

static uint8_t bssid[6] = {};
static int channel = 0;
static bool haveBssid = false, running = false;
static String ssid = "rfmon-eviltwin";
static String command;

static bool parseMac(const String& s, uint8_t out[6]) {
    int v[6];
    if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; ++i) { if (v[i] < 0 || v[i] > 255) return false; out[i] = uint8_t(v[i]); }
    return (out[0] & 0x01) == 0;  // must be unicast for a BSSID
}
static String macStr(const uint8_t m[6]) {
    char b[18];
    std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(b);
}

static void stopClone(const char* why) {
    if (!running) return;
    WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF); running = false;
    Serial.printf("CLONE_STOPPED reason=%s\n", why);
}

static void handle(const String& c) {
    if (c == "status") {
        uint8_t cur[6] = {};
        if (running) esp_wifi_get_mac(WIFI_IF_AP, cur);
        Serial.printf("bssid=%s have=%u channel=%d ssid=%s running=%u actual_bssid=%s\n",
                      macStr(bssid).c_str(), haveBssid, channel, ssid.c_str(), running,
                      running ? macStr(cur).c_str() : "none");
    } else if (c.startsWith("set bssid ")) {
        if (running) { Serial.println("BLOCKED RUNNING"); return; }
        if (parseMac(c.substring(10), bssid)) { haveBssid = true; Serial.printf("BSSID=%s\n", macStr(bssid).c_str()); }
        else Serial.println("BLOCKED BAD_MAC (must be unicast)");
    } else if (c.startsWith("set channel ")) {
        if (running) { Serial.println("BLOCKED RUNNING"); return; }
        const int ch = c.substring(12).toInt();
        if (ch >= 1 && ch <= 196) { channel = ch; Serial.printf("CHANNEL=%d\n", channel); }
        else Serial.println("BLOCKED BAD_CHANNEL");
    } else if (c.startsWith("set ssid ")) {
        if (running) { Serial.println("BLOCKED RUNNING"); return; }
        ssid = c.substring(9); Serial.printf("SSID=%s\n", ssid.c_str());
    } else if (c == "clone_start") {
        if (running) { Serial.println("ALREADY_RUNNING"); return; }
        if (!haveBssid || channel == 0) { Serial.println("BLOCKED NEED_BSSID_AND_CHANNEL"); return; }
        WiFi.mode(WIFI_AP);
        if (esp_wifi_set_mac(WIFI_IF_AP, bssid) != ESP_OK) { WiFi.mode(WIFI_OFF); Serial.println("BLOCKED SET_MAC"); return; }
        if (!WiFi.softAP(ssid.c_str(), kPassword, channel, false, 4)) { WiFi.mode(WIFI_OFF); Serial.println("BLOCKED SOFTAP"); return; }
        esp_wifi_set_max_tx_power(kMaxTxPowerQdbm);
        running = true;
        uint8_t cur[6] = {}; esp_wifi_get_mac(WIFI_IF_AP, cur);
        Serial.printf("CLONE_STARTED bssid=%s channel=%d ssid=%s (OUR-OWN-AP ONLY)\n", macStr(cur).c_str(), channel, ssid.c_str());
    } else if (c == "clone_stop" || c == "stop") {
        stopClone("operator");
    } else {
        Serial.println("Unknown; commands: status | set bssid <mac> | set channel <n> | set ssid <s> | clone_start | clone_stop");
    }
}

void setup() {
    Serial.begin(115200);
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    Serial.println("RFMON evil-twin beacon source (OUR-OWN-AP validation only); idle");
    Serial.println("Commands: status | set bssid <mac> | set channel <n> | set ssid <s> | clone_start | clone_stop");
}

void loop() {
    while (Serial.available()) {
        const char ch = char(Serial.read());
        if (ch == '\n') { command.trim(); if (command.length()) handle(command); command = ""; }
        else if (ch != '\r') { if (command.length() < 80) command += ch; else command = "invalid"; }
    }
}
