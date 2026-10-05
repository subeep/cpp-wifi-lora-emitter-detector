#pragma once

#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace rfmon::cyclo {

struct OfdmHypothesis {
    std::string label;
    std::size_t useful_samples = 0, prefix_samples = 0;
};

struct OfdmMeasurement {
    OfdmHypothesis hypothesis;
    std::string status = "insufficient_symbols";
    std::size_t samples_used = 0, train_symbols = 0, holdout_symbols = 0;
    std::size_t prefix_phase_samples = 0; // fitted on training samples only
    double symbol_rate_hz = 0;
    double train_prefix_coherence_squared = 0;
    double holdout_prefix_coherence_squared = 0;
    double holdout_outside_coherence_squared = 0;
    double holdout_contrast = 0;
    double holdout_symbol_cyclic_coherence_squared = 0;
};

// Timing hypotheses shared by common WLAN OFDM modes, not protocol identities.
// Only exact integer-sample timings are admitted; no implicit resampling.
std::vector<OfdmHypothesis> wlan_ofdm_hypotheses(double sample_rate_hz);

// Fold the lagged cross-product at the proposed symbol period. Fit CP phase
// on the first half, then measure that same phase on the disjoint second half.
// Coherence/contrast are measurements, not calibrated detections or probabilities.
std::vector<OfdmMeasurement> measure_ofdm_structure(
    const std::vector<std::complex<float>>& iq, double sample_rate_hz,
    const std::vector<OfdmHypothesis>& hypotheses, std::size_t max_samples = 262144);

} // namespace rfmon::cyclo
