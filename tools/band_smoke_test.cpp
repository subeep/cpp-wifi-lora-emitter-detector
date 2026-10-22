// Manual hardware validation: drives Scanner through all three bands in
// turn (the same set_active_band()/status()/snapshot() API the GUI
// uses) and reports what each one actually saw. Exercises the brand-new
// 5GHz scan plan against real hardware for the first time - unlike
// sub-GHz/2.4GHz, those parameters were never validated on this B210
// before this port.
//
// Not part of the GUI or the automated DSP test - this is an
// interactive/manual tool since it needs real hardware and takes
// roughly a minute per band. Run it directly:
//   ./build/band_smoke_test

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <map>
#include <vector>
#include <string>
#include <thread>
#include <filesystem>
#include <fstream>

#include "../src/config.hpp"
#include "../src/scanner.hpp"

using namespace rfmon;

namespace {

// `seconds` must comfortably exceed one FULL sweep of the band, because
// the registry only commits at a cycle boundary (see
// DeviceRegistry::update_cycle - hit_count means "seen in N distinct
// cycles"). A 2.4GHz sweep is 13 channels x SUB_CAPTURES_PER_STEP x
// SUB_CAPTURE_DURATION_S = 52s+ before any processing. Dwelling less
// than that snapshots mid-cycle and reports almost every channel as
// empty while the scanner is in fact detecting on all of them.
//
// Waiting on status().cycle_count instead does NOT work: it resets to 0
// only inside the scan thread on the next loop iteration, so a poller
// still sees the previous band's count and exits immediately.
void run_band(Scanner& scanner, const std::string& band, const char* label, int seconds) {
    std::printf("\n=== %s ===\n", label);
    // The Wi-Fi packet log is global, not per-band, so remember where
    // this band's rows start or the previous band's leak into it.
    size_t packets_before = scanner.wifi_packets().size();
    scanner.set_active_band(band);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    ScannerStatus last_status;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        last_status = scanner.status();
    }
    std::printf("cycles completed: %d | last step: %s | overflow seen: %s\n",
                last_status.cycle_count, last_status.active_step_label.c_str(),
                last_status.last_overflow ? "yes" : "no");

    auto all_packets = scanner.wifi_packets();
    // The log is FIFO-capped, so older rows may have been evicted -
    // clamp rather than assuming packets_before is still a valid index.
    size_t start = std::min(packets_before, all_packets.size());
    std::vector<WifiPacketRow> packets(all_packets.begin() + long(start), all_packets.end());
    if (!packets.empty()) {
        std::printf("wifi packets detected: %zu\n", packets.size());
        std::map<int, int> per_channel;
        for (const auto& p : packets) per_channel[p.channel]++;
        std::printf("  per channel:");
        for (const auto& [ch, count] : per_channel) std::printf(" ch%d=%d", ch, count);
        std::printf("\n");
        size_t show = std::min<size_t>(6, packets.size());
        for (size_t i = packets.size() - show; i < packets.size(); ++i) {
            const auto& p = packets[i];
            std::printf("  %s ch%-3d %.4fMHz %-5s pwr=%.1fdB bw=%.2fMHz dur=%.0fus conf=%.3f\n",
                        p.time.c_str(), p.channel, p.freq_mhz, p.modulation.c_str(), p.power_db,
                        p.bandwidth_khz / 1e3, p.duration_us, p.confidence);
        }
    }

    auto sources = scanner.wifi_source_counts(band);
    if (!sources.empty()) {
        std::printf("  beacon-inferred sources:");
        for (const auto& [ch, count] : sources) {
            if (count > 0) std::printf(" ch%d>=%d", ch, count);
        }
        std::printf("\n");
    }

