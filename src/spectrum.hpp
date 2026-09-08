// Build a max-hold detection spectrum from one or more IQ captures.
//
// A time-averaged PSD hides short, low-duty-cycle transmissions (a
// single Wi-Fi beacon occupying ~0.3% of a 150ms window averages down
// to roughly noise-floor level). This instead computes a short-time
// spectrogram (many short Hann-windowed FFT segments across time) and
// keeps the per-frequency-bin *maximum* across every segment and every
// capture - the standard spectrum-analyzer "max hold" technique.

#pragma once

#include <complex>
#include <vector>

namespace rfmon {

constexpr int SPECTRUM_DEFAULT_NPERSEG = 2048;

struct Spectrum {
    std::vector<double> freqs_offset_hz;  // relative to the tuned center; 0 == tuned center
    std::vector<double> psd_db;
};

// One capture -> max-hold spectrum across its short-time segments.
Spectrum spectrogram_max_db(const std::vector<std::complex<float>>& iq,
                             double sample_rate_hz,
                             int nperseg = SPECTRUM_DEFAULT_NPERSEG);

// Multiple captures (same center/rate) -> max-hold across segments AND
// across captures.
Spectrum max_hold_spectrum(const std::vector<std::vector<std::complex<float>>>& captures,
                            double sample_rate_hz,
                            int nperseg = SPECTRUM_DEFAULT_NPERSEG);

// True everywhere EXCEPT within +/- guard_hz of the tuned center (0 Hz
// offset) - i.e. True means "trustworthy", matching mask_edge_guard.
std::vector<bool> mask_dc_guard(const std::vector<double>& freqs_offset_hz, double guard_hz);

// True everywhere EXCEPT the outer edge_fraction of Nyquist on either side.
std::vector<bool> mask_edge_guard(const std::vector<double>& freqs_offset_hz,
                                   double sample_rate_hz, double edge_fraction);

}  // namespace rfmon
