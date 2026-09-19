#pragma once
#include "wifi_frame.hpp"
#include <complex>
#include <string>
#include <vector>
namespace rfmon::wifi {
// Legacy 20 MHz OFDM only. Diagnostic progress never constitutes decoded identity.
struct OfdmDecodeResult {
    std::string status = "No OFDM preamble";
    bool preamble_found = false, header_valid = false, fcs_valid = false;
    int rate_mbps = 0;
    size_t psdu_length = 0, samples_required = 0, ltf_start = 0;
    double cfo_hz = 0, ltf_correlation = 0;
    std::vector<uint8_t> mpdu;
    std::optional<BeaconInfo> beacon;
};
OfdmDecodeResult decode_ofdm_burst(const std::complex<float>* x, size_t n,
    double sample_rate_hz, double capture_center_hz, double channel_center_hz);
}
