// Radio-free equivalence test for the Wi-Fi processing lane (security plan,
// package B: capture/processing overlap).
//
// The same hand-built Wi-Fi steps - real received X310 beacon crops
// (tests/fixtures/wifi_ofdm) embedded in seeded noise - are processed three
// ways by real Scanner instances with separate temporary data roots:
//   A  today's inline sequence: empty-capture records submitted "during
//      acquisition", then process_step_captures, status written at cycle end;
//   B  through a SerialLane running Scanner::run_wifi_lane_job, pushed exactly
//      as Scanner::run() pushes (steps max_outstanding 1, markers 2);
//   B' the same lane with random delays on producer and consumer.
// Everything observable must be identical: the security-monitor recording
// line by line (processing time and header run id masked), packet rows
// (wall-clock time masked), registry rows (ages masked), beacon-source counts,
// the Wi-Fi identity store, and cycle status. The real monitor's ordering
// invariants are checked too. No UsrpCapture is ever constructed.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "scanner.hpp"
#include "serial_lane.hpp"

using namespace rfmon;
namespace fs = std::filesystem;
using cf = std::complex<float>;

namespace rfmon {
struct ScannerTestAccess {
    using Job = Scanner::WifiStepJob;
    static void set_device_factory(Scanner& s, decltype(Scanner::device_factory_) f) { s.device_factory_ = std::move(f); }
    static void raise_stop_flag(Scanner& s) { s.stop_flag_ = true; }
    static bool stop_flag(Scanner& s) { return s.stop_flag_.load(); }
    static void start_monitor(Scanner& s) { s.security_->start(); }
    static void stop_monitor(Scanner& s) { s.security_->stop(); }
    static void lane_job(Scanner& s, Job& j) { s.run_wifi_lane_job(j); }
    // Today's inline path, as run() executes it with the lane disabled.
    static void inline_job(Scanner& s, Job& j) {
        if (j.cycle_end) {
            std::lock_guard<std::mutex> lock(s.status_mutex_);
            s.status_.cycle_count = j.cycle_count;
            s.status_.last_overflow = j.last_overflow;
            return;
        }
        for (auto& r : j.empty_records) s.security_->submit(std::move(r));  // submitted during acquisition
        if (j.captures.empty()) return;                                     // inline `continue`
        std::vector<Detection> detections;  // the cycle-wide vector is empty at every Wi-Fi step start
        s.process_step_captures(j.step, j.band, j.threshold, j.actual_rate, j.captures, j.capture_records,
                                detections);
    }
};
}  // namespace rfmon

