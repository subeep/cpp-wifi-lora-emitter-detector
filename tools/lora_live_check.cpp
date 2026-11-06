// Receive-only live check of the LoRa tab's receiver on the real radio.
// Drives the production Scanner in BAND_SUB_GHZ with a LoRa channel plan and
// records everything the LoRa tab would show: every distinct packet row,
// LoRaWAN devices, RF clusters (with reading history), rule incidents,
// listening statistics and capture evidence. Persistent stores live in a
// temporary root (or --root DIR), never the project's data/ directory.
//
//   lora_live_check --seconds N --json PATH [--channels MHz,MHz,..] [--rotate] [--width MHz|auto]
//                   [--lock MHz] [--gain dB] [--capture-seconds S] [--root DIR]
//                   [--disallow-after S] [--device x310|b210]
// --rotate: one channel per cycle instead of wideband listening.
// --disallow-after S: after S seconds, mark every existing RF cluster
//   "disallowed" (tests the disallowed-emitter rule within one run).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "lora_export.hpp"
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
std::string row_key(const LoraPacketRow& r) {
    char b[160];
    std::snprintf(b, sizeof b, "|%.6f|%d|%.0f|%d|%s|%s", r.freq_mhz, r.sf, r.bandwidth_khz,
                  r.inverted_iq.value_or(false) ? 1 : 0, lora_crc_label(r.crc), r.status.c_str());
    return r.time + "|" + r.reception_id + b + "|" + r.payload_hex + "|" +
           std::to_string(r.decoder_sample_start.value_or(0));
}
}  // namespace

