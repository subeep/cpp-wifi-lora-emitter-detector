// Tests for the security monitor's input side (package A step 4 of
// docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md): event/coverage contracts,
// receive-time derivation, ingestion (acceptance, idempotence, repeated
// transmissions), the bounded queue, the recorder, and record -> replay
// determinism. Frames come from the independently generated
// tests/fixtures/wifi_security/frames.hex vectors; no radio needed.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "security/wifi_frame_event.hpp"
#include "security/wifi_security_io.hpp"
#include "security/wifi_security_monitor.hpp"
#include "security/wifi_security_state.hpp"

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

CaptureRecord capture(uint64_t seq, int channel = 6, const std::string& run = "run-A") {
    CaptureRecord c;
    c.run_id = run;
    c.capture_seq = seq;
    c.radio_session = 1;
    c.band = "wifi_2g4";
    c.channel = channel;
    c.channel_hz = 2407e6 + 5e6 * channel;
    c.capture_center_hz = c.channel_hz + 1.5e6;
    c.sample_rate_hz = 20e6;
    c.requested_rate_hz = 20e6;
    c.samples_requested = c.samples_received = 20000000;
    c.clock = ClockDomain::UsrpDevice;
    c.device_time_ns = 5'000'000'000;
    c.host_before_ns = 1'790'000'000'000'000'000;
    c.host_after_ns = c.host_before_ns + 200'000;  // 200 us bracket
    c.processed = true;
    return c;
}

std::string mac(const std::optional<MacAddress>& a) { return a ? format_mac(*a) : "<unset>"; }

}  // namespace

