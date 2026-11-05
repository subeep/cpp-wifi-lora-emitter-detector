// RF identity rules (security/wifi_security_rf_identity.hpp): the tracker on
// synthetic carrier offsets, and the monitor path through SecurityState with
// capture accounting; no radio.
#include "security/wifi_security_state.hpp"
#include "security/wifi_security_io.hpp"
#include "wifi_frame.hpp"
#include <algorithm>
#include <iostream>
#include <random>
using namespace rfmon;
using namespace rfmon::wifi_security;

int failures = 0, checks = 0;
void check(bool ok, const std::string& label) { ++checks; std::cout << (ok ? "PASS " : "FAIL ") << label << '\n'; if (!ok) ++failures; }

std::mt19937 rng(11);
double jitter(double ppm) { return std::normal_distribution<double>(0, ppm)(rng); }

RfObservation obs(double t_s, double cfo, bool disconnect = false, const std::string& cls = "ofdm-short", const std::string& ta = "00:11:22:33:44:55") {
    RfObservation o;
    o.band = "wifi_2g4"; o.channel = 6; o.transmitter = ta; o.receiver = "ff:ff:ff:ff:ff:ff"; o.bssid = ta;
    o.disconnect = disconnect; o.length_class = cls; o.cfo_ppm = cfo; o.t_ns = int64_t(t_s * 1e9);
    return o;
}
size_t count(const std::vector<RfFinding>& v, RfFinding::Kind k) { return size_t(std::count_if(v.begin(), v.end(), [&](const auto& f) { return f.kind == k; })); }

// The flood test's management header fixture; subtype and sequence varied, FCS refreshed.
std::vector<uint8_t> mpdu(uint8_t subtype_byte, int seq) {
    std::vector<uint8_t> b;
    from_hex("c0003a01ffffffffffff00112233445500112233445530120700713d26fa", b);
    b[0] = subtype_byte; b[22] = uint8_t((seq << 4) & 255); b[23] = uint8_t((seq >> 4) & 255);
    const auto fcs = wifi::fcs32(b.data(), b.size() - 4);
    for (int i = 0; i < 4; ++i) b[b.size() - 4 + i] = uint8_t(fcs >> (8 * i));
    return b;
}
CaptureRecord capture(uint64_t seq) {
    CaptureRecord c; c.run_id = "rf-identity-test"; c.capture_seq = seq; c.radio_session = 1;
    c.band = "wifi_2g4"; c.channel = 6; c.channel_hz = 2437e6; c.sample_rate_hz = 20e6;
    c.samples_received = c.analysed_samples = 10000000; c.processed = true;
    c.clock = ClockDomain::UsrpDevice; c.device_time_ns = int64_t(seq) * 600000000;
    c.host_before_ns = 1700000000000000000LL + c.device_time_ns; c.gain_db = 20;
    c.antenna = "RX2"; c.device = "test";
    return c;
}
// One capture: `normal` frames from the sender at `cfo`, then `disconnects`
// deauthentications at `disconnect_cfo`.
void feed(SecurityState& s, CaptureRecord c, int normal, double cfo, int disconnects = 0, double disconnect_cfo = 0) {
    int n = 0;
    auto emit = [&](uint8_t subtype, double ppm) {
        auto e = make_frame_event(c, 1000 + size_t(n) * 100000, 400, "OFDM", 6, false, mpdu(subtype, int(c.capture_seq) * 40 + n), true);
        wifi::FrameRfFeatures f;
        f.method = "OFDM data-aided"; f.symbols = 8;
        f.cfo_hz = f.cfo_early_hz = ppm * 1e-6 * e.channel_hz;
        f.snr_db = 25;
        attach_rf_features(e, f);
        s.ingest(e);
        ++n;
    };
    for (int i = 0; i < normal; ++i) emit(0x80, cfo + jitter(0.02));                 // beacon
    for (int i = 0; i < disconnects; ++i) emit(0xc0, disconnect_cfo + jitter(0.02)); // deauthentication
    c.events_submitted = n;
    s.ingest(c);
}
size_t incidents(const SecurityState& s, const std::string& rule) {
    const auto snap = s.snapshot();
    return size_t(std::count_if(snap.incidents.begin(), snap.incidents.end(), [&](const auto& i) { return i.rule == rule; }));
}

