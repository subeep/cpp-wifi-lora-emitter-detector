// Package B tests (docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md): parallel
// burst pipeline determinism, receiver timeline / dead-time accounting,
// baseline windows (clean input only, freeze, reset/versioning, receiver-
// setting separation, persistence without pretending continuity), the
// incident store, and monitor persistence. No radio needed.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

#include "security/wifi_security_monitor.hpp"
#include "security/wifi_security_state.hpp"
#include "wifi_burst_pipeline.hpp"

using namespace rfmon;
using namespace rfmon::wifi_security;
namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool cond, const std::string& name, const std::string& detail = "") {
    std::printf("%s [%s]%s%s\n", cond ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    if (!cond) ++failures;
}

std::map<std::string, std::vector<uint8_t>> load_vectors() {
    std::map<std::string, std::vector<uint8_t>> v;
    std::ifstream in(std::string(PROJECT_ROOT_DIR) + "/tests/fixtures/wifi_security/frames.hex");
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        std::vector<uint8_t> b;
        if (tab != std::string::npos && from_hex(line.substr(tab + 1), b)) v[line.substr(0, tab)] = b;
    }
    return v;
}

// A contiguous 1 s capture on (band, channel) at device time t0 seconds.
CaptureRecord cap(uint64_t seq, double t0_s, int channel = 6, std::optional<double> gain = 20.0,
                  const std::string& run = "run-B", uint64_t session = 1) {
    CaptureRecord c;
    c.run_id = run;
    c.capture_seq = seq;
    c.radio_session = session;
    c.band = channel > 14 ? "wifi_5g" : "wifi_2g4";
    c.channel = channel;
    c.channel_hz = channel > 14 ? 5000e6 + 5e6 * channel : 2407e6 + 5e6 * channel;
    c.sample_rate_hz = 20e6;
    c.samples_requested = c.samples_received = c.analysed_samples = 20000000;
    c.gain_db = gain;
    c.antenna = "RX2";
    c.device = "addr=test";
    c.clock = ClockDomain::UsrpDevice;
    c.device_time_ns = int64_t(t0_s * 1e9);
    c.host_before_ns = 1'790'000'000'000'000'000 + c.device_time_ns;
    c.host_after_ns = c.host_before_ns + 100'000;
    c.processed = true;
    c.bursts_detected = 100;
    return c;
}

std::vector<std::complex<float>> read_cf32(const fs::path& p, size_t n) {
    std::vector<std::complex<float>> iq(n);
    std::ifstream in(p, std::ios::binary);
    std::vector<char> raw(n * 8);
    in.read(raw.data(), std::streamsize(raw.size()));
    for (size_t i = 0; i < n; ++i) {
        float v[2];
        for (int c = 0; c < 2; ++c) {
            uint32_t u = 0;
            for (int b = 0; b < 4; ++b) u |= uint32_t(uint8_t(raw[i * 8 + size_t(c) * 4 + size_t(b)])) << (8 * b);
            std::memcpy(&v[c], &u, 4);
        }
        iq[i] = {v[0], v[1]};
    }
    return iq;
}

}  // namespace

