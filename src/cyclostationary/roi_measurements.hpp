#pragma once
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace rfmon::cyclo {
inline constexpr std::size_t roi_block_samples = 128, roi_region_limit = 8;
inline constexpr std::size_t roi_fft_size = 512, roi_frame_limit = 32, roi_band_limit = 3;
struct FrequencyInterval {
    double low_hz = 0, high_hz = 0, power_fraction = 0;
    bool edge_bin = false, contains_dc = false;
};
struct EnergyRegion {
    std::size_t offset = 0, samples = 0, spectral_frames = 0, spectral_samples = 0, bands_seen = 0;
    double energy_fraction = 0, occupied_low_hz = 0, occupied_high_hz = 0;
    bool contrast_selected = false, touches_window_edge = false;
    std::string spectral_status = "insufficient_samples";
    std::vector<FrequencyInterval> bands; // spectral intervals, not emitter identities
};
struct RoiMeasurements {
    std::size_t samples_examined = 0, blocks = 0, regions_seen = 0, contrast_samples = 0;
    double dc_fraction = 0, background_to_mean = 0, high_threshold_to_mean = 0;
    std::string status = "insufficient_samples";
    std::vector<EnergyRegion> regions; // strongest <=8, returned in sample order
};
// Block energy and ordinary PSD only; no waveform/drone decision or filtering.
// Worker-local IQ only, <=65536 samples. Mean removal and quantile proxies are
// disclosed; thresholds are selection heuristics, not significance tests.
RoiMeasurements measure_rois(const std::vector<std::complex<float>>& iq, double sample_rate_hz);
} // namespace rfmon::cyclo