int main() {
    auto V = load_vectors();
    check(V.size() == 40, "vectors_loaded", std::to_string(V.size()));

    // ---- 1. receive-time derivation -------------------------------------------
    {
        CaptureRecord c = capture(1);
        FrameEvent e = make_frame_event(c, 2000, 100, "DSSS", 1, false, V.at("deauth_broadcast_reason7"), true);
        check(e.device_time_ns == 5'000'000'000 + 100'000, "device_time_from_sample_offset",
              std::to_string(e.device_time_ns.value_or(-1)));
        check(e.host_time_ns == c.host_before_ns + 100'000 + 100'000 && e.host_uncertainty_ns == 100'000,
              "host_time_is_bracket_midpoint_plus_offset", std::to_string(e.host_uncertainty_ns));
        check(!e.after_overflow && e.clock == ClockDomain::UsrpDevice, "no_overflow_flag");

        c.overflows.push_back({5000, std::nullopt});  // gap of unknown length at sample 5000
        FrameEvent before = make_frame_event(c, 4000, 10, "DSSS", 1, false, V.at("ack"), true);
        FrameEvent after = make_frame_event(c, 6000, 10, "DSSS", 1, false, V.at("ack"), true);
        check(before.device_time_ns && !before.after_overflow, "time_valid_before_overflow");
        check(!after.device_time_ns && !after.host_time_ns && after.after_overflow,
              "no_time_after_unknown_gap", "after_overflow and no invented time");

        c.overflows.back().resume_device_ns = 9'000'000'000;
        FrameEvent resumed = make_frame_event(c, 6000, 10, "DSSS", 1, false, V.at("ack"), true);
        check(resumed.device_time_ns == 9'000'000'000 + 50'000 && resumed.after_overflow,
              "time_reanchored_after_reported_resume", std::to_string(resumed.device_time_ns.value_or(-1)));

        CaptureRecord u = capture(2);
        u.clock = ClockDomain::Unknown;
        u.host_before_ns = u.host_after_ns = 0;
        FrameEvent none = make_frame_event(u, 10, 10, "OFDM", 6, false, V.at("cts"), true);
        check(!none.device_time_ns && !none.host_time_ns && none.clock == ClockDomain::Unknown,
              "unknown_clock_is_explicit");

        std::vector<uint8_t> huge(kMaxStoredMpduBytes + 10, 0xAB);
        FrameEvent big = make_frame_event(c, 1, 1, "DSSS", 1, false, huge, true);
        check(big.mpdu.size() == kMaxStoredMpduBytes && big.mpdu_truncated, "oversized_mpdu_truncated_and_flagged");
    }

    // ---- 2. every FCS-valid management fixture -> one attributed event ---------
    {
        SecurityState st;
        CaptureRecord c = capture(10, 11);
        size_t sample = 1000, expected = 0;
        std::map<std::string, uint64_t> want_types;
        for (const auto& [name, bytes] : V) {
            MacFrame f = parse_mac_frame(bytes.data(), bytes.size());
            if (!f.fcs_valid() || f.fc.type != FrameType::Management || f.header_state != ParseState::Ok) continue;
            ++expected;
            ++want_types[std::string("Mgmt/") + subtype_name(FrameType::Management, f.fc.subtype)];
            FrameEvent e = make_frame_event(c, sample += 5000, 400, "DSSS", 1, false, bytes, true);
            bool ok = st.ingest(e);
            const auto& p = st.snapshot().recent.back();
            check(ok && p.event.channel == 11 && p.event.capture_seq == 10 && mac(p.frame.bssid) == mac(f.bssid) &&
                      mac(p.frame.transmitter) == mac(f.transmitter),
                  "mgmt_fixture_attributed_" + name, mac(p.frame.transmitter));
        }
        const auto& s = st.snapshot();
        check(s.frames_accepted == expected && expected >= 18, "all_mgmt_fixtures_accepted_once",
              std::to_string(s.frames_accepted) + "/" + std::to_string(expected));
        uint64_t mgmt_total = 0;
        for (const auto& [k, n] : s.frames_by_type) if (k.rfind("Mgmt/", 0) == 0) mgmt_total += n;
        check(mgmt_total == expected, "type_counts_match");
        bool counts_ok = true;
        for (const auto& [k, n] : want_types) counts_ok = counts_ok && s.frames_by_type.count(k) && s.frames_by_type.at(k) == n;
        check(counts_ok, "per_subtype_counts_match");
        check(s.coverage.at({"wifi_2g4", 11}).frames_accepted == expected, "coverage_counts_accepted_frames");
    }

    // ---- 3. invalid / truncated / lying producer never become valid events ----
    {
        SecurityState st;
        CaptureRecord c = capture(20);
        check(!st.ingest(make_frame_event(c, 1, 1, "DSSS", 1, false, V.at("deauth_bad_fcs"), true)), "bad_fcs_rejected");
        check(!st.ingest(make_frame_event(c, 2, 1, "DSSS", 1, false, V.at("too_short_3"), true)), "too_short_rejected");
        check(!st.ingest(make_frame_event(c, 3, 1, "DSSS", 1, false, V.at("deauth_broadcast_reason7"), false)),
              "producer_says_invalid_rejected");
        check(!st.ingest(make_frame_event(c, 4, 1, "DSSS", 1, false, V.at("header_truncated_addr2"), true)),
              "valid_fcs_truncated_header_rejected");
        std::vector<uint8_t> huge(kMaxStoredMpduBytes + 10, 0x00);
        check(!st.ingest(make_frame_event(c, 5, 1, "DSSS", 1, false, huge, true)), "truncated_storage_rejected");
        const auto& s = st.snapshot();
        check(s.frames_accepted == 0 && s.frames_rejected_fcs == 4 && s.frames_rejected_malformed == 1 &&
                  s.frames_truncated_storage == 1,
              "rejections_counted", "fcs=" + std::to_string(s.frames_rejected_fcs) +
                  " malformed=" + std::to_string(s.frames_rejected_malformed));
        // A malformed BODY with a valid FCS is a real, attributable transmission.
        check(st.ingest(make_frame_event(c, 6, 1, "DSSS", 1, false, V.at("beacon_elem_overrun"), true)),
              "malformed_body_with_valid_fcs_kept");
    }

    // ---- 4. retries and repeats stay distinct; duplicates are idempotent -------
    {
        SecurityState st;
        CaptureRecord c = capture(30);
        std::vector<uint8_t> retry = V.at("deauth_unicast_retry_reason3");
        // Same frame with Retry cleared and a recomputed FCS (independent of the parser).
        std::vector<uint8_t> first(retry.begin(), retry.end() - 4);
        first[1] &= uint8_t(~0x08);
        uint32_t crc = rfmon::wifi::fcs32(first.data(), first.size());
        for (int i = 0; i < 4; ++i) first.push_back(uint8_t(crc >> (8 * i)));

        FrameEvent e1 = make_frame_event(c, 100, 50, "DSSS", 1, false, first, true);
        FrameEvent e2 = make_frame_event(c, 9000, 50, "DSSS", 1, false, retry, true);
        check(st.ingest(e1) && st.ingest(e2), "original_and_retry_both_accepted");
        const auto& r = st.snapshot().recent.back();
        check(r.repeated_content && r.first_seen_as && std::get<2>(*r.first_seen_as) == 100,
              "retry_linked_to_original_not_merged");

        FrameEvent e3 = make_frame_event(c, 20000, 50, "DSSS", 1, false, first, true);  // exact same bytes later
        check(st.ingest(e3) && st.snapshot().frames_repeated_content == 2, "identical_bytes_elsewhere_kept_as_repeat");

        check(!st.ingest(e2) && !st.ingest(e2), "same_ingest_key_is_idempotent");
        FrameEvent other_phy = e2;
        other_phy.phy = "OFDM";
        check(st.ingest(other_phy), "same_sample_different_phy_is_distinct");
        const auto& s = st.snapshot();
        check(s.frames_accepted == 4 && s.frames_duplicate == 2 && s.frames_ingested == 6, "idempotence_counts",
              "accepted=" + std::to_string(s.frames_accepted) + " dup=" + std::to_string(s.frames_duplicate));

        st.ingest(c);
        st.ingest(c);
        check(st.snapshot().captures_ingested == 1 && st.snapshot().captures_duplicate == 1, "capture_record_idempotent");
    }

    // ---- 5. coverage accounting ------------------------------------------------
    {
        SecurityState st;
        CaptureRecord ok = capture(40, 1);
        ok.samples_received = 20000000;
        CaptureRecord ovf = capture(41, 1);
        ovf.overflows.push_back({1000, std::nullopt});
        CaptureRecord empty = capture(42, 1);
        empty.samples_received = 0;
        empty.processed = false;
        CaptureRecord capped = capture(43, 1);
        capped.burst_cap_reached = true;
        capped.analysed_samples = 9000000;  // cap hit 0.45 s in
        capped.clock = ClockDomain::HostOnly;
        ok.analysed_samples = ok.samples_received;
        ovf.analysed_samples = ovf.samples_received;
        for (auto* r : {&ok, &ovf, &empty, &capped}) st.ingest(*r);
        const ChannelCoverage& cv = st.snapshot().coverage.at({"wifi_2g4", 1});
        check(cv.captures == 4 && cv.failed_captures == 1 && cv.overflow_captures == 1 && cv.burst_cap_captures == 1,
              "coverage_counts_failures_overflow_saturation");
        check(std::abs(cv.sampled_s - 3.0) < 1e-9 && std::abs(cv.contiguous_s - 2.0) < 1e-9,
              "sampled_vs_contiguous_seconds", std::to_string(cv.sampled_s) + " / " + std::to_string(cv.contiguous_s));
        check(cv.device_time_captures == 3 && cv.host_only_captures == 1, "clock_domains_counted");
        check(std::abs(cv.analysed_s - 2.45) < 1e-9, "analysed_seconds_exclude_air_past_burst_cap",
              std::to_string(cv.analysed_s));
        CaptureRecord legacy = capture(44, 2);
        legacy.burst_cap_reached = true;  // no analysed span recorded
        check(legacy.analysed_seconds() == 0.0, "capped_capture_without_span_counts_as_unanalysed");
    }

    // ---- 6. JSON round trip ------------------------------------------------------
    {
        CaptureRecord c = capture(50);
        c.overflows.push_back({77, int64_t(123)});
        c.gain_db = 20.0;
        c.dsss_fcs_valid = 3;
        CaptureRecord c2 = capture_from_json(to_json(c));
        check(to_json(c2) == to_json(c), "capture_json_round_trip");
        FrameEvent e = make_frame_event(c, 55, 66, "OFDM", 6, true, V.at("beacon_csa_quiet_load_mme"), true);
        e.fp_cfo_ppm = 1.25;
        e.fp_gate_reason = "below SNR floor";
        FrameEvent e2 = frame_from_json(to_json(e));
        check(to_json(e2) == to_json(e) && e2.mpdu == e.mpdu, "frame_json_round_trip");
        bool threw = false;
        try { frame_from_json(nlohmann::json{{"kind", "frame"}, {"run_id", "x"}}); } catch (...) { threw = true; }
        check(threw, "frame_missing_fields_rejected");
    }

    // ---- 7. bounded queue --------------------------------------------------------
    {
        MonitorConfig cfg;
        cfg.queue_max_items = 3;
        cfg.queue_max_bytes = 10000;
        cfg.capture_reserve = 2;
        check(queue_admits(cfg, 2, 0, 600, false) && !queue_admits(cfg, 3, 0, 600, false), "item_limit_for_frames");
        check(queue_admits(cfg, 4, 0, 600, true) && !queue_admits(cfg, 5, 0, 600, true), "captures_get_reserved_headroom");
        check(!queue_admits(cfg, 0, 9800, 600, false), "byte_limit_for_frames");

        // Submissions the monitor cannot take are counted and resurface as a
        // LossNotice once the consumer runs - never silently thinner data.
        WifiSecurityMonitor m(cfg);
        CaptureRecord c = capture(60);
        for (int i = 0; i < 5; ++i) m.submit(make_frame_event(c, size_t(i), 1, "DSSS", 1, false, V.at("ack"), true));
        m.submit(c);
        check(m.queue_stats().events_dropped == 5 && m.queue_stats().captures_dropped == 1, "drops_before_start_counted");
        m.start();
        m.stop();
        auto s = m.snapshot();
        check(s->queue_events_dropped == 5 && s->queue_captures_dropped == 1 && s->losses.size() == 1,
              "loss_visible_downstream");
    }

    // ---- 8. recorder: round trip, determinism, idempotent replay ---------------
    const fs::path dir = fs::temp_directory_path() / ("wifi-security-test-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    {
        MonitorConfig cfg;
        cfg.record_path = (dir / "rec.ndjson").string();
        cfg.run_id = "run-R";
        WifiSecurityMonitor m(cfg);
        m.start();
        size_t submitted = 0;
        for (uint64_t seq = 1; seq <= 30; ++seq) {
            CaptureRecord c = capture(seq, int(1 + seq % 13), "run-R");
            size_t sample = 0;
            for (const auto& [name, bytes] : V) {
                sample += 3000;
                if (m.submit(make_frame_event(c, sample, 400, "DSSS", 1, false, bytes, true))) ++submitted;
            }
            m.submit(c);
        }
        m.stop();
        auto live = m.snapshot();
        check(live->storage_error.empty() && live->recorded_bytes > 0, "recording_written",
              std::to_string(live->recorded_bytes) + " bytes");
        check(live->frames_ingested == submitted, "all_submissions_reached_state");

        auto replay = [&](const std::vector<fs::path>& files) {
            SecurityState st;
            for (const auto& f : files) {
                std::ifstream in(f);
                std::string line;
                while (std::getline(in, line)) {
                    auto j = nlohmann::json::parse(line);
                    std::string k = j.value("kind", "");
                    if (k == "frame") st.ingest(frame_from_json(j));
                    else if (k == "capture") st.ingest(capture_from_json(j));
                    else if (k == "loss") st.ingest(loss_from_json(j));
                }
            }
            return st;
        };
        SecurityState a = replay({dir / "rec.ndjson"});
        nlohmann::json lj = snapshot_json(*live), aj = snapshot_json(a.snapshot());
        for (auto* j : {&lj, &aj}) { j->erase("recorded_bytes"); j->erase("storage_error"); }
        check(lj == aj, "replay_reproduces_live_snapshot");
        SecurityState b = replay({dir / "rec.ndjson"});
        check(snapshot_json(a.snapshot(), true) == snapshot_json(b.snapshot(), true), "replay_is_deterministic");
        SecurityState twice = replay({dir / "rec.ndjson", dir / "rec.ndjson"});
        check(twice.snapshot().frames_accepted == a.snapshot().frames_accepted &&
                  twice.snapshot().frames_duplicate == a.snapshot().frames_ingested &&
                  twice.snapshot().captures_duplicate == a.snapshot().captures_ingested,
              "replaying_twice_is_idempotent");
    }
    {
        // Rotation keeps disk use bounded to about record_max_bytes.
        MonitorConfig cfg;
        cfg.record_path = (dir / "small.ndjson").string();
        cfg.record_max_bytes = 64 * 1024;
        WifiSecurityMonitor m(cfg);
        m.start();
        for (uint64_t seq = 1; seq <= 200; ++seq) {
            CaptureRecord c = capture(seq);
            m.submit(make_frame_event(c, 1, 1, "DSSS", 1, false, V.at("beacon_csa_quiet_load_mme"), true));
            m.submit(c);
            if (seq % 50 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        m.stop();
        const auto cur = fs::file_size(cfg.record_path), old = fs::exists(cfg.record_path + ".1") ? fs::file_size(cfg.record_path + ".1") : 0;
        check(old > 0 && cur + old <= cfg.record_max_bytes + 8192, "rotation_bounds_disk_use",
              std::to_string(cur) + " + " + std::to_string(old) + " bytes");
        std::ifstream first(cfg.record_path);
        std::string line;
        std::getline(first, line);
        check(nlohmann::json::parse(line).value("kind", "") == "header", "rotated_file_starts_with_header");
    }
    {
        // An unwritable recording path is reported and never blocks intake.
        MonitorConfig cfg;
        cfg.record_path = "/nonexistent-dir-for-test/rec.ndjson";
        WifiSecurityMonitor m(cfg);
        m.start();
        CaptureRecord c = capture(1);
        bool accepted = m.submit(make_frame_event(c, 1, 1, "DSSS", 1, false, V.at("deauth_broadcast_reason7"), true));
        m.stop();
        auto s = m.snapshot();
        check(accepted && s->frames_accepted == 1 && !s->storage_error.empty(), "storage_failure_visible_not_blocking",
              s->storage_error);
    }
    fs::remove_all(dir);

    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
