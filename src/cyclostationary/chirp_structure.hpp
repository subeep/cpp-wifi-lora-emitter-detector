#pragma once
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace rfmon::cyclo {
struct ChirpMeasurement {
    std::string label, status = "insufficient_samples";
    double slope_hz_per_second = 0, nominal_sweep_hz = 0;
    std::size_t lag_samples = 0, gate_samples = 0, positions_examined = 0, positions_eligible = 0;
    std::size_t peak_offset = 0;
    double peak_coherence_squared = 0, first_half_coherence_squared = 0, second_half_coherence_squared = 0;
    double half_energy_balance = 0;
    std::string frequency_status = "unavailable";
    std::size_t frequency_pairs = 0, phase_unwraps = 0;
    double frequency_slope_hz_per_second = 0, frequency_rmse_hz = 0;
    double frequency_mean_hz = 0; // relative to this analysis window's center
};
// Research hypotheses from Cwalina et al., Sensors 2025, DOI 10.3390/s25154552.
// Frequency-shifted lag correlation, using two 32 us fragments. Every full
// window position is examined; the selected maximum is NOT a significance
// test or link identity. <=65536 contiguous samples, three fixed hypotheses.
// Full signal passband, receiver quality and independent confusables still
// need qualification. No periodicity, drone, vendor, role or RID inference.
std::vector<ChirpMeasurement> measure_chirp_structure(
    const std::vector<std::complex<float>>& iq, double sample_rate_hz);
} // namespace rfmon::cyclo
