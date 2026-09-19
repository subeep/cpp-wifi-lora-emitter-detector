// Offline only: never constructs a radio or writes emitter identity records.
#include "lora_capture.hpp"
#include "lora_observation.hpp"
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: lora_replay CAPTURE_DIRECTORY\n"; return 2; }
    try {
        auto c = rfmon::load_lora_capture(argv[1]);
        std::cout << "Source: " << c.source << "\nSamples: " << c.iq.size()
                  << "\nActual rate: " << c.sample_rate_hz << "\nRequested center: " << c.requested_center_hz
                  << "\nOverflow: " << (c.overflow ? "yes (discontinuous IQ)" : "no") << '\n';
        auto rows = rfmon::analyze_lora_capture(c.iq, c.sample_rate_hz, c.requested_center_hz);
        for (const auto& r : rows) {
            std::cout << "SF" << r.sf << " BW=" << r.bandwidth_khz << " kHz | " << r.decoder
                      << " | " << r.status << " | CRC=" << rfmon::lora_crc_label(r.crc)
                      << "\n" << r.detail << "\nHEX: " << r.payload_hex << '\n';
        }
        std::cout << rows.size() << " hypothesis results (not a unique packet count)\n";
    } catch (const std::exception& e) { std::cerr << "Replay failed: " << e.what() << '\n'; return 1; }
}
