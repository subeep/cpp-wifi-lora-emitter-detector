// Derive one narrow channel from a wideband capture: mix the channel to
// baseband, then low-pass and decimate by an integer factor (one or two
// Blackman-windowed-sinc stages). Used by coordinated mode to obtain LoRa
// channels at exactly 500 kHz (lora_phy.hpp needs rate == bandwidth) from
// the one 5 MSps sub-GHz capture, instead of retuning once per channel.
//
// Output sample k is aligned with input sample k*D (filter delay removed;
// the first/last half filter length are computed against zero padding), so
// sample-index provenance scales exactly by D.
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace rfmon {

// Unity-DC-gain Blackman-windowed sinc low-pass. `cutoff` and `transition`
// are in cycles per input sample (0..0.5).
inline std::vector<float> blackman_lowpass(double cutoff, double transition) {
    if (!(cutoff > 0 && cutoff < 0.5 && transition > 0)) throw std::invalid_argument("Invalid low-pass design");
    int n = int(std::ceil(5.5 / transition)) | 1;  // Blackman: ~5.5/N transition, odd length
    if (n > 2047) n = 2047;
    std::vector<double> h(static_cast<size_t>(n));
    const double m = (n - 1) / 2.0;
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double t = i - m;
        const double sinc = t == 0 ? 2 * cutoff : std::sin(2 * M_PI * cutoff * t) / (M_PI * t);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) + 0.08 * std::cos(4 * M_PI * i / (n - 1));
        h[size_t(i)] = sinc * w;
        sum += h[size_t(i)];
    }
    std::vector<float> out(h.size());
    for (size_t i = 0; i < h.size(); ++i) out[i] = float(h[i] / sum);
    return out;
}

// y[k] = sum_j h[j] * x[k*d + j - (L-1)/2], zero outside x.
inline std::vector<std::complex<float>> decimate_fir(const std::vector<std::complex<float>>& x,
                                                     const std::vector<float>& h, size_t d) {
    if (d == 0) throw std::invalid_argument("Decimation factor must be positive");
    const size_t out_n = x.size() / d;
    const ptrdiff_t half = ptrdiff_t(h.size() - 1) / 2;
    std::vector<std::complex<float>> y(out_n);
    for (size_t k = 0; k < out_n; ++k) {
        const ptrdiff_t base = ptrdiff_t(k * d) - half;
        size_t j0 = base < 0 ? size_t(-base) : 0;
        size_t j1 = h.size();
        if (base + ptrdiff_t(j1) > ptrdiff_t(x.size())) j1 = size_t(ptrdiff_t(x.size()) - base);
        float re = 0, im = 0;
        const std::complex<float>* src = x.data() + base;
        for (size_t j = j0; j < j1; ++j) {
            re += h[j] * src[j].real();
            im += h[j] * src[j].imag();
        }
        y[k] = {re, im};
    }
    return y;
}

// Channel centred `offset_hz` from the capture centre, at `out_rate_hz`.
// in_rate/out_rate must be an integer >= 1. Pass band +/-0.48*out_rate,
// stop band from +/-0.52*out_rate (the outer 4% of the output Nyquist band
// is the transition, like any real receiver's decimation filter).
inline std::vector<std::complex<float>> derive_channel(const std::vector<std::complex<float>>& wide, double in_rate_hz,
                                                       double offset_hz, double out_rate_hz) {
    const double ratio = in_rate_hz / out_rate_hz;
    if (!std::isfinite(ratio) || ratio < 1 || std::abs(ratio - std::round(ratio)) > 1e-9)
        throw std::invalid_argument("Channel rate must divide the capture rate");
    const size_t d = size_t(std::llround(ratio));
    std::vector<std::complex<float>> x(wide.size());
    const double step = -2 * M_PI * offset_hz / in_rate_hz;
    for (size_t i = 0; i < wide.size(); ++i) {
        const double ph = std::remainder(step * double(i), 2 * M_PI);
        x[i] = wide[i] * std::complex<float>(float(std::cos(ph)), float(std::sin(ph)));
    }
    if (d == 1) return x;
    if (d % 2 == 0 && d > 2) {
        // Cheap wide first stage to 2x the output rate, sharp final halfband-like stage.
        const size_t d1 = d / 2;
        const double mid_rate = in_rate_hz / double(d1);
        const double pass = 0.52 * out_rate_hz, stop = mid_rate - 0.52 * out_rate_hz;
        x = decimate_fir(x, blackman_lowpass((pass + stop) / 2 / in_rate_hz, (stop - pass) / in_rate_hz), d1);
        return decimate_fir(x, blackman_lowpass(0.5 * out_rate_hz / mid_rate, 0.04 * out_rate_hz / mid_rate), 2);
    }
    return decimate_fir(x, blackman_lowpass(0.5 * out_rate_hz / in_rate_hz, 0.04 * out_rate_hz / in_rate_hz), d);
}

}  // namespace rfmon
