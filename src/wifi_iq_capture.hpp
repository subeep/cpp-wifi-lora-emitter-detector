#pragma once
#include "capture_timing.hpp"
#include <nlohmann/json.hpp>
#include <complex>
#include <string>
#include <vector>
namespace rfmon {
struct WifiIqCapture {
    nlohmann::json metadata;
    std::vector<std::complex<float>> iq;
    std::optional<CaptureTiming> timing; // absent for schema 1; never invent an anchor
};
// Local filename only, bounded allocation, checksum and finite-IQ validation.
WifiIqCapture load_wifi_iq_capture(const std::string& manifest);
// Writes IQ first; a renamed manifest is the commit marker. Refuses overwrite.
void save_wifi_iq_capture(const std::string& manifest, nlohmann::json metadata,
                         const std::vector<std::complex<float>>& iq, const CaptureTiming& timing);
} // namespace rfmon
