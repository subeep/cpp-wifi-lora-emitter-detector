// Receive-only live check of OFDM format reporting: runs the production
// Scanner in a Wi-Fi mode with a temporary data root, then prints the OFDM
// PPDU counts by format (headers, and payloads where decoded), the frame
// types of decoded 802.11n/ac/ax payloads, and how many 802.11ac/ax headers were
// attributed to a decoded network.
//
//   wifi_phy_formats_check [--seconds N] [--mode wifi_2g4|wifi_5g|wifi_both] [--device x310|b210] [--rate SPS]
//                          [--data-root DIR]   (default: a temporary store; DIR writes into that store)
#include <stdlib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <thread>

#include "config.hpp"
#include "scanner.hpp"

using namespace rfmon;

int main(int argc, char** argv) {
    double seconds = 90;
    std::string mode = BAND_WIFI_5G;
    SdrDeviceType device = SdrDeviceType::X310;
    std::optional<double> rate;  // Wi-Fi capture rate override
    std::string data_root;       // empty: temporary
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (a == "--device" && i + 1 < argc) device = std::string(argv[++i]) == "b210" ? SdrDeviceType::B210 : SdrDeviceType::X310;
        else if (a == "--rate" && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (a == "--data-root" && i + 1 < argc) data_root = argv[++i];
        else { std::fprintf(stderr, "usage: %s [--seconds N] [--mode BAND] [--device x310|b210] [--rate SPS] [--data-root DIR]\n", argv[0]); return 2; }
    }
    char tmp[] = "/tmp/rfmon-wifi-formats-XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    const auto cwd = std::filesystem::current_path();
    std::filesystem::current_path(tmp);
    std::map<std::string, uint64_t> formats;
    struct Rows { uint64_t rows = 0, payload = 0, hinted = 0; };
    std::map<std::string, Rows> attributed;  // by format
    std::map<std::string, uint64_t> hints, frame_types, outcomes;
    {
        Scanner scanner(data_root.empty() ? std::string(tmp) : data_root, {});
        scanner.set_device_type(device);
        scanner.set_active_band(mode);
        if (rate && !scanner.set_wifi_sample_rate(rate)) { std::fprintf(stderr, "rate rejected\n"); return 2; }
        scanner.start();
        const auto t0 = std::chrono::steady_clock::now();
        std::map<std::string, bool> seen;  // packet rows are a bounded tail: count each once
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            for (const auto& r : scanner.wifi_packets()) {
                if (r.phy_format.empty()) continue;
                const std::string key = r.enrolment_source.empty() ? r.time + r.decode_status : r.enrolment_source;
                if (seen.count(key)) continue;
                seen[key] = true;
                auto& a = attributed[r.phy_format];
                ++a.rows;
                if (r.fcs_valid) { ++a.payload; ++frame_types[r.phy_format + " " + r.frame_type]; }
                // Payload outcome: the status after the header summary, without per-frame numbers.
                std::string outcome = r.decode_status.substr(std::min(r.decode_status.size(), r.decode_status.find("; ") + 2));
                outcome = outcome.substr(0, outcome.find(" ("));
                if (outcome.rfind("payload decoded", 0) != 0) outcome = r.decode_status.substr(r.decode_status.find("; ") + 2);
                ++outcomes[r.phy_format + ": " + outcome];
                if (!r.network_hint.empty()) { ++a.hinted; ++hints[r.network_hint]; }
            }
        }
        formats = scanner.wifi_phy_format_counts();
        scanner.stop();
    }
    std::printf("OFDM PPDUs with a valid L-SIG, by format (%s, %.0f s):\n", mode.c_str(), seconds);
    for (const auto& [k, v] : formats) std::printf("  %-40s %llu\n", k.c_str(), (unsigned long long)v);
    std::printf("Packet rows with a decoded 802.11n/ac/ax header (bounded tail, counted once):\n");
    for (const auto& [k, v] : attributed)
        std::printf("  %-20s %6llu rows, %6llu payload FCS-valid, %6llu attributed to a decoded network\n", k.c_str(),
                    (unsigned long long)v.rows, (unsigned long long)v.payload, (unsigned long long)v.hinted);
    std::printf("Payload outcome per row:\n");
    for (const auto& [k, v] : outcomes) std::printf("  %5llu  %s\n", (unsigned long long)v, k.c_str());
    std::printf("Frame types of decoded 802.11n/ac/ax payloads:\n");
    for (const auto& [k, v] : frame_types) std::printf("  %5llu  %s\n", (unsigned long long)v, k.c_str());
    std::printf("Attributions:\n");
    for (const auto& [k, v] : hints) std::printf("  %5llu  %s\n", (unsigned long long)v, k.c_str());
    std::filesystem::current_path(cwd);
    std::filesystem::remove_all(tmp);
    return 0;
}
