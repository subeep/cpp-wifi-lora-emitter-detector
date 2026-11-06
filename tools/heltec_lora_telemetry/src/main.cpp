// Heltec WiFi LoRa 32 V3 LoRa test transmitter (receive side: the X310 LoRa tab).
//
// Every transmission prints one JSON line on serial (115200 baud), so the host
// has exact ground truth: what was sent, when, where and how.
//
// Modes (autonomous, one packet per interval, hopping the channel list):
//   TEL  ASCII telemetry tagged with the run id: "HLT<run> #<seq> up=.. t=.. heap=.."
//   WAN  LoRaWAN 1.0.x unconfirmed uplinks (ABP, test keys, real AES payload
//        encryption and MIC), FPort 2, carrying the same telemetry text.
//   OFF  silent; only commands transmit.
// Commands (one per line):
//   MODE TEL|WAN|OFF        CH <MHz>[,<MHz>...]      SF <7-12>    BW <125|250|500>
//   CR <5-8>                PWR <dBm -9..22>         INT <ms>     JIT <ms>
//   RUN <tag>               FCNT <n>                 SYNC <hex>
//   ONCE                    send one packet in the current mode now
//   RAW <MHz> <hex>         send exact bytes once on <MHz>
//   WANF <fcnt> <hex>       send one LoRaWAN uplink with this FCnt and plaintext payload
//   JOIN <devnonce>         send one join request with this DevNonce (test AppKey MIC)
//   STATUS                  print the configuration as JSON
// Test identities only: DevAddr 26FFBEEF, keys below. Nothing here targets a real network.
#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <mbedtls/aes.h>

namespace {

// Heltec V3 SX1262 wiring.
constexpr int PIN_NSS = 8, PIN_DIO1 = 14, PIN_RST = 12, PIN_BUSY = 13;
constexpr int PIN_SCK = 9, PIN_MISO = 11, PIN_MOSI = 10;

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY);

