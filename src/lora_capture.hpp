#pragma once
#include <complex>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include "capture_timing.hpp"
namespace rfmon {
struct LoraCapture {
    std::vector<std::complex<float>> iq;
    double sample_rate_hz = 0;
    double requested_sample_rate_hz = 0;
    double requested_center_hz = 0;
    double requested_duration_s = 0;
    double host_start_unix_s = 0; // host capture-call timestamp, not hardware time
    std::optional<double> requested_gain_db;
    bool overflow = false;
    std::string device_args;
    std::string antenna;
    std::string source = "live";
    std::string run_id;
    uint64_t capture_seq = 0, radio_session = 0;
    std::optional<CaptureTiming> timing; // absent in legacy v1 recordings

};
// Versioned manifest + little-endian float32 IQ. Throws on IO/validation errors.
// Each save creates a new directory; existing captures are never overwritten.
std::string save_lora_capture(const std::string& parent, const LoraCapture& capture);
LoraCapture crop_lora_capture(const LoraCapture& source, size_t begin, size_t end);
LoraCapture load_lora_capture(const std::string& directory);
} // namespace rfmon
