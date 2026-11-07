#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace rfmon::lora_decimator {

// Decimates LoRa IQ by an integer factor to one sample per chip (the rate the
// PHY codec expects). The former filter was a plain boxcar (integrate over one
// chip); at 500 kHz -> 125 kHz it rejects a neighbour 200 kHz away (EU868
// spacing) by only ~12 dB, so a packet on the next channel decoded on this
// one, even CRC-valid (seen live: 866.9 MHz on 867.1 MHz at +50 kHz). This
// keeps the boxcar and adds a Kaiser low-pass at the output rate in front of
// it (one combined FIR): in band the response matches the boxcar, which the
// decoder's sub-chip timing behaviour was tuned on, while everything that
// would fold into the band from >= ~1.2 * out_rate away is removed (>= 60 dB
// at 200 kHz). Factor 2 (BW250) is the boxcar unchanged (cut-off = Nyquist).
// Unit DC gain. Output sample k is centred on input k*factor + (factor-1)/2,
// the same instant as the boxcar's, so packet timing is unchanged.
// factor <= 1 returns the input. taps() is exposed for tests.

namespace detail {
constexpr double kPi = 3.14159265358979323846;

inline double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}
}  // namespace detail

inline std::vector<float> taps(int factor) {
    if (factor <= 1) return {1.0f};
    // Kaiser low-pass, cut-off (-6 dB) at the output rate (fraction of input:
    // 1/factor, at most Nyquist), 12*factor+1 taps, beta 6 (~63 dB).
    const double cutoff = std::min(1.0 / factor, 0.5);
    const int lp_length = 12 * factor + 1;
    const double beta = 6.0, centre = 0.5 * (lp_length - 1), i0b = detail::bessel_i0(beta);
    std::vector<double> lp(static_cast<size_t>(lp_length));
    for (int j = 0; j < lp_length; ++j) {
        const double t = j - centre;
        const double x = 2 * cutoff * t;
        const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(detail::kPi * x) / (detail::kPi * x);
        const double r = t / centre;
        lp[size_t(j)] = sinc * detail::bessel_i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0b;
    }
    // Convolve with the boxcar of `factor`; length lp_length + factor - 1 has
    // the parity of `factor`, so the centre lands on k*factor + (factor-1)/2.
    std::vector<double> h(lp.size() + size_t(factor) - 1, 0.0);
    for (size_t j = 0; j < lp.size(); ++j)
        for (int b = 0; b < factor; ++b) h[j + size_t(b)] += lp[j];
    double sum = 0;
    for (double v : h) sum += v;
    std::vector<float> out(h.size());
    for (size_t j = 0; j < h.size(); ++j) out[j] = float(h[j] / sum);
    return out;
}

inline std::vector<std::complex<float>> decimate(const std::vector<std::complex<float>>& in, int factor) {
    if (factor <= 1) return in;
    const auto h = taps(factor);
    const long length = long(h.size());
    // First tap index relative to k*factor: centre at (factor-1)/2.
    const long offset = (long(factor) - 1) / 2 - (length - 1) / 2;
    const long n = long(in.size());
    std::vector<std::complex<float>> out(in.size() / size_t(factor));
    for (size_t k = 0; k < out.size(); ++k) {
        const long start = long(k) * factor + offset;
        float re = 0, im = 0;
        if (start >= 0 && start + length <= n) {
            const auto* x = in.data() + start;
            for (long j = 0; j < length; ++j) { re += h[size_t(j)] * x[j].real(); im += h[size_t(j)] * x[j].imag(); }
        } else {
            for (long j = 0; j < length; ++j) {
                const long i = start + j;
                if (i < 0 || i >= n) continue;
                re += h[size_t(j)] * in[size_t(i)].real();
                im += h[size_t(j)] * in[size_t(i)].imag();
            }
        }
        out[k] = {re, im};
    }
    return out;
}

}  // namespace rfmon::lora_decimator