namespace {

int failures = 0;
void check(bool cond, const std::string& name, const std::string& detail = "") {
    std::printf("%s [%s]%s%s\n", cond ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    if (!cond) ++failures;
}

std::vector<cf> load_crop(const std::string& stem) {
    const fs::path dir = fs::path(PROJECT_ROOT_DIR) / "tests/fixtures/wifi_ofdm";
    nlohmann::json j;
    std::ifstream(dir / (stem + ".json")) >> j;
    const size_t n = j.at("samples");
    std::vector<cf> iq(n);
    std::ifstream in(dir / j.at("iq_file").get<std::string>(), std::ios::binary);
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

// Frequency-shift a crop so a channel recorded at the capture center appears
// where the production scanner puts it (WIFI_CHANNEL_CAPTURE_OFFSET_HZ below).
std::vector<cf> shifted(const std::vector<cf>& x, double shift_hz, double rate) {
    std::vector<cf> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        const double ph = 2 * M_PI * shift_hz * double(i) / rate;
        y[i] = x[i] * cf(float(std::cos(ph)), float(std::sin(ph)));
    }
    return y;
}

double rms(const std::vector<cf>& x) {
    double s = 0;
    for (const auto& v : x) s += double(std::norm(v));
    return std::sqrt(s / double(x.size()));
}

std::vector<cf> make_capture(size_t n, const std::vector<cf>& crop, size_t copies, size_t spacing, unsigned seed) {
    std::mt19937 rng(seed);
    const float sigma = float(rms(crop) / 200.0);
    std::normal_distribution<float> g(0.0f, sigma);
    std::vector<cf> cap(n);
    for (auto& v : cap) v = {g(rng), g(rng)};
    for (size_t k = 0; k < copies; ++k) {
        const size_t at = 20000 + k * spacing;
        if (at + crop.size() > n) break;
        std::copy(crop.begin(), crop.end(), cap.begin() + long(at));
    }
    return cap;
}

struct Builder {
    uint64_t seq = 0;
    int64_t device_ns = 1'000'000'000;
    wifi_security::CaptureRecord record(const std::string& band, int channel, double channel_hz, size_t samples) {
        wifi_security::CaptureRecord r;
        r.run_id = "lane-test";
        r.capture_seq = ++seq;
        r.radio_session = 1;
        r.band = band;
        r.channel = channel;
        r.channel_hz = channel_hz;
        r.capture_center_hz = channel_hz + WIFI_CHANNEL_CAPTURE_OFFSET_HZ;
        r.requested_rate_hz = r.sample_rate_hz = 20e6;
        r.requested_duration_s = 1.0;
        r.gain_db = 20.0;
        r.antenna = "RX2";
        r.device = "test";
        r.samples_requested = 20000000;
        r.samples_received = samples;
        r.clock = wifi_security::ClockDomain::UsrpDevice;
        r.device_time_ns = device_ns;
        r.host_before_ns = 1'790'000'000'000'000'000 + device_ns;
        r.host_after_ns = r.host_before_ns + 120'000;
        r.burst_cap = WIFI_SECURITY_MAX_BURSTS_PER_CAPTURE;
        r.retuned = seq == 1;
        device_ns += int64_t(double(samples) / 20e6 * 1e9) + 44'000'000;  // 44 ms inter-capture gap
        return r;
    }
};

using Job = ScannerTestAccess::Job;

// Steps: 5 GHz ch36 with a failed middle capture; 2.4 GHz ch1; an all-failed
// 5 GHz step; a 5 GHz step with >400 bursts in one capture; cycle markers.
std::vector<Job> build_jobs() {
    const auto c5 = load_crop("avgarde-5g-offset");  // recorded at the production +1.5 MHz offset
    const auto c24 = shifted(load_crop("airtel-2g4"), -WIFI_CHANNEL_CAPTURE_OFFSET_HZ, 20e6);
    Builder b;
    std::vector<Job> jobs;
    auto step_for = [](const std::string& band, int ch, double hz) {
        return ScanStep{band, hz + WIFI_CHANNEL_CAPTURE_OFFSET_HZ, 56e6, "test ch " + std::to_string(ch)};
    };
    auto wifi_job = [&](const std::string& band, int ch, double hz) {
        Job j;
        j.step = step_for(band, ch, hz);
        j.band = band;
        j.threshold = DEFAULT_DETECTION_THRESHOLD_DB;
        j.actual_rate = 20e6;
        return j;
    };
    {   // Step 1: capture, failed capture, capture, capture.
        Job j = wifi_job(BAND_WIFI_5G, 36, 5180e6);
        unsigned seed = 1;
        for (int k = 0; k < 4; ++k) {
            if (k == 1) { j.empty_records.push_back(b.record(j.band, 36, 5180e6, 0)); continue; }
            auto cap = make_capture(2'000'000, c5, 8, 240'000, seed++);
            j.capture_records.push_back(b.record(j.band, 36, 5180e6, cap.size()));
            j.captures.push_back(std::move(cap));
        }
        jobs.push_back(std::move(j));
    }
    {   // Step 2: 2.4 GHz channel 1.
        Job j = wifi_job(BAND_WIFI_2G4, 1, 2412e6);
        for (int k = 0; k < 4; ++k) {
            auto cap = make_capture(2'000'000, c24, 6, 300'000, 100 + unsigned(k));
            j.capture_records.push_back(b.record(j.band, 1, 2412e6, cap.size()));
            j.captures.push_back(std::move(cap));
        }
        jobs.push_back(std::move(j));
    }
    {   Job m; m.cycle_end = true; m.cycle_count = 1; m.last_overflow = false; jobs.push_back(std::move(m)); }
    {   // Step 3: every capture failed.
        Job j = wifi_job(BAND_WIFI_5G, 36, 5180e6);
        for (int k = 0; k < 4; ++k) j.empty_records.push_back(b.record(j.band, 36, 5180e6, 0));
        jobs.push_back(std::move(j));
    }
    {   // Step 4: one capture with more than 400 bursts, then an ordinary one.
        Job j = wifi_job(BAND_WIFI_5G, 36, 5180e6);
        // ~50% occupancy, so the detector's 25th-percentile noise floor is
        // still noise; 420 beacons pushes the capture past the 400-burst limit.
        auto dense = make_capture(10'600'000, c5, 420, 25'000, 200);
        j.capture_records.push_back(b.record(j.band, 36, 5180e6, dense.size()));
        j.captures.push_back(std::move(dense));
        auto cap = make_capture(2'000'000, c5, 5, 350'000, 201);
        j.capture_records.push_back(b.record(j.band, 36, 5180e6, cap.size()));
        j.captures.push_back(std::move(cap));
        jobs.push_back(std::move(j));
    }
    {   Job m; m.cycle_end = true; m.cycle_count = 2; m.last_overflow = true; jobs.push_back(std::move(m)); }
    return jobs;
}

struct Outcome {
    std::vector<std::string> recording;  // masked NDJSON lines
    nlohmann::json snapshot;             // masked scanner-visible state
    nlohmann::json raw_monitor;
};

std::vector<std::string> read_recording(const fs::path& p) {
    std::vector<std::string> out;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        auto j = nlohmann::json::parse(line);
        if (j.value("kind", "") == "header") continue;  // random run id of the Scanner instance
        if (j.value("kind", "") == "capture") j["processing_s"] = 0;
        if (j.contains("run_id")) j["run_id"] = "masked";  // random per Scanner instance
        out.push_back(j.dump());
    }
    return out;
}

enum class Mode { Inline, Lane, SlowLane };

Outcome run_mode(Mode mode, const fs::path& root) {
    fs::create_directories(root);
    Outcome o;
    {
        Scanner s(root.string());
        s.set_wifi_security_recording((root / "rec.ndjson").string());
        ScannerTestAccess::start_monitor(s);
        std::vector<Job> jobs = build_jobs();
        if (mode == Mode::Inline) {
            for (auto& j : jobs) ScannerTestAccess::inline_job(s, j);
        } else {
            std::mt19937 rng(mode == Mode::SlowLane ? 5 : 0);
            std::mt19937 rng_c(9);
            SerialLane<Job> lane([&](Job& j) {
                if (mode == Mode::SlowLane) std::this_thread::sleep_for(std::chrono::milliseconds(rng_c() % 40));
                ScannerTestAccess::lane_job(s, j);
            });
            for (auto& j : jobs) {
                if (mode == Mode::SlowLane) std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 25));
                const bool marker = j.cycle_end;
                lane.push(std::move(j), marker ? 2 : 1);
            }
            lane.close();
        }
        ScannerTestAccess::stop_monitor(s);

        nlohmann::json snap;
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& p : s.wifi_packets())
            rows.push_back({p.freq_mhz, p.channel, p.modulation, p.power_db, p.bandwidth_khz, p.duration_us,
                            p.confidence, p.decode_status, p.fcs_valid, p.frame_type, p.security_decode_only,
                            p.master_key, p.ofdm_rate_mbps, p.psdu_length,
                            p.identity ? p.identity->bssid : std::string(), p.fp_gate_reason.value_or("")});
        snap["rows"] = rows;
        for (const char* band : {BAND_WIFI_2G4, BAND_WIFI_5G}) {
            nlohmann::json reg = nlohmann::json::array();
            for (const auto& d : s.snapshot(band))
                reg.push_back({d.band, d.freq_mhz, d.bandwidth_khz, d.protocol_guess, d.power_db, d.hit_count});
            snap[std::string("registry_") + band] = reg;
            nlohmann::json counts = nlohmann::json::object();
            for (const auto& [ch, n] : s.wifi_source_counts(band)) counts[std::to_string(ch)] = n;
            snap[std::string("sources_") + band] = counts;
        }
        nlohmann::json master = nlohmann::json::array();
        for (const auto& m : s.wifi_master_snapshot())
            master.push_back({m.device_key, m.key_is_mac, m.reading_count, m.identity_count, m.last_channel_hz,
                              m.last_phy, m.identity ? m.identity->ssid : std::string()});
        std::sort(master.begin(), master.end());
        snap["master"] = master;
        const auto st = s.status();
        snap["status"] = {st.cycle_count, st.last_overflow};
        o.snapshot = snap;
        o.raw_monitor = wifi_security::snapshot_json(*s.wifi_security_snapshot(), false);
    }
    o.recording = read_recording(root / "rec.ndjson");
    return o;
}

// ---- End to end through the real Scanner::run(), with a fake radio -------
//
// Deterministic script, keyed by capture index (the scan thread calls the
// fake synchronously): captures 1-8 are 5 GHz ch36 (fixed channel, two
// cycles; capture 3 fails); at capture 8 the fake switches the band to
// Sub-GHz, so the next cycle is a Sub-GHz step plus a LoRa listen
// (captures 9-13); at capture 13 it switches back to 5 GHz for two more
// cycles (captures 14-21; capture 16 fails); at capture 21 it raises the
// stop flag itself, so both runs process exactly the same captures. The test
// then calls stop() at once - with the lane still busy - which is what
// catches a wrong stop order.
struct FakeRadio : CaptureDevice {
    Scanner* scanner = nullptr;
    // Pre-generated capture templates: handing out a copy is ~ms, so
    // acquisition outruns processing and the lane's backpressure bound is
    // actually exercised (a slow fake would hide an unbounded queue).
    const std::vector<std::vector<cf>>* wifi_templates = nullptr;
    const std::vector<cf>* subghz_template = nullptr;
    int* count = nullptr;
    size_t* rows_at_first_subghz = nullptr;
    int64_t device_ns = 1'000'000'000;
    std::optional<double> tuned;
    CaptureResult capture_detailed(double center_hz, double sample_rate_hz, double duration_s, double,
                                   bool retune_always) override {
        const int idx = ++*count;
        CaptureResult r;
        r.sample_rate_hz = sample_rate_hz;
        CaptureTiming& t = r.timing;
        t.requested_samples = size_t(sample_rate_hz * duration_s);
        t.retuned = retune_always || !tuned || *tuned != center_hz;
        tuned = center_hz;
        t.actual_rf_hz = center_hz;
        t.gain_db = 20.0;
        const bool wifi = center_hz > 2e9;
        if (!wifi && rows_at_first_subghz && *rows_at_first_subghz == size_t(-1))
            *rows_at_first_subghz = scanner->wifi_packets().size();  // Wi-Fi work must be finished here
        const bool failed = idx == 3 || idx == 16;
        if (!failed) {
            r.samples = wifi ? (*wifi_templates)[size_t(idx) % wifi_templates->size()] : *subghz_template;
            t.device_time_valid = true;
            t.device_time_ns = device_ns;
            t.host_before_ns = 1'790'000'000'000'000'000 + device_ns;
            t.host_after_ns = t.host_before_ns + 100'000;
            device_ns += int64_t(double(r.samples.size()) / sample_rate_hz * 1e9) + 44'000'000;
        } else {
            t.host_before_ns = t.host_after_ns = 1'790'000'000'000'000'000 + device_ns;
        }
        if (idx == 8) scanner->set_active_band(BAND_SUB_GHZ);
        if (idx == 13) scanner->set_active_band(BAND_WIFI_5G);
        if (idx == 21) ScannerTestAccess::raise_stop_flag(*scanner);
        return r;
    }
    void set_gain(std::optional<double>) override {}
};

struct RunOutcome {
    std::vector<std::string> recording;
    nlohmann::json state;
    ScannerStatus status;
    size_t rows_at_first_subghz = size_t(-1);
    int captures = 0;
};

RunOutcome run_end_to_end(bool lane, const fs::path& root) {
    fs::create_directories(root);
    if (lane) unsetenv("RFMON_WIFI_LANE");
    else setenv("RFMON_WIFI_LANE", "0", 1);
    const auto crop5 = load_crop("avgarde-5g-offset");
    std::vector<std::vector<cf>> wifi_templates;
    for (unsigned k = 0; k < 4; ++k) wifi_templates.push_back(make_capture(1'500'000, crop5, 5, 280'000, 1000 + k));
    const auto subghz_template = make_capture(200'000, std::vector<cf>(64, cf(0.01f, 0.0f)), 0, 1, 999);
    RunOutcome o;
    {
        Scanner s(root.string());
        s.set_device_type(SdrDeviceType::X310);
        s.set_active_band(BAND_WIFI_5G);
        s.set_wifi_fixed_channel(36);
        s.set_wifi_security_recording((root / "rec.ndjson").string());
        ScannerTestAccess::set_device_factory(s, [&](const DeviceProfile&, std::optional<double>) {
            auto f = std::make_unique<FakeRadio>();
            f->scanner = &s;
            f->wifi_templates = &wifi_templates;
            f->subghz_template = &subghz_template;
            f->count = &o.captures;
            f->rows_at_first_subghz = &o.rows_at_first_subghz;
            return std::unique_ptr<CaptureDevice>(std::move(f));
        });
        s.start();
        while (!ScannerTestAccess::stop_flag(s)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        s.stop();  // immediately: the lane may still be draining
        o.status = s.status();
        nlohmann::json st;
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& p : s.wifi_packets())
            rows.push_back({p.freq_mhz, p.channel, p.modulation, p.power_db, p.decode_status, p.fcs_valid,
                            p.frame_type, p.master_key, p.identity ? p.identity->bssid : std::string()});
        st["rows"] = rows;
        for (const char* band : {BAND_WIFI_2G4, BAND_WIFI_5G, BAND_SUB_GHZ}) {
            nlohmann::json reg = nlohmann::json::array();
            for (const auto& d : s.snapshot(band))
                reg.push_back({d.band, d.freq_mhz, d.bandwidth_khz, d.protocol_guess, d.power_db, d.hit_count});
            st[std::string("registry_") + band] = reg;
        }
        nlohmann::json master = nlohmann::json::array();
        for (const auto& m : s.wifi_master_snapshot())
            master.push_back({m.device_key, m.reading_count, m.identity_count});
        std::sort(master.begin(), master.end());
        st["master"] = master;
        const auto mon = wifi_security::snapshot_json(*s.wifi_security_snapshot(), false);
        st["monitor"] = {mon.at("captures_ingested"), mon.at("frames_accepted"), mon.at("queue_events_dropped"),
                         mon.at("queue_captures_dropped"), mon.at("timeline").at("out_of_order")};
        o.state = st;
    }
    o.recording = read_recording(root / "rec.ndjson");
    unsetenv("RFMON_WIFI_LANE");
    return o;
}

void end_to_end_checks(const fs::path& base) {
    RunOutcome off = run_end_to_end(false, base / "e2e-off");
    RunOutcome on = run_end_to_end(true, base / "e2e-on");
    check(!off.status.wifi_lane_enabled && on.status.wifi_lane_enabled, "e2e_lane_gate_and_env_switch");
    check(off.captures == 21 && on.captures == 21, "e2e_same_captures",
          std::to_string(off.captures) + " / " + std::to_string(on.captures));
    check(!off.state["rows"].empty() && off.state["monitor"][0] == 16, "e2e_fixture_meaningful",
          std::to_string(off.state["rows"].size()) + " rows, " + off.state["monitor"][0].dump() + " Wi-Fi records");
    check(on.recording == off.recording, "e2e_recording_identical_lane_vs_inline",
          std::to_string(on.recording.size()) + " vs " + std::to_string(off.recording.size()) + " lines");
    for (const char* key : {"rows", "registry_wifi_2g4", "registry_wifi_5g", "registry_sub_ghz_ism", "master", "monitor"})
        check(on.state[key] == off.state[key], std::string("e2e_identical_") + key);
    check(on.status.cycle_count == off.status.cycle_count && on.status.cycle_count == 2 &&
              on.status.last_overflow == off.status.last_overflow,
          "e2e_cycle_status_identical", std::to_string(on.status.cycle_count) + " vs " + std::to_string(off.status.cycle_count));
    check(off.rows_at_first_subghz != size_t(-1) && on.rows_at_first_subghz == off.rows_at_first_subghz,
          "e2e_wifi_work_finished_before_subghz",
          std::to_string(on.rows_at_first_subghz) + " vs " + std::to_string(off.rows_at_first_subghz) + " rows");
    check(on.status.wifi_lane_high_water >= 1 && on.status.wifi_lane_high_water <= 2, "e2e_lane_bounded_to_step_plus_marker",
          "high water " + std::to_string(on.status.wifi_lane_high_water));
    check(on.state["monitor"][2] == 0 && on.state["monitor"][3] == 0 && on.state["monitor"][4] == 0,
          "e2e_no_drops_no_out_of_order");
}

}  // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / ("scanner-lane-test-" + std::to_string(::getpid()));
    Outcome a = run_mode(Mode::Inline, base / "inline");
    Outcome b = run_mode(Mode::Lane, base / "lane");
    Outcome c = run_mode(Mode::SlowLane, base / "slow");

    // The fixtures must actually exercise the paths under test.
    const auto& m = a.raw_monitor;
    const uint64_t frames = m.value("frames_accepted", uint64_t(0));
    check(frames > 400, "fixture_produces_frames", std::to_string(frames) + " accepted frames");
    uint64_t tail = 0;
    for (const auto& cv : m.at("coverage")) tail += cv.value("bursts_beyond_identity_limit", uint64_t(0));
    check(tail > 0, "fixture_exercises_security_only_tail", std::to_string(tail) + " tail bursts");
    check(!a.snapshot["rows"].empty() && !a.snapshot["master"].empty(), "fixture_produces_rows_and_identities",
          std::to_string(a.snapshot["rows"].size()) + " rows, " + std::to_string(a.snapshot["master"].size()) + " identities");
    check(a.recording.size() > frames, "recording_has_frames_and_records", std::to_string(a.recording.size()) + " lines");

    // Equivalence.
    check(a.recording == b.recording, "lane_recording_identical_to_inline");
    check(a.recording == c.recording, "slow_lane_recording_identical_to_inline");
    for (const char* key : {"rows", "registry_wifi_2g4", "registry_wifi_5g", "sources_wifi_2g4", "sources_wifi_5g", "master", "status"}) {
        check(a.snapshot[key] == b.snapshot[key] && a.snapshot[key] == c.snapshot[key],
              std::string("identical_") + key);
    }
    check(b.snapshot["status"] == nlohmann::json::array({2, true}), "cycle_status_published_by_marker",
          b.snapshot["status"].dump());

    // Real-monitor ordering invariants on the lane recording.
    {
        std::vector<nlohmann::json> lines;
        for (const auto& l : b.recording) lines.push_back(nlohmann::json::parse(l));
        bool frames_before_record = true, seq_increasing = true;
        std::set<uint64_t> recorded;
        uint64_t last_nonempty = 0;
        for (const auto& j : lines) {
            const std::string k = j.value("kind", "");
            const uint64_t seq = j.value("capture_seq", uint64_t(0));
            if (k == "frame" && recorded.count(seq)) frames_before_record = false;
            if (k == "capture") {
                recorded.insert(seq);
                if (j.value("samples_received", size_t(0)) > 0) {
                    if (seq <= last_nonempty) seq_increasing = false;
                    last_nonempty = seq;
                }
            }
        }
        check(frames_before_record, "frames_precede_their_capture_record");
        check(seq_increasing, "nonempty_records_in_capture_order");
        const auto& bm = b.raw_monitor;
        check(bm.at("timeline").value("out_of_order", 1) == 0, "timeline_no_out_of_order");
        check(bm.value("queue_events_dropped", 1) == 0 && bm.value("queue_captures_dropped", 1) == 0, "no_queue_drops");
        check(bm.value("baseline_pending_dropped", 1) == 0, "no_baseline_pending_drops");
        check(bm.value("captures_ingested", 0) == 14, "every_capture_record_ingested",
              std::to_string(bm.value("captures_ingested", 0)));
    }

    // Negative control: the documented stop order matters. Submitting after
    // the monitor stopped loses records - which is why run() closes the lane
    // before stop() stops the monitor.
    {
        fs::create_directories(base / "neg");
        Scanner s((base / "neg").string());
        ScannerTestAccess::start_monitor(s);
        ScannerTestAccess::stop_monitor(s);
        std::vector<Job> jobs = build_jobs();
        ScannerTestAccess::lane_job(s, jobs[2 + 1]);  // the all-failed step: record-only
        check(s.wifi_security_queue_stats().captures_dropped == 4, "submits_after_monitor_stop_are_lost",
              std::to_string(s.wifi_security_queue_stats().captures_dropped));
    }

    // Scanner::run() itself, end to end, lane on vs RFMON_WIFI_LANE=0. Runs
    // from the temporary directory so any cwd-relative log stays out of the tree.
    {
        const fs::path cwd = fs::current_path();
        fs::create_directories(base);
        fs::current_path(base);
        end_to_end_checks(base);
        fs::current_path(cwd);
    }

    fs::remove_all(base);
    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