int main(int argc, char** argv) {
    double seconds = 60, gain = 5, capture_seconds = LORA_LISTEN_DURATION_S, disallow_after = -1;
    std::string json_path, root_arg;
    std::vector<double> channels = LORA_LISTEN_CHANNELS_HZ;
    bool wideband = true;
    std::optional<double> lock, width = 20e6;
    SdrDeviceType device = SdrDeviceType::X310;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        std::string err;
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--json" && i + 1 < argc) json_path = argv[++i];
        else if (a == "--channels" && i + 1 < argc) {
            channels = lora_plan::parse_channels_mhz(argv[++i], &err);
            if (channels.empty()) { std::fprintf(stderr, "--channels: %s\n", err.c_str()); return 2; }
        } else if (a == "--rotate") wideband = false;
        else if (a == "--width" && i + 1 < argc) {
            const std::string w = argv[++i];
            width = w == "auto" ? std::optional<double>() : std::optional<double>(std::atof(w.c_str()) * 1e6);
        }
        else if (a == "--lock" && i + 1 < argc) lock = std::atof(argv[++i]) * 1e6;
        else if (a == "--gain" && i + 1 < argc) gain = std::atof(argv[++i]);
        else if (a == "--capture-seconds" && i + 1 < argc) capture_seconds = std::atof(argv[++i]);
        else if (a == "--root" && i + 1 < argc) root_arg = argv[++i];
        else if (a == "--disallow-after" && i + 1 < argc) disallow_after = std::atof(argv[++i]);
        else if (a == "--device" && i + 1 < argc) device = std::string(argv[++i]) == "b210" ? SdrDeviceType::B210 : SdrDeviceType::X310;
        else {
            std::fprintf(stderr, "usage: %s --seconds N --json PATH [--channels MHz,..] [--rotate] [--lock MHz] [--width MHz|auto] [--gain dB] "
                                 "[--capture-seconds S] [--root DIR] [--disallow-after S] [--device x310|b210]\n", argv[0]);
            return 2;
        }
    }
    if (json_path.empty()) { std::fprintf(stderr, "--json PATH is required\n"); return 2; }
    json_path = std::filesystem::absolute(json_path).string();
    std::string root;
    if (root_arg.empty()) {
        char tmp[] = "/tmp/rfmon-lora-live-XXXXXX";
        if (!mkdtemp(tmp)) return 1;
        root = tmp;
    } else {
        root = std::filesystem::absolute(root_arg).string();
        std::filesystem::create_directories(root);
    }
    const auto cwd = std::filesystem::current_path();
    std::filesystem::current_path(root);  // the legacy fingerprint log is cwd-relative

    json out;
    out["config"] = {{"seconds", seconds}, {"gain_db", gain}, {"capture_seconds", capture_seconds},
                     {"channels_mhz", lora_plan::format_channels_mhz(channels)}, {"wideband", wideband},
                     {"lock_mhz", lock ? json(*lock / 1e6) : json(nullptr)}, {"root", root},
                     {"width_mhz", width ? json(*width / 1e6) : json("auto")},
                     {"device", device == SdrDeviceType::X310 ? "x310" : "b210"}};
    int code = 0;
    {
        Scanner scanner(root);
        scanner.set_device_type(device);
        scanner.set_gain(gain);
        scanner.set_active_band(BAND_SUB_GHZ);
        scanner.set_lora_capture_seconds(capture_seconds);
        scanner.set_lora_channels(channels);
        scanner.set_lora_wideband_listen(wideband);
        scanner.set_lora_wideband_rate(width);
        scanner.set_lora_lock_freq(lock);
        json plan = json::array();
        for (const auto& c : scanner.lora_listen_plan())
            plan.push_back({{"center_mhz", c.center_hz / 1e6}, {"rate_msps", c.rate_hz / 1e6}, {"wideband", c.wideband},
                            {"channels_mhz", lora_plan::format_channels_mhz(c.channels_hz)},
                            {"dc_clearance_khz", c.dc_clearance_hz / 1e3}});
        out["plan"] = plan;
        out["energy_scan_replaced"] = scanner.lora_capture_replaces_energy_scan();
        std::printf("LoRa live check: %.0f s, %s, plan %s\n", seconds, lock ? "locked" : wideband ? "wideband" : "rotate",
                    plan.dump().c_str());
        std::fflush(stdout);

        const auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        scanner.start();
        std::map<std::string, LoraPacketRow> seen;  // distinct rows over the whole run
        uint64_t overflow_polls = 0, polls = 0;
        bool disallowed = false;
        double next_print = 5;
        while (elapsed() < seconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            for (auto& r : scanner.lora_packets()) seen.emplace(row_key(r), r);
            const auto st = scanner.status();
            ++polls;
            overflow_polls += st.last_overflow;
            if (disallow_after >= 0 && !disallowed && elapsed() >= disallow_after) {
                for (const auto& e : scanner.lora_master_snapshot())
                    scanner.set_lora_enrolment(e.device_id, "disallowed", "", "lora_live_check --disallow-after");
                disallowed = true;
                std::printf("[%5.0f s] marked %zu cluster(s) disallowed\n", elapsed(), scanner.lora_master_snapshot().size());
            }
            if (elapsed() >= next_print) {
                size_t valid = 0;
                for (const auto& [k, r] : seen) valid += r.crc == LoraPacketRow::Crc::Valid;
                const auto ls = scanner.lora_listen_stats();
                std::printf("[%5.0f s] connected=%d captures=%llu crc_valid_rows=%zu incidents=%zu last=%.2fs IQ/%.2fs decode %s\n",
                            elapsed(), st.connected, (unsigned long long)ls.captures, valid,
                            scanner.lora_incidents().incidents.size(), ls.last_capture_s, ls.last_decode_s, st.error.c_str());
                std::fflush(stdout);
                next_print += 5;
            }
        }
        for (auto& r : scanner.lora_packets()) seen.emplace(row_key(r), r);
        const auto status = scanner.status();
        const double wall = elapsed();
        scanner.stop();

        std::vector<LoraPacketRow> valid_rows, failed_rows;
        std::map<std::string, uint64_t> other;
        for (const auto& [k, r] : seen) {
            if (r.crc == LoraPacketRow::Crc::Valid) valid_rows.push_back(r);
            else {
                ++other[r.status];
                if (r.header_valid) failed_rows.push_back(r);
            }
        }
        out["decoded_not_valid_rows"] = json::parse(lora_export::packets_json(failed_rows));
        out["wall_s"] = wall;
        out["connected"] = status.connected;
        out["error"] = status.error;
        out["overflow_polls"] = overflow_polls;
        out["polls"] = polls;
        out["vm_hwm_kb"] = vm_hwm_kb();
        out["crc_valid_rows"] = json::parse(lora_export::packets_json(valid_rows));
        out["other_rows_by_status"] = other;
        json devices = json::array();
        for (const auto& d : scanner.lora_devices())
            devices.push_back({{"kind", d.kind}, {"id", d.id}, {"frames", d.frames}, {"uplinks", d.uplinks},
                               {"downlinks", d.downlinks}, {"last_fcnt16", d.last_fcnt16 ? json(*d.last_fcnt16) : json(nullptr)},
                               {"rf_clusters", d.rf_clusters}, {"last_type", d.last_type},
                               {"last_freq_mhz", d.last_freq_hz / 1e6}, {"last_sf", d.last_sf}});
        out["lorawan_devices"] = devices;
        json emitters = json::array();
        for (const auto& e : scanner.lora_master_snapshot()) {
            json h = json::array();
            for (const auto& r : scanner.lora_emitter_history(e.device_id))
                h.push_back({{"ts", r.ts}, {"freq_mhz", r.freq_hz / 1e6}, {"sf", r.sf}, {"bw_khz", r.bw_hz / 1e3},
                             {"cfo_ppm", r.cfo_ppm}, {"irr_db", r.irr_db}, {"iq_eps", r.iq_eps}, {"iq_phi_deg", r.iq_phi_deg},
                             {"dc_dbc", r.dc_dbc}, {"snr_db", r.snr_db}, {"evm_pct", r.evm_pct}});
            emitters.push_back({{"id", e.device_id_str}, {"readings", e.reading_count}, {"policy", e.enrolment.policy},
                                {"last_freq_mhz", e.last_freq_hz / 1e6}, {"last_sf", e.last_sf}, {"history", h}});
        }
        out["rf_emitters"] = emitters;
        const auto inc = scanner.lora_incidents();
        json incidents = json::array();
        for (const auto& i : inc.incidents)
            incidents.push_back({{"id", i.id}, {"rule", i.rule}, {"severity", i.severity}, {"subject", i.subject},
                                 {"count", i.count}, {"first_ts", i.first_ts}, {"last_ts", i.last_ts},
                                 {"summary", i.summary}, {"evidence", i.evidence}});
        out["incidents"] = incidents;
        out["rule_stats"] = {{"frames", inc.stats.frames}, {"data_frames", inc.stats.data_frames},
                             {"join_requests", inc.stats.join_requests}, {"retransmissions", inc.stats.retransmissions},
                             {"ignored", inc.stats.ignored}};
        const auto ls = scanner.lora_listen_stats();
        json share = json::object();
        for (const auto& [ch, s] : ls.channel_listened_s) share[lora_plan::format_channels_mhz({ch})] = s;
        out["listen"] = {{"captures", ls.captures}, {"channel_iq_seconds", share}, {"last_capture_s", ls.last_capture_s},
                         {"last_decode_s", ls.last_decode_s}, {"last_rate_msps", ls.last_rate_hz / 1e6},
                         {"last_wideband", ls.last_wideband}};
        out["evidence"] = lora_security::snapshot_json(*scanner.lora_security_snapshot());
        std::vector<lora_plan::Observation> energy;
        for (const auto& r : scanner.snapshot(BAND_SUB_GHZ))
            energy.push_back({r.freq_mhz * 1e6, r.bandwidth_khz * 1e3, r.power_db, r.hit_count, r.protocol_guess.rfind("LoRa", 0) == 0});
        json suggestions = json::array();
        for (const auto& s : lora_plan::suggest(energy, channels))
            suggestions.push_back({{"freq_mhz", s.freq_hz / 1e6}, {"bw_khz", s.bandwidth_hz / 1e3}, {"hits", s.hits}, {"power_db", s.power_db}});
        out["suggestions"] = suggestions;
        code = status.connected || ls.captures ? 0 : 1;
        std::printf("Done: captures=%llu crc_valid_rows=%zu incidents=%zu devices=%zu clusters=%zu\n",
                    (unsigned long long)ls.captures, valid_rows.size(), incidents.size(), devices.size(), emitters.size());
    }
    std::filesystem::current_path(cwd);
    std::ofstream(json_path) << out.dump(1) << "\n";
    if (root_arg.empty()) std::filesystem::remove_all(root);
    return code;
}
