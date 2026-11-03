// Narrow-channel extraction for the Bluetooth/Zigbee decoders: mix one
// channel to baseband and low-pass/decimate it, block by block, so a 1 s,
// 20 MSps capture never needs a second full-size copy per channel.
//
// The FIR runs in polyphase form with the tap loop outside and the output
// loop inside, so each output accumulates independently and the inner loop
// vectorises without floating-point reassociation. Output sample k is
// aligned with input sample k*decim (the filter delay is removed; samples
// outside the input count as zero), like channelize.hpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "channelize.hpp"

namespace rfmon::shortrange {

struct Fir {
    std::vector<float> taps;
    size_t decim = 1;
};

// Low-pass passing +/-pass_hz and stopping from stop_hz (both at in_rate_hz).
inline Fir design_fir(double in_rate_hz, double pass_hz, double stop_hz, size_t decim) {
    if (!(pass_hz > 0 && stop_hz > pass_hz && stop_hz < in_rate_hz / 2 + pass_hz) || decim == 0)
        throw std::invalid_argument("Invalid channel filter");
    return {blackman_lowpass((pass_hz + stop_hz) / 2 / in_rate_hz, (stop_hz - pass_hz) / in_rate_hz), decim};
}

// Plain complex products: std::complex's operator* takes an inf/NaN-safe
// library path without -ffast-math, which dominated these loops.
inline std::complex<float> mul(std::complex<float> a, std::complex<float> b) {
    return {a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real()};
}
// a * conj(b)
inline std::complex<float> mul_conj(std::complex<float> a, std::complex<float> b) {
    return {a.real() * b.real() + a.imag() * b.imag(), a.imag() * b.real() - a.real() * b.imag()};
}

// y[k] = sum_j h[j] * mix(x)[k*D + j - c], c = (L-1)/2, mix(x)[i] =
// x[i]*exp(-j*2*pi*offset*i/rate). offset_hz == 0 skips the mixer.
inline std::vector<std::complex<float>> ddc(const std::complex<float>* x, size_t n, double rate_hz, double offset_hz,
                                            const Fir& fir) {
    const size_t D = fir.decim, L = fir.taps.size();
    if (!D || !L) throw std::invalid_argument("Invalid channel filter");
    const size_t out_n = n / D;
    std::vector<std::complex<float>> y(out_n);
    const ptrdiff_t c = ptrdiff_t(L - 1) / 2;
    const size_t M = (L + D - 1) / D;  // taps per polyphase branch
    constexpr size_t kBlock = 16384;   // outputs per block
    std::vector<float> re(D * (kBlock + M)), im(D * (kBlock + M)), acc_re(kBlock), acc_im(kBlock);
    const double w = -2 * M_PI * offset_hz / rate_hz;
    for (size_t k0 = 0; k0 < out_n; k0 += kBlock) {
        const size_t K = std::min(kBlock, out_n - k0);
        const size_t span = K + M;  // phase-array length for this block
        // De-interleave mix(x)[(k0+i)*D + p - c] into phase p, index i. The
        // float oscillator is re-anchored from double every 1024 samples.
        const ptrdiff_t first = ptrdiff_t(k0 * D) - c;
        const std::complex<float> step = std::polar(1.f, float(w));
        std::complex<float> rot(1, 0);
        for (size_t s = 0, p = 0, i = 0; s < span * D; ++s) {
            const ptrdiff_t idx = first + ptrdiff_t(s);
            std::complex<float> v(0, 0);
            if (idx >= 0 && size_t(idx) < n) {
                v = x[idx];
                if (offset_hz != 0) {
                    if ((s & 1023) == 0) rot = std::complex<float>(std::polar(1.0, std::remainder(w * double(idx), 2 * M_PI)));
                    v = mul(v, rot);
                    rot = mul(rot, step);
                }
            } else if (offset_hz != 0) {
                rot = std::complex<float>(std::polar(1.0, std::remainder(w * double(idx + 1), 2 * M_PI)));
            }
            re[p * span + i] = v.real();
            im[p * span + i] = v.imag();
            if (++p == D) { p = 0; ++i; }
        }
        std::fill(acc_re.begin(), acc_re.begin() + K, 0.f);
        std::fill(acc_im.begin(), acc_im.begin() + K, 0.f);
        for (size_t j = 0; j < L; ++j) {
            const float h = fir.taps[j];
            const size_t p = j % D, m = j / D;
            const float* r = re.data() + p * span + m;
            const float* q = im.data() + p * span + m;
            for (size_t k = 0; k < K; ++k) {
                acc_re[k] += h * r[k];
                acc_im[k] += h * q[k];
            }
        }
        for (size_t k = 0; k < K; ++k) y[k0 + k] = {acc_re[k], acc_im[k]};
    }
    return y;
}

// Fast atan2 (max error ~0.005 rad), enough for FM discrimination.
inline float fast_atan2(float y, float x) {
    const float ax = std::abs(x), ay = std::abs(y);
    if (ax == 0 && ay == 0) return 0;
    const float a = std::min(ax, ay) / std::max(ax, ay), s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

// Phase change from x[n-lag] to x[n] (rad); out[n] = 0 for n < lag.
inline std::vector<float> discriminate(const std::vector<std::complex<float>>& x, size_t lag) {
    std::vector<float> d(x.size(), 0.f);
    for (size_t n = lag; n < x.size(); ++n) {
        const auto v = mul_conj(x[n], x[n - lag]);
        d[n] = fast_atan2(v.imag(), v.real());
    }
    return d;
}

// Sample ranges worth demodulating: blocks whose power exceeds the
// channel's own noise floor (20th-percentile block power) by `margin_db`,
// merged when closer than `pad` and widened by `pad` on each side. A
// channel busy for the whole capture yields one span covering all of it.
inline std::vector<std::pair<size_t, size_t>> active_spans(const std::vector<std::complex<float>>& x, size_t block,
                                                           double margin_db, size_t pad) {
    std::vector<std::pair<size_t, size_t>> spans;
    const size_t nb = x.size() / block;
    if (nb < 8) {
        if (!x.empty()) spans.push_back({0, x.size()});
        return spans;
    }
    std::vector<float> p(nb);
    for (size_t b = 0; b < nb; ++b) {
        float s = 0;
        for (size_t i = b * block; i < (b + 1) * block; ++i) s += std::norm(x[i]);
        p[b] = s;
    }
    std::vector<float> sorted(p);
    std::nth_element(sorted.begin(), sorted.begin() + ptrdiff_t(nb / 5), sorted.end());
    const float floor = sorted[nb / 5], threshold = floor * float(std::pow(10.0, margin_db / 10));
    for (size_t b = 0; b < nb; ++b) {
        if (!(p[b] > threshold)) continue;
        const size_t lo = b * block > pad ? b * block - pad : 0, hi = std::min(x.size(), (b + 1) * block + pad);
        if (!spans.empty() && lo <= spans.back().second) spans.back().second = std::max(spans.back().second, hi);
        else spans.push_back({lo, hi});
    }
    return spans;
}

// Mean power over [begin, end) in dB relative to full scale (1.0).
inline double power_db(const std::vector<std::complex<float>>& x, size_t begin, size_t end) {
    end = std::min(end, x.size());
    if (begin >= end) return -200;
    double sum = 0;
    for (size_t i = begin; i < end; ++i) sum += std::norm(x[i]);
    return 10 * std::log10(sum / double(end - begin) + 1e-20);
}

}  // namespace rfmon::shortrange
