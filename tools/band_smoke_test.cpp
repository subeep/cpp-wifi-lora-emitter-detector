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
#include <thread>

#include "../src/config.hpp"
#include "../src/scanner.hpp"

using namespace rfmon;

namespace {

void run_band(Scanner& scanner, const std::string& band, const char* label, int seconds) {
    std::printf("\n=== %s ===\n", label);
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
    auto rows = scanner.snapshot(band);
    std::printf("active emitters: %zu\n", rows.size());
    for (const auto& d : rows) {
        std::printf("  %.4f MHz  bw=%.1fkHz  power=%.1fdB  hits=%d  %s\n", d.freq_mhz,
                    d.bandwidth_khz, d.power_db, d.hit_count, d.protocol_guess.c_str());
    }
}

}  // namespace

int main() {
    Scanner scanner;
    scanner.start();

    // Give the SDR init a moment before checking connection.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    ScannerStatus status = scanner.status();
    if (!status.connected) {
        std::fprintf(stderr, "FAILED to connect: %s\n", status.error.c_str());
        scanner.stop();
        return 1;
    }
    std::printf("Connected.\n");

    run_band(scanner, BAND_SUB_GHZ, "LoRa / Sub-GHz ISM", 20);
    run_band(scanner, BAND_WIFI_2G4, "Wi-Fi 2.4GHz", 25);
    run_band(scanner, BAND_WIFI_5G, "Wi-Fi 5GHz (new - first real-hardware run)", 40);

    scanner.stop();
    std::printf("\nDone.\n");
    return 0;
}
