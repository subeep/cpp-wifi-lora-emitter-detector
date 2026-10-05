#pragma once

#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace rfmon::cyclo {

struct SpectralConfig {
    double sample_rate_hz = 0; // caller-declared, never guessed
    std::size_t fft_size = 512;
    std::size_t hop_samples = 0; // auto: first coprime hop above N/2
    std::size_t max_frames = 1024;
    std::size_t max_alpha_bins = 32;
    std::size_t peak_count = 12;
    double pair_power_floor_relative = 1e-4;
};

struct CyclicPeak {
    double alpha_hz = 0;
    double frequency_offset_hz = 0;
    double coherence_squared = 0; // |mean cross-spectrum|^2 / (P_hi * P_lo)
    double normalized_cross_magnitude = 0;
};

struct SpectralFeatures {
    std::size_t fft_size = 0, hop_samples = 0, frames = 0, samples_used = 0;
    double bin_hz = 0;
    double mean_i = 0, mean_q = 0, mean_power = 0, variance_power = 0;
    double dc_fraction = 0, max_component_abs = 0;
    double spectral_flatness = 0, strongest_bin_fraction = 0;
    double occupied_low_hz = 0, occupied_high_hz = 0;
    std::string quality = "insufficient_samples";
    std::vector<std::string> warnings;
    std::vector<double> power_fraction; // fftshift order, ordinary alpha=0
    std::vector<CyclicPeak> peaks; // nonzero positive alpha; no significance claim
};

// Windowed, phase-corrected FFT cross-products on an integer alpha grid.
// This is an offline measurement prototype, not a trained drone classifier.
SpectralFeatures measure_spectral_correlation(
    const std::vector<std::complex<float>>& iq, const SpectralConfig& config);

} // namespace rfmon::cyclo
