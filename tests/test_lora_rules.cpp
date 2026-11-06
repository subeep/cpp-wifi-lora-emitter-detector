// Keyless LoRaWAN rules (src/security/lora_rules.cpp) on synthetic frames.
#include "security/lora_rules.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

using namespace rfmon::lora_rules;

namespace {
int failures = 0;
void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s [%s] %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    failures += !ok;
}
// Unconfirmed uplink: MHDR 0x40, DevAddr (LE), FCtrl, FCnt (LE), FPort, data, MIC.
Frame uplink(uint32_t devaddr, uint16_t fcnt, uint8_t data, double ts, std::optional<int> cluster = std::nullopt,
             uint64_t capture = 0) {
    Frame f;
    f.payload = {0x40, uint8_t(devaddr), uint8_t(devaddr >> 8), uint8_t(devaddr >> 16), uint8_t(devaddr >> 24), 0x00,
                 uint8_t(fcnt), uint8_t(fcnt >> 8), 0x01, data, 0x55, 0xA1, 0xB2, 0xC3, uint8_t(data ^ fcnt)};
    f.ts = ts;
    f.freq_hz = 865.0625e6;
    f.bw_hz = 125e3;
    f.sf = 7;
    f.rf_cluster = cluster;
    f.capture_seq = capture ? capture : uint64_t(ts * 10);
    return f;
}
Frame join(uint8_t dev, uint16_t nonce, double ts) {
    Frame f;
    f.payload = {0x00};
    for (int i = 0; i < 8; ++i) f.payload.push_back(uint8_t(0x70 + i));      // JoinEUI
    for (int i = 0; i < 8; ++i) f.payload.push_back(uint8_t(i ? 0x10 : dev)); // DevEUI
    f.payload.push_back(uint8_t(nonce));
    f.payload.push_back(uint8_t(nonce >> 8));
    for (int i = 0; i < 4; ++i) f.payload.push_back(uint8_t(0xE0 + i));
    f.ts = ts;
    f.freq_hz = 865.4025e6;
    f.bw_hz = 125e3;
    f.sf = 10;
    f.capture_seq = uint64_t(ts * 10) + dev;
    return f;
}
size_t count(const Snapshot& s, const char* rule) {
    size_t n = 0;
    for (const auto& i : s.incidents) n += i.rule == rule;
    return n;
}
}  // namespace