int main() {
    // Tracker: genuine disconnects match the sender's own carrier.
    {
        RfIdentityTracker t;
        for (int i = 0; i < 20; ++i) t.add(obs(0.1 * i, -3.0 + jitter(0.02)));
        for (int i = 0; i < 8; ++i) t.add(obs(2.0 + 0.05 * i, -3.0 + jitter(0.02), true));
        const auto f = t.evaluate();
        check(f.empty() && t.judged() == 8 && t.inconsistent() == 0, "genuine disconnects consistent with the sender");
    }
    // Tracker: a burst from a radio 0.8 ppm away is a forged disconnect.
    {
        RfIdentityTracker t;
        for (int i = 0; i < 20; ++i) t.add(obs(0.1 * i, -3.0 + jitter(0.02)));
        for (int i = 0; i < 8; ++i) t.add(obs(2.0 + 0.05 * i, -2.2 + jitter(0.02), true));
        const auto f = t.evaluate();
        check(count(f, RfFinding::Kind::ForgedDisconnect) == 1, "foreign-radio disconnect burst reported");
        if (!f.empty())
            check(f[0].inconsistent == 8 && f[0].evaluated == 8 && std::abs(f[0].suspect_ppm + 2.2) < 0.05 && std::abs(f[0].reference_ppm + 3.0) < 0.05,
                  "finding carries counts, suspect and reference offsets");
    }
    // Tracker: fewer than three inconsistent frames, or a minority, is not reported.
    {
        RfIdentityTracker t;
        for (int i = 0; i < 20; ++i) t.add(obs(0.1 * i, -3.0 + jitter(0.02)));
        t.add(obs(2.0, -2.0, true)); t.add(obs(2.1, -2.0, true));
        check(t.evaluate().empty() && t.inconsistent() == 2, "two inconsistent frames are not enough");
        for (int i = 0; i < 6; ++i) t.add(obs(2.2 + 0.05 * i, -3.0 + jitter(0.02), true));
        t.add(obs(2.6, -2.0, true));
        check(t.evaluate().empty(), "three of nine inconsistent (a minority) not reported");
    }
    // Tracker: no reference, no verdict.
    {
        RfIdentityTracker t;
        for (int i = 0; i < 3; ++i) t.add(obs(0.1 * i, -3.0));
        for (int i = 0; i < 6; ++i) t.add(obs(1.0 + 0.05 * i, 5.0, true));
        check(t.evaluate().empty() && t.unverifiable() == 6, "too few reference frames: unverifiable, not inconsistent");
        RfIdentityTracker u;
        for (int i = 0; i < 20; ++i) u.add(obs(0.1 * i, -3.0));
        for (int i = 0; i < 6; ++i) u.add(obs(90.0 + 0.05 * i, 5.0, true));
        check(u.evaluate().empty() && u.unverifiable() == 6, "reference older than the fallback window is not used");
        RfIdentityTracker v;
        for (int i = 0; i < 20; ++i) v.add(obs(0.1 * i, -3.0 + jitter(0.02)));
        for (int i = 0; i < 6; ++i) v.add(obs(40.0 + 0.05 * i, -1.5, true));
        check(count(v.evaluate(), RfFinding::Kind::ForgedDisconnect) == 1, "quiet sender: frames from the last minute used as reference");
    }
    // Tracker: the short/long allowance only applies when the class is missing.
    {
        RfIdentityTracker t;
        for (int i = 0; i < 20; ++i) t.add(obs(0.1 * i, -3.0 + jitter(0.01), false, "ofdm-long"));
        for (int i = 0; i < 6; ++i) t.add(obs(2.0 + 0.05 * i, -3.12 + jitter(0.01), true, "ofdm-short"));
        check(t.evaluate().empty(), "0.12 ppm short/long device offset tolerated across classes");
    }
    // Tracker: two radios interleaving under one address.
    {
        RfIdentityTracker t;
        for (int i = 0; i < 24; ++i) t.add(obs(0.1 * i, (i % 3 ? -3.0 : 1.5) + jitter(0.02)));
        const auto f = t.evaluate();
        check(count(f, RfFinding::Kind::TwoTransmitters) == 1, "interleaved clusters 4.5 ppm apart: two transmitters");
        if (!f.empty())
            check(f[0].a_frames == 16 && f[0].b_frames == 8 && f[0].alternations >= 4, "cluster sizes and alternations");
    }
    // Tracker: a clone sending other frame lengths than the owner (pooled classes).
    {
        RfIdentityTracker t;
        for (int i = 0; i < 24; ++i) {
            const bool clone = i % 3 == 0;
            t.add(obs(0.1 * i, (clone ? 1.5 : -3.0) + jitter(0.02), false, clone ? "ofdm-long" : "ofdm-short"));
        }
        check(count(t.evaluate(), RfFinding::Kind::TwoTransmitters) == 1, "clone in another length class still found");
    }
    // Tracker: one step change (re-tune, temperature, a replaced device) is not two transmitters.
    {
        RfIdentityTracker t;
        for (int i = 0; i < 24; ++i) t.add(obs(0.1 * i, (i < 12 ? -3.0 : -1.0) + jitter(0.02)));
        check(t.evaluate().empty(), "single step change not reported");
    }
    // Roles: a client pre-corrects to its AP and re-adjusts in steps (live: up to 1.1 ppm).
    auto client = [](double t_s, double cfo, bool disconnect = false) {
        auto o = obs(t_s, cfo, disconnect, "ofdm-short", "aa:bb:cc:00:00:01");
        o.bssid = "00:11:22:33:44:55";            // its AP
        o.client_frame = !disconnect;
        return o;
    };
    {
        RfIdentityTracker t;
        for (int i = 0; i < 30; ++i) t.add(client(0.1 * i, (i / 3 % 2 ? -8.6 : -9.6) + jitter(0.02)));
        check(count(t.evaluate(), RfFinding::Kind::TwoTransmitters) == 0, "client stepping 1 ppm back and forth: not two transmitters");
    }
    {
        RfIdentityTracker t;
        for (int i = 0; i < 20; ++i) t.add(client(0.1 * i, -9.6 + jitter(0.02)));
        for (int i = 0; i < 6; ++i) t.add(client(2.0 + 0.05 * i, -8.5 + jitter(0.02), true));
        check(t.evaluate().empty() && t.inconsistent() == 0, "client-claimed disconnects 1.1 ppm off: within the client tolerance");
        for (int i = 0; i < 6; ++i) t.add(client(2.5 + 0.05 * i, -6.5 + jitter(0.02), true));
        const auto f = t.evaluate();
        check(count(f, RfFinding::Kind::ForgedDisconnect) == 1 && f[0].claimed_client, "client-claimed disconnects 3 ppm off: forged, marked as a client claim");
    }
    {
        RfIdentityTracker t;
        for (int i = 0; i < 20; ++i) t.add(obs(0.1 * i, -3.0 + jitter(0.02)));
        for (int i = 0; i < 6; ++i) t.add(obs(2.0 + 0.05 * i, -2.4 + jitter(0.02), true));
        const auto f = t.evaluate();
        check(count(f, RfFinding::Kind::ForgedDisconnect) == 1 && !f[0].claimed_client, "AP-claimed disconnects 0.6 ppm off: forged");
    }
    {
        RfIdentityTracker t;  // no BSSID ever seen as its own: role unknown, no cluster rule
        for (int i = 0; i < 24; ++i) { auto o = obs(0.1 * i, (i % 3 ? -3.0 : 1.5) + jitter(0.02)); o.bssid.clear(); t.add(o); }
        check(t.evaluate().empty(), "unknown role: two-transmitter rule not run");
    }

    // Tracker: one noisy radio is not two transmitters.
    {
        RfIdentityTracker t;
        for (int i = 0; i < 40; ++i) t.add(obs(0.1 * i, -3.0 + jitter(0.1)));
        check(t.evaluate().empty(), "one radio with 0.1 ppm jitter not split");
    }
    // Tracker: a short/long device offset alone is not two transmitters.
    {
        RfIdentityTracker t;
        for (int i = 0; i < 30; ++i) t.add(obs(0.1 * i, (i % 2 ? -3.0 : -3.12) + jitter(0.01), false, i % 2 ? "ofdm-long" : "ofdm-short"));
        check(t.evaluate().empty(), "0.12 ppm short/long offset not split when classes are pooled");
    }

    // Monitor path: rule wired into SecurityState with capture accounting.
    StateLimits limits;
    limits.flood.enabled = false;
    {
        SecurityState s(limits);
        for (uint64_t c = 1; c <= 3; ++c) feed(s, capture(c), 6, -4.0);
        feed(s, capture(4), 2, -4.0, 6, -4.0);
        const auto snap = s.snapshot();
        check(snap.rf_identity_frames == 26 && snap.rf_identity_judged == 6 && snap.rf_identity_inconsistent == 0, "monitor: genuine deauths judged consistent");
        check(incidents(s, "rf_forged_disconnect") == 0, "monitor: no incident for genuine deauths");
    }
    {
        SecurityState s(limits);
        for (uint64_t c = 1; c <= 3; ++c) feed(s, capture(c), 6, -4.0);
        feed(s, capture(4), 2, -4.0, 6, -2.5);
        check(incidents(s, "rf_forged_disconnect") == 1, "monitor: forged deauth burst raises rf_forged_disconnect");
        const auto snap = s.snapshot();
        if (!snap.incidents.empty()) {
            const auto& i = *std::find_if(snap.incidents.begin(), snap.incidents.end(), [](const auto& x) { return x.rule == "rf_forged_disconnect"; });
            check(i.claimed_source == "00:11:22:33:44:55" && i.severity == IncidentSeverity::High && !i.first_evidence.empty(),
                  "monitor: incident names the claimed sender, with evidence");
        }
    }
    {
        SecurityState s(limits);
        for (uint64_t c = 1; c <= 3; ++c) feed(s, capture(c), 6, -4.0);
        auto bad = capture(4);
        bad.overflows.push_back({9000000, std::nullopt});  // after the frames, so they keep their time
        feed(s, bad, 2, -4.0, 6, -2.5);
        check(incidents(s, "rf_forged_disconnect") == 0 && s.snapshot().rf_identity_excluded == 8, "monitor: degraded capture excluded, never judged (" + std::to_string(s.snapshot().rf_identity_excluded) + " frames, " + std::to_string(s.snapshot().rf_identity_judged) + " judged)");
    }
    {
        StateLimits off = limits;
        off.rf_identity.enabled = false;
        SecurityState s(off);
        for (uint64_t c = 1; c <= 3; ++c) feed(s, capture(c), 6, -4.0);
        feed(s, capture(4), 2, -4.0, 6, -2.5);
        check(incidents(s, "rf_forged_disconnect") == 0 && !s.snapshot().rf_identity_enabled, "monitor: rule can be disabled");
    }
    {
        // Control frames (here RTS, 0xb4, with the sender as transmitter) are not used.
        SecurityState s(limits);
        auto c = capture(1);
        for (int i = 0; i < 4; ++i) {
            std::vector<uint8_t> b;
            from_hex("b4000000ffffffffffff00112233445500000000", b);
            const auto fcs = wifi::fcs32(b.data(), b.size() - 4);
            for (int k = 0; k < 4; ++k) b[b.size() - 4 + k] = uint8_t(fcs >> (8 * k));
            auto e = make_frame_event(c, 1000 + size_t(i) * 100000, 400, "OFDM", 6, false, b, true);
            wifi::FrameRfFeatures f;
            f.method = "OFDM data-aided"; f.symbols = 8; f.cfo_hz = f.cfo_early_hz = -4.0e-6 * e.channel_hz;
            attach_rf_features(e, f);
            s.ingest(e);
        }
        c.events_submitted = 4;
        s.ingest(c);
        check(s.snapshot().frames_accepted == 4 && s.snapshot().rf_identity_frames == 0, "monitor: control frames not used");
    }
    std::cout << checks - failures << "/" << checks << " checks passed\n";
    return failures ? 1 : 0;
}
