#pragma once
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace rfmon::cyclo {
inline constexpr const char* waveform_features_version = "bounded_caf_morphology_v1";
inline constexpr std::size_t cyclic_partition_limit = 8192, cyclic_peak_limit = 8;
inline constexpr std::size_t cyclic_lags[] = {0, 1, 4, 16};
struct RatePeak {
    bool conjugate = false;
    std::size_t lag_samples = 0;
    double coarse_alpha_hz = 0, alpha_hz = 0;
    double discovery_coherence_squared = 0, holdout_coherence_squared = 0;
    bool persistent_pattern = false; // heuristic, not a probability or identity
};
struct WaveformFeatures {
    std::string cyclic_status = "insufficient_samples", morphology_status = "insufficient_samples";
    std::size_t samples_examined = 0, partition_samples = 0, holdout_offset = 0;
    std::size_t fft_calls = 0, refinement_evaluations = 0, phase_pairs = 0;
    double alpha_bin_hz = 0;
    std::vector<RatePeak> peaks;
    double amplitude_cv = 0, envelope_low = 0, envelope_high = 0;
    double envelope_high_fraction = 0, envelope_fit_residual = 0, envelope_contrast = 0;
    std::size_t envelope_transitions = 0;
    bool two_level_envelope_pattern = false;
    double frequency_low_hz = 0, frequency_high_hz = 0;
    double frequency_first_fraction = 0, frequency_second_fraction = 0, frequency_concentration = 0;
    std::size_t frequency_transitions = 0;
    bool two_frequency_pattern = false;
};
// <=65536 contiguous samples. Discovery and holdout have separate means and a
// >=16-sample guard. No phase joining, signal identity or learned classifier.
WaveformFeatures measure_waveform_features(const std::vector<std::complex<float>>& iq, double rate_hz);
} // namespace rfmon::cyclo
