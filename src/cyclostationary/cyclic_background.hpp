#pragma once
#include "waveform_features.hpp"

namespace rfmon::cyclo {
inline constexpr const char* cyclic_background_version="local_caf_background_v1";
struct BackgroundPartition {
    std::size_t reference_bins=0;
    double median_coherence_squared=0, upper_coherence_squared=0;
    double line_to_median=0, line_to_upper=0;
    double block_phase_coherence_squared=0, min_block_energy_fraction=0;
};
struct CyclicBackgroundPeak {
    BackgroundPartition discovery, holdout;
    bool background_supported=false;
};
struct CyclicBackground {
    std::string status="insufficient_samples";
    std::size_t samples_examined=0, partition_samples=0, holdout_offset=0, fft_calls=0;
    std::vector<CyclicBackgroundPeak> peaks;
};
// Local same-sign coarse-grid reference bins, 4..20 bins from the discovery
// peak. This is a coloured-background diagnostic, not a calibrated null law.
std::vector<std::size_t> cyclic_reference_bins(std::size_t partition,int coarse_bin);
bool cyclic_background_supported(const CyclicBackgroundPeak& p,const RatePeak& raw);
CyclicBackground measure_cyclic_background(const std::vector<std::complex<float>>& iq,
    double rate_hz,const WaveformFeatures& selected);
} // namespace rfmon::cyclo