int main() {
    {  // Normal traffic with counter gaps (missed frames) raises nothing.
        Engine e;
        for (uint16_t n : {1, 2, 5, 9, 40, 41}) e.ingest(uplink(0x26011234, n, uint8_t(n), 100.0 + n * 30));
        auto s = e.snapshot();
        check(s.incidents.empty(), "gaps_are_not_incidents", std::to_string(s.incidents.size()));
        check(s.stats.data_frames == 6, "data_frames_counted", std::to_string(s.stats.data_frames));
    }
    {  // Identical bytes inside the retransmission window are one transmission.
        Engine e;
        e.ingest(uplink(0x26011234, 7, 9, 100));
        e.ingest(uplink(0x26011234, 7, 9, 102));
        e.ingest(uplink(0x26011234, 7, 9, 130));
        auto s = e.snapshot();
        check(s.incidents.empty() && s.stats.retransmissions == 2, "retransmissions_ignored");
    }
    {  // Identical bytes long after: replay, reported once (no extra counter incident).
        Engine e;
        e.ingest(uplink(0x26011234, 7, 9, 100));
        e.ingest(uplink(0x26011234, 8, 3, 160));
        e.ingest(uplink(0x26011234, 7, 9, 900));
        auto s = e.snapshot();
        check(count(s, RULE_REPLAY) == 1 && s.incidents.size() == 1, "replay_detected_once",
              std::to_string(s.incidents.size()));
        check(s.incidents.front().subject == "DevAddr 26011234", "replay_subject", s.incidents.front().subject);
        e.ingest(uplink(0x26011234, 7, 9, 2000));
        s = e.snapshot();
        check(s.incidents.size() == 1 && s.incidents.front().count == 2, "open_incident_deduplicated");
        check(e.acknowledge(s.incidents.front().id) && !e.snapshot().incidents.front().open, "acknowledge_closes");
        e.clear_closed();
        check(e.snapshot().incidents.empty(), "clear_closed_removes");
    }
    {  // Counter going backwards with new content.
        Engine e;
        e.ingest(uplink(0x26011234, 50, 1, 100));
        e.ingest(uplink(0x26011234, 12, 2, 400));
        auto s = e.snapshot();
        check(count(s, RULE_FCNT_REGRESSION) == 1, "fcnt_regression");
    }
    {  // 16-bit wrap is forward progress.
        Engine e;
        e.ingest(uplink(0x26011234, 65530, 1, 100));
        e.ingest(uplink(0x26011234, 3, 2, 400));
        check(e.snapshot().incidents.empty(), "fcnt_wrap_not_regression");
    }
    {  // Same counter, different bytes.
        Engine e;
        e.ingest(uplink(0x26011234, 20, 1, 100));
        e.ingest(uplink(0x26011234, 21, 2, 130));
        e.ingest(uplink(0x26011234, 20, 7, 140));
        auto s = e.snapshot();
        check(count(s, RULE_FCNT_REUSE) == 1, "fcnt_reuse_with_different_bytes");
    }
    {  // Uplink and downlink counters are independent.
        Engine e;
        e.ingest(uplink(0x26011234, 50, 1, 100));
        Frame down = uplink(0x26011234, 3, 2, 110);
        down.payload[0] = 0x60;  // unconfirmed downlink
        e.ingest(down);
        check(e.snapshot().incidents.empty(), "directions_independent");
    }
    {  // DevNonce reuse, but not for a retransmitted copy.
        Engine e;
        e.ingest(join(1, 100, 10));
        e.ingest(join(1, 100, 12));
        e.ingest(join(1, 101, 300));
        check(e.snapshot().incidents.empty(), "join_retransmission_ok");
        e.ingest(join(1, 100, 900));
        auto s = e.snapshot();
        check(count(s, RULE_DEVNONCE_REUSE) == 1 && count(s, RULE_REPLAY) == 0, "devnonce_reuse");
    }
    {  // Join-request burst: threshold count inside the window, once per window.
        Engine e;
        for (int i = 0; i < int(JOIN_FLOOD_COUNT) + 5; ++i) e.ingest(join(uint8_t(i), uint16_t(i), 100.0 + i));
        auto s = e.snapshot();
        check(count(s, RULE_JOIN_FLOOD) == 1, "join_flood", std::to_string(count(s, RULE_JOIN_FLOOD)));
        Engine quiet;
        for (int i = 0; i < int(JOIN_FLOOD_COUNT) + 5; ++i) quiet.ingest(join(uint8_t(i), uint16_t(i), 100.0 + i * 10));
        check(quiet.snapshot().incidents.empty(), "spread_joins_not_flood");
    }
    {  // RF mismatch only after a baseline, and remap follows merges.
        Engine e;
        e.ingest(uplink(0x26011234, 1, 1, 100, 4));
        e.ingest(uplink(0x26011234, 2, 2, 200, 9));  // no baseline yet
        check(e.snapshot().incidents.empty(), "rf_needs_baseline");
        for (uint16_t n = 3; n < 8; ++n) e.ingest(uplink(0x26011234, n, uint8_t(n), 100.0 * n, 4));
        e.ingest(uplink(0x26011234, 9, 9, 1000, 9));
        auto s = e.snapshot();
        check(count(s, RULE_RF_MISMATCH) == 1, "rf_mismatch");
        Engine m;
        for (uint16_t n = 1; n < 6; ++n) m.ingest(uplink(0x26015555, n, uint8_t(n), 100.0 * n, 4));
        m.remap_cluster(4, 2);
        m.ingest(uplink(0x26015555, 7, 7, 900, 2));
        check(m.snapshot().incidents.empty(), "remap_cluster_keeps_baseline");
    }
    {  // Disallowed emitter and non-LoRaWAN payloads.
        Engine e;
        e.note_disallowed_emitter(3, "Rogue", 100, 866.9e6);
        Frame text;
        text.payload = {'A', 'u', 't', 'o'};
        text.ts = 101;
        e.ingest(text);
        auto s = e.snapshot();
        check(count(s, RULE_DISALLOWED) == 1 && s.incidents.front().subject == "LORA-0003", "disallowed_emitter");
        check(s.stats.ignored == 1, "non_lorawan_ignored");
    }
    if (failures) {
        std::printf("%d LoRa rule check(s) failed\n", failures);
        return 1;
    }
    std::printf("All LoRa rule checks passed\n");
}
