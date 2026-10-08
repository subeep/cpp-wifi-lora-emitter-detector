#pragma once
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace rfmon::cyclo {
inline constexpr const char* structure_discovery_version="bounded_ofdm_barker_discovery_v1";
inline constexpr std::size_t structure_partition_limit=8192, structure_candidate_limit=4;
struct DiscoveredOfdm {
    std::size_t useful_samples=0,prefix_samples=0,phase_samples=0,symbols_per_partition=0;
    double lag_coherence_squared=0,train_prefix=0,train_outside=0,holdout_prefix=0,holdout_outside=0,holdout_cyclic=0;
    bool pattern_consistent=false;
};
struct SpreadMeasurement {
    std::size_t chip_samples=0,code_phase_samples=0,train_words=0,holdout_words=0;
    double carrier_hz=0,train_code_coherence_squared=0,holdout_code_coherence_squared=0;
    double train_other_phase=0,holdout_other_phase=0;
    bool pattern_consistent=false;
};
struct StructureDiscovery {
    std::string status="insufficient_samples";
    std::size_t samples_examined=0,partition_samples=0,holdout_offset=0,max_lag=0;
    std::size_t ofdm_fft_calls=0,timing_hypotheses=0,spread_hypotheses=0;
    double carrier_estimate_hz=0,carrier_coherence_squared=0;
    std::vector<DiscoveredOfdm> ofdm;
    std::vector<SpreadMeasurement> spread;
};
// Separate means/discovery-only selection. Same contiguous tile only; <=65536
// samples. No protocol, modulation-order, drone, role or probability decision.
StructureDiscovery discover_waveform_structure(const std::vector<std::complex<float>>& iq,double rate_hz);
} // namespace rfmon::cyclo
