#pragma once
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace rfmon::cyclo {
inline constexpr const char* chirp_discovery_version = "bounded_linear_sweep_discovery_v2";
inline constexpr std::size_t sweep_partition_limit = 8192, sweep_candidate_limit = 3;
struct LinearSweep {
    std::size_t span_samples=0, discovery_offset=0, holdout_offset=0, holdout_trials=0;
    double slope_hz_per_second=0, sweep_hz=0;
    double discovery_center_hz=0, holdout_center_hz=0;
    double discovery_rmse_fraction=0, holdout_rmse_fraction=0;
    double discovery_coherence_squared=0, holdout_coherence_squared=0;
    double discovery_amplitude_cv=0, holdout_amplitude_cv=0;
    bool holdout_supported=false, pattern_consistent=false;
};
struct ChirpDiscovery {
    std::string status="insufficient_samples";
    std::size_t samples_examined=0, partition_samples=0, holdout_partition_offset=0;
    std::size_t discovery_trials=0, discovery_continuation_trials=0;
    std::vector<LinearSweep> candidates;
};
std::size_t sweep_window_count(std::size_t partition, std::size_t span);
// Eight principal phase increments averaged; no unwrap/alias recovery. Select span/slope
// on the first partition; hold slope/span fixed while searching the last one.
// Carrier is a separately fitted nuisance parameter in every holdout window.
// This is a bounded shape diagnostic, not CSS decoding or emitter identity.
ChirpDiscovery discover_linear_sweeps(const std::vector<std::complex<float>>& iq,double rate_hz);
} // namespace rfmon::cyclo