// LoRaWAN test identity (ABP) and join identity. Test values only.
constexpr uint32_t DEV_ADDR = 0x26FFBEEF;
const uint8_t NWK_SKEY[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6, 0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};
const uint8_t APP_SKEY[16] = {0x3C, 0x4F, 0xCF, 0x09, 0x88, 0x15, 0xF7, 0xAB, 0xA6, 0xD2, 0xAE, 0x28, 0x16, 0x15, 0x7E, 0x2B};
const uint8_t APP_KEY[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
const uint8_t JOIN_EUI[8] = {0x70, 0xB3, 0xD5, 0x7E, 0xD0, 0x00, 0x00, 0x01};  // MSB first
uint8_t dev_eui[8];                                                          // from the chip MAC, MSB first

struct Config {
    String mode = "TEL";
    float channels[16] = {865.0625f, 865.4025f, 865.985f, 866.9f};
    int n_channels = 4;
    int sf = 7, cr = 5, power = 0;
    float bw = 125.0f;
    uint8_t sync = 0x12;
    uint32_t interval_ms = 6000, jitter_ms = 1000;
    String run = "0000";
} cfg;

uint32_t seq = 0, fcnt = 0, next_tx_ms = 0;
int hop = 0;
float radio_freq = 0, radio_bw = 0;
int radio_sf = 0, radio_cr = 0, radio_power = -100;
uint8_t radio_sync = 0;

void aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

// RFC 4493 AES-CMAC.
void cmac(const uint8_t key[16], const uint8_t* msg, size_t len, uint8_t mac[16]) {
    uint8_t zero[16] = {0}, l[16], k1[16], k2[16];
    aes_block(key, zero, l);
    auto shift = [](const uint8_t in[16], uint8_t out[16]) {
        uint8_t carry = 0;
        for (int i = 15; i >= 0; --i) { out[i] = uint8_t((in[i] << 1) | carry); carry = in[i] >> 7; }
        if (in[0] & 0x80) out[15] ^= 0x87;
    };
    shift(l, k1);
    shift(k1, k2);
    const size_t n = len ? (len + 15) / 16 : 1;
    const bool complete = len && len % 16 == 0;
    uint8_t x[16] = {0}, y[16];
    for (size_t b = 0; b < n; ++b) {
        uint8_t block[16];
        for (int i = 0; i < 16; ++i) {
            const size_t at = b * 16 + i;
            if (b + 1 < n) block[i] = msg[at];
            else if (complete) block[i] = msg[at] ^ k1[i];
            else block[i] = (at < len ? msg[at] : at == len ? 0x80 : 0x00) ^ k2[i];
        }
        for (int i = 0; i < 16; ++i) y[i] = x[i] ^ block[i];
        aes_block(key, y, x);
    }
    memcpy(mac, x, 16);
}

String hex(const uint8_t* b, size_t n) {
    static const char* d = "0123456789ABCDEF";
    String s;
    for (size_t i = 0; i < n; ++i) { s += d[b[i] >> 4]; s += d[b[i] & 15]; }
    return s;
}

int parse_hex(const String& text, uint8_t* out, int max) {
    int n = 0, high = -1;
    for (char c : text) {
        if (c == ' ') continue;
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return -1;
        if (high < 0) high = v;
        else { if (n >= max) return -1; out[n++] = uint8_t(high * 16 + v); high = -1; }
    }
    return high < 0 ? n : -1;
}

// LoRaWAN 1.0.x unconfirmed uplink, FPort 2.
int build_uplink(uint32_t counter, const uint8_t* plain, size_t plen, uint8_t* out) {
    int n = 0;
    out[n++] = 0x40;
    for (int i = 0; i < 4; ++i) out[n++] = uint8_t(DEV_ADDR >> (8 * i));
    out[n++] = 0x00;
    out[n++] = uint8_t(counter);
    out[n++] = uint8_t(counter >> 8);
    out[n++] = 0x02;
    for (size_t blk = 0; blk * 16 < plen; ++blk) {
        uint8_t a[16] = {0x01, 0, 0, 0, 0, 0x00};
        for (int i = 0; i < 4; ++i) a[6 + i] = uint8_t(DEV_ADDR >> (8 * i));
        for (int i = 0; i < 4; ++i) a[10 + i] = uint8_t(counter >> (8 * i));
        a[15] = uint8_t(blk + 1);
        uint8_t s[16];
        aes_block(APP_SKEY, a, s);
        for (size_t i = 0; i < 16 && blk * 16 + i < plen; ++i) out[n++] = plain[blk * 16 + i] ^ s[i];
    }
    uint8_t b0[16 + 255] = {0x49, 0, 0, 0, 0, 0x00};
    for (int i = 0; i < 4; ++i) b0[6 + i] = uint8_t(DEV_ADDR >> (8 * i));
    for (int i = 0; i < 4; ++i) b0[10 + i] = uint8_t(counter >> (8 * i));
    b0[15] = uint8_t(n);
    memcpy(b0 + 16, out, n);
    uint8_t mac[16];
    cmac(NWK_SKEY, b0, 16 + n, mac);
    memcpy(out + n, mac, 4);
    return n + 4;
}

int build_join(uint16_t nonce, uint8_t* out) {
    int n = 0;
    out[n++] = 0x00;
    for (int i = 7; i >= 0; --i) out[n++] = JOIN_EUI[i];  // little endian on air
    for (int i = 7; i >= 0; --i) out[n++] = dev_eui[i];
    out[n++] = uint8_t(nonce);
    out[n++] = uint8_t(nonce >> 8);
    uint8_t mac[16];
    cmac(APP_KEY, out, n, mac);
    memcpy(out + n, mac, 4);
    return n + 4;
}

bool apply_radio(float freq) {
    int st = RADIOLIB_ERR_NONE;
    if (radio_bw != cfg.bw) { st |= radio.setBandwidth(cfg.bw); radio_bw = cfg.bw; }
    if (radio_sf != cfg.sf) { st |= radio.setSpreadingFactor(cfg.sf); radio_sf = cfg.sf; }
    if (radio_cr != cfg.cr) { st |= radio.setCodingRate(cfg.cr); radio_cr = cfg.cr; }
    if (radio_power != cfg.power) { st |= radio.setOutputPower(cfg.power); radio_power = cfg.power; }
    if (radio_sync != cfg.sync) { st |= radio.setSyncWord(cfg.sync); radio_sync = cfg.sync; }
    if (radio_freq != freq) { st |= radio.setFrequency(freq); radio_freq = freq; }
    return st == RADIOLIB_ERR_NONE;
}

void send(const char* kind, float freq, const uint8_t* bytes, size_t len, long counter = -1) {
    if (!apply_radio(freq)) {
        Serial.printf("{\"ev\":\"err\",\"what\":\"radio config\",\"freq\":%.4f,\"sf\":%d,\"bw\":%.0f}\n", freq, cfg.sf, cfg.bw);
        return;
    }
    const uint32_t start = millis();
    const int st = radio.transmit(const_cast<uint8_t*>(bytes), len);
    const uint32_t end = millis();
    const uint32_t air_us = radio.getTimeOnAir(len);
    ++seq;
    Serial.printf("{\"ev\":\"tx\",\"run\":\"%s\",\"seq\":%lu,\"kind\":\"%s\",\"ms_start\":%lu,\"ms_end\":%lu,\"freq\":%.4f,"
                  "\"sf\":%d,\"bw\":%.0f,\"cr\":%d,\"pwr\":%d,\"sync\":%u,\"len\":%u,\"air_ms\":%.1f,\"fcnt\":%ld,"
                  "\"status\":%d,\"hex\":\"%s\"}\n",
                  cfg.run.c_str(), (unsigned long)seq, kind, (unsigned long)start, (unsigned long)end, freq, cfg.sf, cfg.bw,
                  cfg.cr, cfg.power, cfg.sync, (unsigned)len, air_us / 1000.0, counter, st, hex(bytes, len).c_str());
}

String telemetry() {
    char b[96];
    snprintf(b, sizeof b, "HLT%s #%lu up=%lu t=%.1f heap=%lu", cfg.run.c_str(), (unsigned long)(seq + 1),
             (unsigned long)(millis() / 1000), temperatureRead(), (unsigned long)(ESP.getFreeHeap() / 1024));
    return b;
}

float next_channel() {
    const float f = cfg.channels[hop % cfg.n_channels];
    hop = (hop + 1) % cfg.n_channels;
    return f;
}

void send_mode_packet() {
    const String text = telemetry();
    const float f = next_channel();
    if (cfg.mode == "WAN") {
        uint8_t frame[255];
        const int n = build_uplink(fcnt, (const uint8_t*)text.c_str(), text.length(), frame);
        send("wan", f, frame, n, long(fcnt));
        ++fcnt;
    } else {
        send("tel", f, (const uint8_t*)text.c_str(), text.length());
    }
}

void status() {
    String ch;
    for (int i = 0; i < cfg.n_channels; ++i) ch += (i ? "," : "") + String(cfg.channels[i], 4);
    Serial.printf("{\"ev\":\"status\",\"run\":\"%s\",\"mode\":\"%s\",\"channels\":\"%s\",\"sf\":%d,\"bw\":%.0f,\"cr\":%d,"
                  "\"pwr\":%d,\"sync\":%u,\"int_ms\":%lu,\"jit_ms\":%lu,\"fcnt\":%lu,\"seq\":%lu,\"dev_addr\":\"%08lX\","
                  "\"dev_eui\":\"%s\",\"ms\":%lu}\n",
                  cfg.run.c_str(), cfg.mode.c_str(), ch.c_str(), cfg.sf, cfg.bw, cfg.cr, cfg.power, cfg.sync,
                  (unsigned long)cfg.interval_ms, (unsigned long)cfg.jitter_ms, (unsigned long)fcnt, (unsigned long)seq,
                  (unsigned long)DEV_ADDR, hex(dev_eui, 8).c_str(), (unsigned long)millis());
}

void reply(bool ok, const String& what) {
    Serial.printf("{\"ev\":\"%s\",\"cmd\":\"%s\"}\n", ok ? "ok" : "bad", what.c_str());
}

void command(String line) {
    line.trim();
    if (!line.length()) return;
    const int sp = line.indexOf(' ');
    String op = sp < 0 ? line : line.substring(0, sp), arg = sp < 0 ? "" : line.substring(sp + 1);
    op.toUpperCase();
    arg.trim();
    bool ok = true;
    if (op == "MODE") { arg.toUpperCase(); ok = arg == "TEL" || arg == "WAN" || arg == "OFF"; if (ok) cfg.mode = arg; next_tx_ms = millis() + 500; }
    else if (op == "CH") {
        int n = 0, from = 0;
        float tmp[16];
        while (from <= (int)arg.length() && n < 16) {
            int comma = arg.indexOf(',', from);
            if (comma < 0) comma = arg.length();
            const float f = arg.substring(from, comma).toFloat();
            if (f < 150 || f > 960) { ok = false; break; }
            tmp[n++] = f;
            from = comma + 1;
        }
        ok = ok && n > 0;
        if (ok) { memcpy(cfg.channels, tmp, sizeof(float) * n); cfg.n_channels = n; hop = 0; }
    }
    else if (op == "SF") { const int v = arg.toInt(); ok = v >= 7 && v <= 12; if (ok) cfg.sf = v; }
    else if (op == "BW") { const float v = arg.toFloat(); ok = v == 125 || v == 250 || v == 500; if (ok) cfg.bw = v; }
    else if (op == "CR") { const int v = arg.toInt(); ok = v >= 5 && v <= 8; if (ok) cfg.cr = v; }
    else if (op == "PWR") { const int v = arg.toInt(); ok = v >= -9 && v <= 22; if (ok) cfg.power = v; }
    else if (op == "INT") { const long v = arg.toInt(); ok = v >= 200; if (ok) cfg.interval_ms = v; }
    else if (op == "JIT") { const long v = arg.toInt(); ok = v >= 0; if (ok) cfg.jitter_ms = v; }
    else if (op == "RUN") { ok = arg.length() >= 1 && arg.length() <= 8; if (ok) cfg.run = arg; }
    else if (op == "FCNT") { fcnt = uint32_t(arg.toInt()); }
    else if (op == "SYNC") { cfg.sync = uint8_t(strtol(arg.c_str(), nullptr, 16)); }
    else if (op == "ONCE") { send_mode_packet(); return; }
    else if (op == "RAW") {
        const int sp2 = arg.indexOf(' ');
        uint8_t b[255];
        const float f = arg.substring(0, sp2).toFloat();
        const int n = sp2 < 0 ? -1 : parse_hex(arg.substring(sp2 + 1), b, 255);
        ok = f >= 150 && f <= 960 && n > 0;
        if (ok) { send("raw", f, b, n); return; }
    }
    else if (op == "WANF") {
        const int sp2 = arg.indexOf(' ');
        uint8_t plain[200], frame[255];
        const long counter = arg.substring(0, sp2).toInt();
        const int n = sp2 < 0 ? -1 : parse_hex(arg.substring(sp2 + 1), plain, 200);
        ok = counter >= 0 && n > 0;
        if (ok) { send("wanf", next_channel(), frame, build_uplink(uint32_t(counter), plain, n, frame), counter); return; }
    }
    else if (op == "JOIN") {
        uint8_t frame[23];
        const long nonce = arg.toInt();
        ok = nonce >= 0 && nonce <= 65535;
        if (ok) { send("join", next_channel(), frame, build_join(uint16_t(nonce), frame), nonce); return; }
    }
    else if (op == "STATUS") { status(); return; }
    else ok = false;
    reply(ok, line);
    if (ok) status();
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    uint64_t mac = ESP.getEfuseMac();
    // DevEUI from the 48-bit MAC with FFFE inserted (EUI-64 style), MSB first.
    uint8_t m[6];
    for (int i = 0; i < 6; ++i) m[i] = uint8_t(mac >> (8 * i));
    const uint8_t eui[8] = {m[0], m[1], m[2], 0xFF, 0xFE, m[3], m[4], m[5]};
    memcpy(dev_eui, eui, 8);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
    const int st = radio.begin(cfg.channels[0], cfg.bw, cfg.sf, cfg.cr, cfg.sync, cfg.power, 8);
    radio_freq = cfg.channels[0]; radio_bw = cfg.bw; radio_sf = cfg.sf; radio_cr = cfg.cr; radio_power = cfg.power; radio_sync = cfg.sync;
    radio.setCRC(true);
    Serial.printf("{\"ev\":\"boot\",\"radio\":%d,\"fw\":\"heltec_lora_telemetry 1\",\"ms\":%lu}\n", st, (unsigned long)millis());
    cfg.mode = "OFF";  // silent until the host configures a run
    status();
}

void loop() {
    static String line;
    while (Serial.available()) {
        const char c = char(Serial.read());
        if (c == '\n' || c == '\r') { command(line); line = ""; }
        else if (line.length() < 600) line += c;
    }
    if (cfg.mode != "OFF" && int32_t(millis() - next_tx_ms) >= 0) {
        send_mode_packet();
        next_tx_ms = millis() + cfg.interval_ms + (cfg.jitter_ms ? esp_random() % cfg.jitter_ms : 0);
    }
}
