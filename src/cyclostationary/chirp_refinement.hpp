#pragma once
#include "chirp_structure.hpp"
namespace rfmon::cyclo {
inline constexpr const char* chirp_refinement_version = "offline_dechirp_refinement_v1";
struct ChirpRefinement {
    std::string label, status = "insufficient_samples";
    double nominal_slope_hz_per_second = 0, selected_slope_hz_per_second = 0;
    std::size_t span_samples = 0, peak_offset = 0, positions_examined = 0, spectra_examined = 0;
    double coarse_coherence_squared = 0, dechirped_band_power_fraction = 0;
    double first_half_band_power_fraction = 0, second_half_band_power_fraction = 0, half_energy_balance = 0;
    double dechirped_band_center_hz = 0, dechirped_band_width_hz = 0;
    double estimated_span_mid_frequency_hz = 0;
    bool shape_consistent = false; // heuristic morphology only; never drone/link identity
};
// Offline-only bounded research diagnostic. At most 65536 samples, 3 nominal
// hypotheses x 5 fixed slope factors x 5 shortlisted full-span FFT checks.
// A wider collapsed dechirp band permits limited echoes; unknown emission
// types can also match. Does not replace v1 policy, child or live measurements.
std::vector<ChirpRefinement> refine_chirp_candidates(
    const std::vector<std::complex<float>>& iq, double rate,
    const std::vector<ChirpMeasurement>& legacy);
} // namespace rfmon::cyclo
