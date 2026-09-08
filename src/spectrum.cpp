#include "spectrum.hpp"

#include <kissfft/kiss_fft.h>

#include <algorithm>
#include <cmath>

namespace rfmon {

namespace {

// Periodic Hann window (matches the "fftbins=True"/spectral-analysis
// convention scipy.signal.stft uses by default).
std::vector<float> hann_window(int n) {
    std::vector<float> w(n);
    for (int k = 0; k < n; ++k) {
        w[k] = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) * k / n));
    }
    return w;
}

}  // namespace

Spectrum spectrogram_max_db(const std::vector<std::complex<float>>& iq, double sample_rate_hz,
                             int nperseg) {
    int n = std::min<int>(nperseg, static_cast<int>(iq.size()));
    Spectrum result;

    if (n < 8) {
        // Degenerate short capture: fall back to a single whole-buffer FFT.
        n = static_cast<int>(iq.size());
        if (n == 0) return result;
        kiss_fft_cfg cfg = kiss_fft_alloc(n, 0, nullptr, nullptr);
        std::vector<kiss_fft_cpx> in(n), out(n);
        for (int k = 0; k < n; ++k) {
            in[k].r = iq[k].real();
            in[k].i = iq[k].imag();
        }
        kiss_fft(cfg, in.data(), out.data());
        kiss_fft_free(cfg);

        result.freqs_offset_hz.resize(n);
        result.psd_db.resize(n);
        double bin_hz = sample_rate_hz / n;
        for (int j = 0; j < n; ++j) {
            int orig = (j + n / 2) % n;
            double mag2 = double(out[orig].r) * out[orig].r + double(out[orig].i) * out[orig].i;
            double power = mag2 / (double(n) * n);
            result.psd_db[j] = 10.0 * std::log10(power + 1e-15);
            result.freqs_offset_hz[j] = (j - n / 2) * bin_hz;
        }
        return result;
    }

    int num_segments = static_cast<int>(iq.size()) / n;
    std::vector<float> window = hann_window(n);
    std::vector<double> max_power(n, 0.0);

    kiss_fft_cfg cfg = kiss_fft_alloc(n, 0, nullptr, nullptr);
    std::vector<kiss_fft_cpx> in(n), out(n);

    for (int seg = 0; seg < num_segments; ++seg) {
        const std::complex<float>* base = iq.data() + size_t(seg) * n;
        for (int k = 0; k < n; ++k) {
            in[k].r = base[k].real() * window[k];
            in[k].i = base[k].imag() * window[k];
        }
        kiss_fft(cfg, in.data(), out.data());
        for (int k = 0; k < n; ++k) {
            double mag2 = double(out[k].r) * out[k].r + double(out[k].i) * out[k].i;
            if (mag2 > max_power[k]) max_power[k] = mag2;
        }
    }
    kiss_fft_free(cfg);

    result.freqs_offset_hz.resize(n);
    result.psd_db.resize(n);
    double bin_hz = sample_rate_hz / n;
    for (int j = 0; j < n; ++j) {
        int orig = (j + n / 2) % n;
        result.psd_db[j] = 10.0 * std::log10(max_power[orig] + 1e-15);
        result.freqs_offset_hz[j] = (j - n / 2) * bin_hz;
    }
    return result;
}

Spectrum max_hold_spectrum(const std::vector<std::vector<std::complex<float>>>& captures,
                            double sample_rate_hz, int nperseg) {
    Spectrum held;
    for (const auto& iq : captures) {
        Spectrum s = spectrogram_max_db(iq, sample_rate_hz, nperseg);
        if (s.psd_db.empty()) continue;
        if (held.psd_db.empty()) {
            held = std::move(s);
        } else {
            for (size_t i = 0; i < held.psd_db.size() && i < s.psd_db.size(); ++i) {
                held.psd_db[i] = std::max(held.psd_db[i], s.psd_db[i]);
            }
        }
    }
    return held;
}

std::vector<bool> mask_dc_guard(const std::vector<double>& freqs_offset_hz, double guard_hz) {
    std::vector<bool> mask(freqs_offset_hz.size());
    for (size_t i = 0; i < freqs_offset_hz.size(); ++i) {
        mask[i] = std::abs(freqs_offset_hz[i]) > guard_hz;
    }
    return mask;
}

std::vector<bool> mask_edge_guard(const std::vector<double>& freqs_offset_hz,
                                   double sample_rate_hz, double edge_fraction) {
    double nyquist_hz = sample_rate_hz / 2.0;
    double cutoff_hz = nyquist_hz * (1.0 - edge_fraction);
    std::vector<bool> mask(freqs_offset_hz.size());
    for (size_t i = 0; i < freqs_offset_hz.size(); ++i) {
        mask[i] = std::abs(freqs_offset_hz[i]) < cutoff_hz;
    }
    return mask;
}

}  // namespace rfmon
