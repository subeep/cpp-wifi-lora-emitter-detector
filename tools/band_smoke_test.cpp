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
