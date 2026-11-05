// Receive-only live check of coordinated mode on the real receiver
// (upgradedplan.md P5 exit evidence). Drives the production Scanner in
// BAND_COORDINATED with the production drone add-on (native DJI/O4 backend,
// Wi-Fi Remote ID) and, optionally, the production ML worker attached
// exactly as the GUI attaches them. Every persistent store is rooted in a
// temporary directory, so the project's Wi-Fi/LoRa/security data is never
// modified. Prints progress every 5 s and a JSON summary at the end.
//
//   coordinated_live_check [--seconds N] [--device x310|b210] [--ml] [--json PATH] [--mode BAND]
//                          [--channel N] [--security-record PATH]
// --mode runs a band mode instead (e.g. wifi_both) for an A/B comparison.
// --channel N holds a Wi-Fi band mode on one channel. --security-record
// writes the Wi-Fi security monitor's NDJSON (frames with RF features) to
// PATH, outside the temporary root, for wifi_security_replay.
// --retain N keeps the last N Wi-Fi/drone captures' IQ; with --ml and
// --export-dir, the latest model result's capture is exported as evidence.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "drone_evidence.hpp"
#include "drones/addon.hpp"
#include "ml_inference.hpp"
#include "scanner.hpp"

using namespace rfmon;
using json = nlohmann::json;

namespace {
long vm_hwm_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmHWM:", 0) == 0) return std::atol(line.c_str() + 6);
    return -1;
}
const char* slot_name(const std::string& b) {
    return b == BAND_SUB_GHZ ? "sub-GHz" : b == BAND_WIFI_2G4 ? "2.4 GHz" : b == BAND_WIFI_5G ? "5 GHz" : b.c_str();
}
}  // namespace

