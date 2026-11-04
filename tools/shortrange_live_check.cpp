// Receive-only live check of the Bluetooth LE / 802.15.4 decoders: drives the
// production Scanner in a 2.4 GHz mode with the shortrange Monitor as its
// Wi-Fi IQ observer, then prints coverage and the decoded address lists.
// Nothing is transmitted. Stores go to a temporary directory.
//
//   shortrange_live_check [--seconds N] [--mode wifi_2g4|wifi_both|coordinated]
//                         [--device x310|b210] [--export-dir PATH]
#include <stdlib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "config.hpp"
#include "scanner.hpp"
#include "shortrange/monitor.hpp"

using namespace rfmon;

int main(int argc, char** argv) {
    double seconds = 60;
    std::string mode = BAND_WIFI_2G4, export_dir;
    SdrDeviceType device = SdrDeviceType::X310;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (a == "--export-dir" && i + 1 < argc) export_dir = argv[++i];
        else if (a == "--device" && i + 1 < argc) device = std::string(argv[++i]) == "b210" ? SdrDeviceType::B210 : SdrDeviceType::X310;
        else { std::fprintf(stderr, "usage: %s [--seconds N] [--mode BAND] [--device x310|b210] [--export-dir PATH]\n", argv[0]); return 2; }
    }
    char tmp[] = "/tmp/rfmon-shortrange-live-XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    const auto cwd = std::filesystem::current_path();
    std::filesystem::current_path(tmp);

    shortrange::Monitor monitor;
    monitor.enable(true);
    {
        Scanner scanner(tmp, [&](const auto& iq, const auto& r) { monitor.observe(iq, r); });
        scanner.set_device_type(device);
        scanner.set_active_band(mode);
        const auto t0 = std::chrono::steady_clock::now();
        scanner.start();
        double next = 10;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (el >= seconds) break;
            if (el >= next) {
                next += 10;
                const auto s = monitor.snapshot();
                const auto st = scanner.status();
                std::printf("[%5.0fs] connected=%d step=%s | offered %llu decoded %llu dropped %llu outside %llu | "
                            "BLE pkts %llu addrs %zu | 802.15.4 frames %llu addrs %zu | last %.2fs for %.2fs capture\n",
                            el, int(st.connected), st.active_step_label.c_str(), (unsigned long long)s.offered,
                            (unsigned long long)s.decoded_captures, (unsigned long long)s.dropped_busy,
                            (unsigned long long)s.outside_band, (unsigned long long)s.ble_packets, s.ble.size(),
                            (unsigned long long)s.zigbee_frames, s.zigbee.size(), s.last_processing_s, s.last_capture_s);
                std::fflush(stdout);
            }
        }
        scanner.stop();
    }
    monitor.enable(false);
    const auto s = monitor.snapshot();
    std::printf("\nCoverage (capture seconds decoded / captures / CRC-valid packets / syncs / CRC failures):\n");
    for (const auto* cov : {&s.ble_coverage, &s.zigbee_coverage})
        for (const auto& c : *cov)
            if (c.captures)
                std::printf("  %-10s %7.1f MHz  %6.1f s  %4llu  %5llu  %5llu  %5llu\n", c.label.c_str(), c.frequency_hz / 1e6, c.seconds,
                            (unsigned long long)c.captures, (unsigned long long)c.packets, (unsigned long long)c.syncs,
                            (unsigned long long)c.failures);
    std::printf("\nBluetooth LE addresses: %zu (packets %llu)\n", s.ble.size(), (unsigned long long)s.ble_packets);
    for (const auto& d : s.ble) {
        std::string tags, ch;
        for (const auto& t : d.tags) tags += t + ";";
        for (const auto& [c, k] : d.channel_counts) ch += std::to_string(c) + ":" + std::to_string(k) + " ";
        std::printf("  %s %-28s pkts %-4llu %6.1f dB  ch %-14s %s | %s | %s %s\n", d.address.c_str(), d.address_kind.c_str(),
                    (unsigned long long)d.packets, d.max_power_db, ch.c_str(), d.name.empty() ? "-" : d.name.c_str(),
                    d.company.empty() ? (d.vendor.empty() ? "-" : d.vendor.c_str()) : d.company.c_str(), tags.c_str(),
                    d.remote_id.empty() ? "" : "[Remote ID]");
    }
    std::printf("\n802.15.4 / Zigbee addresses: %zu (frames %llu)\n", s.zigbee.size(), (unsigned long long)s.zigbee_frames);
    for (const auto& d : s.zigbee) {
        std::string roles;
        for (const auto& r : d.roles) roles += r + ";";
        std::printf("  %-28s %-10s frames %-4llu %6.1f dB %s %s\n", d.address.c_str(), d.network.c_str(), (unsigned long long)d.frames,
                    d.max_power_db, roles.c_str(), d.vendor.c_str());
    }
    std::printf("\nRecent 802.15.4 frames (newest first):\n");
    for (size_t i = 0; i < s.recent_zigbee.size() && i < 15; ++i)
        std::printf("  ch %d %6.1f dB  %s  %s\n", s.recent_zigbee[i].channel, s.recent_zigbee[i].power_db, s.recent_zigbee[i].summary.c_str(),
                    s.recent_zigbee[i].source.c_str());
    if (!export_dir.empty()) std::printf("\nExported: %s\n", monitor.export_json(export_dir).c_str());
    std::filesystem::current_path(cwd);
    std::filesystem::remove_all(tmp);
    return 0;
}