    auto rows = scanner.snapshot(band);
    std::printf("active emitters: %zu\n", rows.size());
    for (const auto& d : rows) {
        std::printf("  %.4f MHz  bw=%.1fkHz  power=%.1fdB  hits=%d  %s\n", d.freq_mhz,
                    d.bandwidth_khz, d.power_db, d.hit_count, d.protocol_guess.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if(argc==3 && std::string(argv[1])=="--isolated") {
        const auto root=std::filesystem::absolute(argv[2]);
        if(!std::filesystem::create_directories(root)){std::fprintf(stderr,"Output directory must be new\n");return 2;}
        std::filesystem::current_path(root);
        Scanner scanner(root.string());scanner.set_device_type(SdrDeviceType::X310);scanner.set_gain(20);
        scanner.set_lora_lock_freq(865e6);scanner.set_lora_capture_seconds(2);
        scanner.set_wifi_fixed_channel(1);scanner.set_active_band(BAND_WIFI_2G4);
        scanner.set_wifi_security_recording((root/"events.ndjson").string());
        nlohmann::json report;report["phases"]=nlohmann::json::array();bool passed=true;
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
        passed=scanner.set_wifi_cyclo_enabled(true);
#endif
        scanner.start();
        auto phase=[&](const char* name,const std::string& band,int channel,int seconds) {
            scanner.set_wifi_fixed_channel(channel?std::optional<int>(channel):std::nullopt);scanner.set_active_band(band);
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
            while(std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto s=scanner.status();
            nlohmann::json p={{"name",name},{"connected",s.connected},{"error",s.error},{"rx_stalled",s.rx_stalled},
                {"active_band",s.active_band},{"step",s.active_step_label},{"cycles",s.cycle_count},
                {"rf_rows",scanner.snapshot(band).size()},{"wifi_tail",scanner.wifi_packets().size()},
                {"lora_captures",scanner.lora_security_snapshot()->captures}};
            passed=passed&&s.connected&&s.error.empty()&&!s.rx_stalled&&s.active_band==band;
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
            auto stats=scanner.wifi_cyclo_stats();auto r=scanner.wifi_cyclo_snapshot();
            p["dsp"]={{"enabled",stats.enabled},{"measured",stats.measured},{"busy",stats.dropped_busy},
                {"failures",stats.failures},{"timeouts",stats.worker_timeouts},{"protocol_errors",stats.protocol_errors},
                {"current_result",bool(r)},{"result_band",r?r->capture.band_ghz:0},{"result_epoch",r?r->epoch:0},{"epoch",stats.epoch}};
            if(r)passed=passed&&r->epoch==stats.epoch&&r->capture.band_ghz==(band==BAND_WIFI_2G4?2:band==BAND_WIFI_5G?5:0);
#endif
            report["phases"].push_back(p);std::printf("%s connected=%d error=%s\n",name,s.connected,s.error.c_str());std::fflush(stdout);
        };
        phase("wifi24_dsp",BAND_WIFI_2G4,1,20);
        scanner.set_gain(25);phase("wifi5_gain_change",BAND_WIFI_5G,36,20);
        phase("lora_rate_change",BAND_SUB_GHZ,0,15);
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
        scanner.set_wifi_cyclo_enabled(false);
#endif
        phase("wifi24_dsp_disabled",BAND_WIFI_2G4,6,15);
        scanner.stop();scanner.start();phase("stop_start",BAND_WIFI_2G4,6,15);scanner.stop();
        auto health=scanner.wifi_enrolment_health();report["enrolment_error"]=health.error;
        report["enrolment_drops"]=health.dropped_batches;report["security"]=wifi_security::snapshot_json(*scanner.wifi_security_snapshot());
        report["lora"]=lora_security::snapshot_json(*scanner.lora_security_snapshot());
        report["passed"]=passed&&health.error.empty()&&!health.dropped_batches;
        std::ofstream out(root/"report.json");out<<report.dump(2)<<'\n';out.close();
        return out&&report["passed"].get<bool>()?0:1;
    }
    // Scanner defaults to the B210 (see scanner.hpp), and only the
    // GUI's device selector ever overrides it - so this tool used to
    // try to open a B210 regardless of what was actually plugged in,
    // failing with "No devices found for type: b200" on an X310-only
    // machine. Default to the X310 here, overridable as:
    //   ./band_smoke_test b210
    SdrDeviceType device = SdrDeviceType::X310;
    if (argc > 1 && std::string(argv[1]) == "b210") device = SdrDeviceType::B210;
    std::printf("Using radio: %s\n", device == SdrDeviceType::X310 ? "X310" : "B210");

    Scanner scanner;
    scanner.set_device_type(device);
    scanner.start();

    // Poll rather than sleeping a flat 2s. An X310 over Ethernet needs
    // several seconds to bring up its RFNoC graph, and connect_sdr()
    // itself retries up to 6 times with 1.2s between attempts (X310
    // connects fail intermittently - see its comment), so a fixed short
    // wait reported "FAILED to connect:" with an EMPTY error string:
    // nothing had gone wrong, the connection just had not finished yet.
    ScannerStatus status;
    for (int i = 0; i < 60; ++i) {  // up to ~30s
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        status = scanner.status();
        if (status.connected || !status.error.empty()) break;
    }
    if (!status.connected) {
        std::fprintf(stderr, "FAILED to connect: %s\n",
                     status.error.empty() ? "(timed out waiting for the radio)"
                                           : status.error.c_str());
        scanner.stop();
        return 1;
    }
    std::printf("Connected.\n");

    // Each dwell must outlast a FULL sweep of its band or the later
    // channels are never visited at all: 2.4GHz is 13 channels x
    // SUB_CAPTURES_PER_STEP x SUB_CAPTURE_DURATION_S = ~52s, 5GHz is 9
    // channels = ~36s. 2.4GHz used to dwell 25s, which stopped around
    // channel 6 and so reported everything above it as simply absent.
    run_band(scanner, BAND_SUB_GHZ, "LoRa / Sub-GHz ISM", 25);
    run_band(scanner, BAND_WIFI_2G4, "Wi-Fi 2.4GHz", 130);
    run_band(scanner, BAND_WIFI_5G, "Wi-Fi 5GHz", 100);

    scanner.stop();
    std::printf("\nDone.\n");
    return 0;
}