int main(int argc, char** argv) {
    double seconds = 120;
    bool with_ml = false;
    std::string json_path;
    std::string mode = BAND_COORDINATED;  // or a band mode, for A/B comparison
    size_t retain = 0;                    // captures of IQ kept for evidence export
    std::string export_dir;               // with --ml: export the latest model result's evidence here
    SdrDeviceType device = SdrDeviceType::X310;
    std::optional<int> channel;
    std::string security_record;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--ml") with_ml = true;
        else if (a == "--json" && i + 1 < argc) json_path = argv[++i];
        else if (a == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (a == "--retain" && i + 1 < argc) retain = size_t(std::atoi(argv[++i]));
        else if (a == "--export-dir" && i + 1 < argc) export_dir = argv[++i];
        else if (a == "--channel" && i + 1 < argc) channel = std::atoi(argv[++i]);
        else if (a == "--security-record" && i + 1 < argc) security_record = std::filesystem::absolute(argv[++i]).string();
        else if (a == "--device" && i + 1 < argc) device = std::string(argv[++i]) == "b210" ? SdrDeviceType::B210 : SdrDeviceType::X310;
        else { std::fprintf(stderr, "usage: %s [--seconds N] [--device x310|b210] [--ml] [--json PATH] [--mode BAND] [--channel N] [--security-record PATH] [--retain N] [--export-dir PATH]\n", argv[0]); return 2; }
    }
    char tmp[] = "/tmp/rfmon-coordinated-live-XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    const auto cwd = std::filesystem::current_path();
    std::filesystem::current_path(tmp);  // relative logs (LoRa fingerprints) stay in the temporary root

    ml::Inference model;
    drone::Addon drones;
    std::mutex mu;
    std::map<std::string, uint64_t> observed_by_band, frames_by_band, fcs_by_band;
    uint64_t observer_calls = 0, observer_mismatch = 0;
    double last_center = 0;
    std::string last_band;
    {
        Scanner scanner(
            tmp,
            [&](const std::vector<std::complex<float>>& iq, const wifi_security::CaptureRecord& r) {
                {
                    std::lock_guard<std::mutex> l(mu);
                    ++observer_calls;
                    ++observed_by_band[r.band];
                    // A record must describe its own samples: size and a centre in its band.
                    const bool band_ok = (r.band == BAND_WIFI_2G4 && r.capture_center_hz > 2.3e9 && r.capture_center_hz < 2.6e9) ||
                                         (r.band == BAND_WIFI_5G && r.capture_center_hz > 5e9 && r.capture_center_hz < 6e9);
                    if (!band_ok || r.samples_received != iq.size()) ++observer_mismatch;
                    last_center = r.capture_center_hz;
                    last_band = r.band;
                }
                drones.observe(iq, r);
                model.observe(iq, r);
            },
            [&](const wifi_security::FrameEvent& f) {
                {
                    std::lock_guard<std::mutex> l(mu);
                    ++frames_by_band[f.band];
                    if (f.fcs_valid) ++fcs_by_band[f.band];
                }
                drones.remote_id.submit(f);
            });
        scanner.set_device_type(device);
        scanner.set_iq_retention(retain);
        drones.native_max_capture_s = mode == BAND_COORDINATED ? 3600 : 0.160;
        drones.native.start(true);
        drones.remote_id.enable(true);
        if (with_ml) {
            ml::Options o;
            o.model = std::string(PROJECT_ROOT_DIR) + "/data/drone_ml/2026-10-14/expanded_training_01/residual_model/model.pt";
            o.python = std::string(PROJECT_ROOT_DIR) + "/data/drone_ml/runtime-2026-10-14/bin/python";
            o.script = std::string(PROJECT_ROOT_DIR) + "/tools/drone_ml/live_worker.py";
            model.start(o);
        }
        scanner.set_active_band(mode);
        if (channel) scanner.set_wifi_fixed_channel(channel);
        if (!security_record.empty() && !scanner.set_wifi_security_recording(security_record, uint64_t(4) << 30))
            std::fprintf(stderr, "security recording could not start: %s\n", security_record.c_str());
        const auto t0 = std::chrono::steady_clock::now();
        scanner.start();
        std::vector<double> cycle_times;
        uint64_t last_cycle = 0;
        double next_print = 5;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const auto st = scanner.coordinated_status();
            if (st.cycle != last_cycle) { last_cycle = st.cycle; cycle_times.push_back(st.last_cycle_s); }
            if (el >= next_print || el >= seconds) {
                next_print += 5;
                const auto s = scanner.status();
                const auto n = drones.native.snapshot();
                const auto m = model.snapshot();
                std::printf("[%6.1fs] connected=%d cycle=%llu last=%.2fs cfg=%.2fs %s | ", el, int(s.connected),
                            static_cast<unsigned long long>(st.cycle), st.last_cycle_s, st.estimate.cycle_s,
                            st.current ? st.current->label.c_str() : "-");
                for (const auto& sl : st.slots)
                    std::printf("%s v=%llu ok=%llu an=%llu drop=%llu | ", slot_name(sl.band), static_cast<unsigned long long>(sl.visits),
                                static_cast<unsigned long long>(sl.valid_visits), static_cast<unsigned long long>(sl.analysed_visits),
                                static_cast<unsigned long long>(sl.lane_drops));
                std::printf("ovf=%llu empty=%llu | native samples=%llu drop=%llu disc=%llu pkts=%llu o4win=%llu | ml proc=%llu drop=%llu stale=%llu%s%s\n",
                            static_cast<unsigned long long>(st.overflows), static_cast<unsigned long long>(st.empty_captures),
                            static_cast<unsigned long long>(n.samples), static_cast<unsigned long long>(n.dropped),
                            static_cast<unsigned long long>(n.discontinuities), static_cast<unsigned long long>(n.native_packet_count),
                            static_cast<unsigned long long>(n.o4_windows), static_cast<unsigned long long>(m.processed),
                            static_cast<unsigned long long>(m.dropped), static_cast<unsigned long long>(m.stale),
                            s.error.empty() ? "" : " ERROR: ", s.error.c_str());
                std::fflush(stdout);
            }
            if (el >= seconds) break;
        }
        const auto st = scanner.coordinated_status();
        const auto visits = scanner.coordinated_visits();
        const auto status = scanner.status();
        const auto security = scanner.wifi_security_snapshot();
        const auto lora = scanner.lora_security_snapshot();
        const auto lora_rows = scanner.lora_packets();
        const auto wifi_rows = scanner.wifi_packets();
        const auto native = drones.native.snapshot();
        const auto ml_state = model.snapshot();
        const auto rid = drones.remote_id.snapshot();
        // Live evidence export: the latest model result references the exact
        // capture it analysed; export +/-50 ms around its interval.
        json export_info = nullptr;
        if (!export_dir.empty() && !ml_state.history.empty()) {
            const auto& r = ml_state.history.front();
            const auto capture = scanner.retained_capture(r.capture);
            evidence::Entry e;
            e.source = "Model (diagnostic)";
            e.key = "model/latest";
            e.evidence = ml::observation_decision(r);
            e.identity = r.candidate.empty() ? "no candidate" : "candidate " + r.candidate + " (not accepted)";
            e.frequency_hz = r.center;
            e.capture_seq = r.capture;
            e.raw_json = r.raw;
            if (capture && capture->record.sample_rate_hz > 0)
                e.event_device_time_s = capture->record.device_time_ns * 1e-9 + double(r.offset_samples) / capture->record.sample_rate_hz;
            try {
                const auto dir = std::filesystem::absolute(std::filesystem::path(cwd) / export_dir).string();
                const auto path = evidence::export_entry(dir, e, visits, st.config_hash, capture, Scanner::now_steady_s());
                export_info = {{"path", path}, {"capture_seq", r.capture}, {"capture_retained", capture.has_value()},
                               {"iq_bytes", std::filesystem::exists(path + "/iq.cf32_le") ? std::filesystem::file_size(path + "/iq.cf32_le") : 0}};
            } catch (const std::exception& ex) {
                export_info = {{"error", ex.what()}};
            }
        }
        const auto retained_seqs = scanner.retained_capture_seqs();
        scanner.stop();
        drones.native.stop();
        drones.remote_id.enable(false);
        model.stop();

        json out;
        out["device"] = device == SdrDeviceType::X310 ? "X310" : "B210";
        out["mode"] = mode;
        out["seconds"] = seconds;
        out["connected"] = status.connected;
        out["error"] = status.error;
        out["config_hash"] = st.config_hash;
        out["configured_cycle_s"] = st.estimate.cycle_s;
        out["configured_valid_s"] = st.estimate.valid_s;
        out["cycles"] = st.cycle;
        out["measured_cycle_s"] = cycle_times;
        out["overflows"] = st.overflows;
        out["empty_captures"] = st.empty_captures;
        for (size_t i = 0; i < st.slots.size(); ++i) {
            const auto& sl = st.slots[i];
            out["slots"].push_back({{"band", sl.band}, {"visits", sl.visits}, {"valid", sl.valid_visits},
                                    {"analysed", sl.analysed_visits}, {"lane_drops", sl.lane_drops},
                                    {"acquired_s", sl.acquired_s}, {"analysed_s", sl.analysed_s},
                                    {"configured_sweep_s", i < st.estimate.slots.size() ? st.estimate.slots[i].sweep_s : 0},
                                    {"measured_sweep_s", sl.last_sweep_s}});
        }
        // Order check over retained visits: within a cycle, slots in order.
        uint64_t order_violations = 0;
        auto rank = [](const std::string& b) { return b == BAND_SUB_GHZ ? 0 : b == BAND_WIFI_2G4 ? 1 : 2; };
        for (size_t i = 1; i < visits.size(); ++i)
            if (visits[i].cycle == visits[i - 1].cycle && rank(visits[i].band) < rank(visits[i - 1].band)) ++order_violations;
        out["visit_order_violations"] = order_violations;
        double max_overrun = 0;
        for (const auto& v : visits)
            if (!v.in_progress) max_overrun = std::max(max_overrun, (v.end_s - v.start_s) - (v.acquired_s));
        out["max_visit_overhead_s"] = max_overrun;  // settle + transport/host overhead per visit
        {
            std::lock_guard<std::mutex> l(mu);
            out["observer_calls"] = observer_calls;
            out["observer_record_mismatch"] = observer_mismatch;
            out["observer_by_band"] = observed_by_band;
            out["frames_by_band"] = frames_by_band;
            out["fcs_valid_frames_by_band"] = fcs_by_band;
        }
        out["wifi_security_captures"] = security->captures_ingested;
        out["rf_identity"] = {{"frames", security->rf_identity_frames}, {"judged", security->rf_identity_judged},
                              {"unverifiable", security->rf_identity_unverifiable}, {"inconsistent", security->rf_identity_inconsistent},
                              {"excluded_frames", security->rf_identity_excluded}, {"tracked", security->rf_identity_tracked},
                              {"accepted_frames", security->frames_accepted}};
        {
            nlohmann::json incidents = nlohmann::json::array();
            for (const auto& i : security->incidents)
                incidents.push_back({{"rule", i.rule}, {"band", i.band}, {"channel", i.channel}, {"source", i.claimed_source},
                                     {"target", i.target}, {"observations", i.observations}, {"measures", i.measures},
                                     {"confidence", int(i.confidence)}, {"open", i.open}});
            out["wifi_security_incidents"] = incidents;
        }
        out["wifi_packet_rows"] = wifi_rows.size();
        out["lora_capture_records"] = lora->captures;
        size_t lora_decoded = 0;
        for (const auto& r : lora_rows) lora_decoded += r.header_valid ? 1 : 0;
        out["lora_rows"] = lora_rows.size();
        out["lora_header_valid_rows"] = lora_decoded;
        out["subghz_energy_groups"] = scanner.snapshot(BAND_SUB_GHZ).size();
        out["wifi24_groups"] = scanner.snapshot(BAND_WIFI_2G4).size();
        out["wifi5_groups"] = scanner.snapshot(BAND_WIFI_5G).size();
        out["native"] = {{"messages", drones.native_chunks.load()}, {"offered", drones.native_offered.load()},
                         {"omitted", drones.native_omitted.load()}, {"unhealthy", drones.native_unhealthy.load()},
                         {"samples", native.samples}, {"dropped", native.dropped}, {"discontinuities", native.discontinuities},
                         {"packets", native.native_packet_count}, {"o4_windows", native.o4_windows},
                         {"o4_candidates", native.o4_candidates}, {"errors", native.analysis_errors}, {"status", native.status},
                         {"max_observer_ms", drones.native_max_observer_ns.load() * 1e-6}};
        out["remote_id_tracks"] = rid.contains("tracks") ? rid["tracks"].size() : 0;
        out["ml"] = {{"enabled", with_ml}, {"state", ml_state.state}, {"processed", ml_state.processed},
                     {"dropped", ml_state.dropped}, {"stale", ml_state.stale}, {"invalid", ml_state.invalid},
                     {"error", ml_state.error}};
        out["vm_hwm_mb"] = vm_hwm_kb() / 1024.0;
        out["retained_captures"] = retained_seqs.size();
        out["evidence_export"] = export_info;
        const std::string text = out.dump(2);
        std::printf("%s\n", text.c_str());
        if (!json_path.empty()) {
            std::filesystem::current_path(cwd);
            std::ofstream(json_path) << text << "\n";
        }
    }
    std::filesystem::current_path(cwd);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    return 0;
}