int main() {
    auto V = load_vectors();

    // ---- 1. parallel pipeline: identical results for any worker count -------
    {
        const fs::path dir = fs::path(PROJECT_ROOT_DIR) / "tests/fixtures/wifi_ofdm";
        nlohmann::json j;
        std::ifstream(dir / "airtel-2g4.json") >> j;
        const size_t n = j.at("samples");
        auto iq = read_cf32(dir / j.at("iq_file").get<std::string>(), n);
        std::vector<uint8_t> expected;
        from_hex(j.at("expected_mpdu_hex").get<std::string>(), expected);
        // The same real received beacon offered as many bursts at once.
        std::vector<wifi::BurstWindow> bursts(64, wifi::BurstWindow{0, n, 0.0});
        const double rate = j.at("sample_rate_hz"), center = j.at("capture_center_hz"), ch = j.at("channel_hz");
        auto one = wifi::process_bursts(iq.data(), n, bursts, rate, center, ch, true, 1);
        auto many = wifi::process_bursts(iq.data(), n, bursts, rate, center, ch, true, 8);
        bool same = one.size() == many.size(), decoded = true;
        for (size_t i = 0; i < one.size() && same; ++i) {
            same = one[i].cls.outcome == many[i].cls.outcome && bool(one[i].ofdm) == bool(many[i].ofdm) &&
                   (!one[i].ofdm || one[i].ofdm->mpdu == many[i].ofdm->mpdu);
            decoded = decoded && many[i].ofdm && many[i].ofdm->fcs_valid && many[i].ofdm->mpdu == expected;
        }
        check(same, "parallel_equals_sequential", "64 bursts, 1 vs 8 workers");
        check(decoded, "parallel_decodes_real_beacon", "every copy decodes the recorded MPDU");
        check(wifi::default_burst_workers() >= 1 && wifi::default_burst_workers() <= 8, "worker_count_bounded",
              std::to_string(wifi::default_burst_workers()));
    }

    // ---- 2. timeline / dead time ---------------------------------------------
    {
        SecurityState st;
        st.ingest(cap(1, 0.0));    // session start: no gap measurable
        st.ingest(cap(2, 1.1));    // 0.1 s gap
        st.ingest(cap(3, 2.4));    // 0.3 s gap
        CaptureRecord ovf = cap(4, 3.4);  // 0.0 s gap, then an unknown-length overflow
        ovf.overflows.push_back({1000, std::nullopt});
        st.ingest(ovf);
        st.ingest(cap(5, 9.0));    // after unknown gap: not measured
        st.ingest(cap(6, 9.5));    // starts before previous end: out of order
        st.ingest(cap(7, 0.0, 6, 20.0, "run-B", 2));  // new radio session
        CaptureRecord same = cap(8, 1.2, 6, 20.0, "run-B", 2);
        same.retuned = false;
        st.ingest(same);           // 0.2 s gap, retune skipped
        const auto& t = st.snapshot().timeline;
        check(t.radio_sessions == 2 && t.gaps == 4 && t.unknown_gaps == 1 && t.out_of_order == 1,
              "gap_classification", "gaps=" + std::to_string(t.gaps) + " unknown=" + std::to_string(t.unknown_gaps) +
                  " ooo=" + std::to_string(t.out_of_order));
        check(std::abs(t.dead_s - 0.6) < 1e-6 && std::abs(t.max_gap_s - 0.3) < 1e-6, "dead_time_measured",
              std::to_string(t.dead_s));
        check(std::abs(t.sampled_s - 8.0) < 1e-9 && std::abs(t.duty() - 8.0 / 8.6) < 1e-9, "duty_cycle",
              std::to_string(t.duty()));
        check(t.retune_skipped == 1, "retune_skips_counted");
        check(std::abs(st.snapshot().coverage.at({"wifi_2g4", 6}).dead_s - 0.6) < 1e-6, "dead_time_per_channel");
    }

    // ---- 3. baselines ----------------------------------------------------------
    {
        StateLimits lim;
        lim.baseline.window_analysed_s = 10.0;
        lim.baseline.max_windows = 5;
        SecurityState st(lim);
        uint64_t seq = 0;
        // 20 windows of 10 s; window w carries (w+1) deauths per second.
        for (int w = 0; w < 20; ++w) {
            for (int s = 0; s < 10; ++s) {
                CaptureRecord c = cap(++seq, double(seq) * 1.2);
                for (int k = 0; k <= w; ++k)
                    st.ingest(make_frame_event(c, size_t(1000 + k * 5000), 400, "DSSS", 1, false,
                                               V.at("deauth_broadcast_reason7"), true));
                c.events_submitted = size_t(w+1);
                st.ingest(c);
            }
        }
        auto bl = st.snapshot().baselines;
        check(bl.size() == 1 && bl[0].windows_closed == 20 && bl[0].windows_retained == 5, "windows_close_and_are_bounded",
              std::to_string(bl.empty() ? 0 : bl[0].windows_closed));
        const auto& r = bl[0].rates[size_t(BaselineMetric::Deauth)];
        // Retained windows carry 16..20 deauths/s.
        check(std::abs(r.p50 - 18) < 1e-9 && std::abs(r.p95 - 20) < 1e-9 && std::abs(r.mean - 18) < 1e-9 &&
                  std::abs(r.max - 20) < 1e-9,
              "rate_percentiles", "p50=" + std::to_string(r.p50) + " p95=" + std::to_string(r.p95));
        check(bl[0].rates[size_t(BaselineMetric::DeauthBroadcast)].max == 20, "broadcast_deauth_counted");
        check(std::abs(bl[0].rates[size_t(BaselineMetric::Bursts)].p50 - 100) < 1e-9, "bursts_per_second_from_captures");

        // Degraded captures and their frames are not learned from.
        CaptureRecord bad = cap(++seq, 1000);
        bad.overflows.push_back({10, std::nullopt});
        st.ingest(make_frame_event(bad, 5, 5, "DSSS", 1, false, V.at("deauth_broadcast_reason7"), true));
        bad.events_submitted = 1;
        st.ingest(bad);
        bl = st.snapshot().baselines;
        check(bl[0].captures_excluded == 1 && bl[0].current_analysed_s == 0, "degraded_capture_excluded");

        // Frozen: windows close but do not enter statistics.
        st.ingest(ControlCommand{"baseline_freeze", "", 0});
        for (int s = 0; s < 10; ++s) {
            CaptureRecord c = cap(++seq, 2000 + s * 1.2);
            for (int k = 0; k < 500; ++k)
                st.ingest(make_frame_event(c, size_t(k * 100), 50, "DSSS", 1, false, V.at("deauth_broadcast_reason7"), true));
            c.events_submitted = 500;
            st.ingest(c);
        }
        bl = st.snapshot().baselines;
        check(bl[0].frozen && bl[0].windows_excluded == 1 && bl[0].rates[size_t(BaselineMetric::Deauth)].max == 20,
              "frozen_window_excluded_from_stats", "max=" + std::to_string(bl[0].rates[size_t(BaselineMetric::Deauth)].max));
        st.ingest(ControlCommand{"baseline_unfreeze", "", 0});

        // Different receiver settings: a different baseline, never mixed.
        st.ingest(cap(++seq, 3000, 6, 31.5));
        check(st.snapshot().baselines.size() == 2, "gain_change_starts_new_baseline");

        // Persistence: closed windows survive, partial windows do not.
        CaptureRecord part = cap(++seq, 4000);
        st.ingest(part);
        nlohmann::json saved = st.export_persistent();
        SecurityState restored(lim);
        restored.import_persistent(saved);
        auto rb = restored.snapshot().baselines;
        const auto it = std::find_if(rb.begin(), rb.end(), [&](const auto& b) { return b.key == bl[0].key; });
        check(it != rb.end() && it->restored && it->windows_retained == 5 && it->current_analysed_s == 0 &&
                  it->rates[size_t(BaselineMetric::Deauth)].p50 == 18,
              "restore_keeps_closed_windows_only");
        check(restored.snapshot().baselines_restored == 2, "restore_count");

        // Reset: new version, history cleared.
        st.ingest(ControlCommand{"baseline_reset", bl[0].key, 0});
        auto after = st.snapshot().baselines;
        const auto a = std::find_if(after.begin(), after.end(), [&](const auto& b) { return b.key == bl[0].key; });
        check(a != after.end() && a->version == 2 && a->windows_retained == 0 && a->windows_included == 0,
              "reset_bumps_version_and_clears");
        check(st.snapshot().commands_applied == 3, "commands_counted");
        st.ingest(ControlCommand{"not_a_command", "", 0});
        check(st.snapshot().commands_applied == 3 && st.snapshot().input_lines_rejected == 1, "unknown_command_rejected");
    }

    // ---- 4. incidents ------------------------------------------------------------
    {
        StateLimits lim;
        lim.incidents.max_evidence = 3;
        lim.incidents.max_first_evidence = 1;  // first evidence kept once, then the newest 3
        lim.incidents.close_after_quiet_analysed_s = 5.0;
        lim.incidents.max_incidents = 2;
        SecurityState st(lim);
        IncidentObservation o;
        o.rule = "test_rule"; o.rule_version = "0"; o.config_version = "c0";
        o.band = "wifi_2g4"; o.channel = 6; o.claimed_source = "00:11:22:33:44:55"; o.target = "ff:ff:ff:ff:ff:ff";
        o.severity = IncidentSeverity::Low; o.numerator = 5; o.denominator_analysed_s = 10;
        o.benign_alternatives = {"AP reboot"};
        for (int i = 0; i < 5; ++i) {
            o.host_ns = 1000 + i;
            o.evidence = IncidentEvidence{{"run", uint64_t(i), size_t(i), "DSSS"}, o.host_ns, "frame"};
            if (i == 3) o.severity = IncidentSeverity::High;
            if (i == 4) o.severity = IncidentSeverity::Low;
            st.observe(o);
        }
        auto inc = st.snapshot().incidents;
        check(inc.size() == 1 && inc[0].observations == 5 && inc[0].open, "observations_coalesce");
        check(inc[0].first_evidence.size() == 1 && std::get<1>(inc[0].first_evidence[0].frame) == 0 &&
                  inc[0].evidence.size() == 3 && inc[0].evidence_dropped == 1 &&
                  std::get<1>(inc[0].evidence.back().frame) == 4,
              "evidence_bounded_first_kept_latest_rolling");
        check(inc[0].severity == IncidentSeverity::High, "severity_keeps_maximum");
        check(inc[0].first_host_ns == 1000 && inc[0].last_host_ns == 1004, "first_last_observation");

        // Analysed air on OTHER channels, or unprocessed captures, does not close it.
        CaptureRecord other = cap(1, 0, 11);
        for (int i = 0; i < 10; ++i) { other.capture_seq = 100 + uint64_t(i); st.ingest(other); }
        check(st.snapshot().incidents_open == 1, "quiet_on_other_channel_does_not_close");
        for (uint64_t i = 0; i < 4; ++i) st.ingest(cap(200 + i, double(i) * 1.1, 6));
        check(st.snapshot().incidents_open == 1, "short_quiet_keeps_open");
        st.ingest(cap(210, 10, 6));
        check(st.snapshot().incidents_open == 0 && !st.snapshot().incidents[0].open, "closes_after_analysed_quiet");

        o.claimed_source = "66:77:88:99:aa:bb"; st.observe(o);
        o.claimed_source = "da:01:02:03:04:05"; st.observe(o);
        check(st.snapshot().incidents_total == 2 && st.snapshot().incidents_evicted == 1, "closed_incident_evicted_first");

        SecurityState restored(lim);
        restored.import_persistent(st.export_persistent());
        auto ri = restored.snapshot().incidents;
        check(ri.size() == 2 && ri[0].restored && ri[0].open && restored.snapshot().incidents_restored == 2,
              "incidents_restored_and_flagged_interrupted");
        uint64_t id = restored.observe(o);
        check(id == ri[1].id && restored.snapshot().incidents.back().observations == 2, "restored_open_incident_coalesces");
    }

    // ---- 5. monitor: persistence, commands in recordings, replay equality ------
    const fs::path dir = fs::temp_directory_path() / ("wifi-security-b-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    {
        MonitorConfig cfg;
        cfg.run_id = "run-M";
        cfg.record_path = (dir / "rec.ndjson").string();
        cfg.state_path = (dir / "state" / "state.json").string();
        cfg.limits.baseline.window_analysed_s = 2.0;
        {
            WifiSecurityMonitor m(cfg);
            m.start();
            for (uint64_t s = 1; s <= 10; ++s) {
                CaptureRecord c = cap(s, double(s) * 1.1, 6, 20.0, "run-M");
                m.submit(make_frame_event(c, 10, 10, "DSSS", 1, false, V.at("probe_req_wildcard"), true));
                c.events_submitted = 1;
                m.submit(c);
                if (s == 5) m.submit(ControlCommand{"baseline_freeze", "", 1});
            }
            m.stop();
            auto s = m.snapshot();
            check(s->baselines.size() == 1 && s->baselines[0].windows_closed == 5 && s->baselines[0].windows_excluded == 3,
                  "live_windows_and_freeze", "closed=" + std::to_string(s->baselines.empty() ? 0 : s->baselines[0].windows_closed));
            check(fs::exists(cfg.state_path) && s->persist_error.empty(), "state_saved_on_stop");

            // Replaying the recording (commands included) reproduces the snapshot.
            SecurityState replay(cfg.limits);
            std::ifstream in(cfg.record_path);
            std::string line;
            while (std::getline(in, line)) {
                auto j = nlohmann::json::parse(line);
                const std::string k = j.value("kind", "");
                if (k == "frame") replay.ingest(frame_from_json(j));
                else if (k == "capture") replay.ingest(capture_from_json(j));
                else if (k == "command") replay.ingest(command_from_json(j));
            }
            auto lj = snapshot_json(*s), rj = snapshot_json(replay.snapshot());
            for (auto* x : {&lj, &rj}) for (const char* k : {"recorded_bytes", "storage_error", "persist_error"}) x->erase(k);
            check(lj == rj, "replay_with_commands_matches_live");
        }
        {
            WifiSecurityMonitor m(cfg);  // same state path: restores
            m.start();
            m.stop();
            auto s = m.snapshot();
            check(s->baselines_restored == 1 && s->baselines.size() == 1 && s->baselines[0].restored &&
                      s->baselines[0].windows_closed == 5 && s->baselines_frozen,
                  "restart_restores_baselines_and_freeze");
        }
        {
            std::ofstream(cfg.state_path) << "{not json";
            WifiSecurityMonitor m(cfg);
            m.start();
            CaptureRecord c = cap(1, 0, 6, 20.0, "run-N");
            bool ok = m.submit(c);
            m.stop();
            auto s = m.snapshot();
            check(ok && s->captures_ingested == 1 && s->persist_error.find("could not load") != std::string::npos,
                  "corrupt_state_reported_not_fatal", s->persist_error);
        }
    }
    fs::remove_all(dir);

    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
