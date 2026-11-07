#include "lora_channelizer.hpp"

#include <kissfft/kiss_fft.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace rfmon::lora_channelizer {
namespace {

// Output transform size per block. N = M*D input samples per block; the first
// and last N/16 of each block are overlap (discarded), so S = 7N/8 input
// samples, 7M/8 output samples, are kept per block. The flat-top mask's
// impulse response is ~50 output samples, well inside the 64-sample overlap.
// M divisible by 16 keeps every boundary on a multiple of D.
constexpr size_t M = 1024;

struct Fft {
    kiss_fft_cfg cfg = nullptr;
    explicit Fft(size_t n, bool inverse) : cfg(kiss_fft_alloc(int(n), inverse ? 1 : 0, nullptr, nullptr)) {
        if (!cfg) throw std::runtime_error("kiss_fft_alloc failed");
    }
    ~Fft() { kiss_fft_free(cfg); }
    Fft(const Fft&) = delete;
    Fft& operator=(const Fft&) = delete;
};

}  // namespace

std::vector<std::vector<std::complex<float>>> extract(const std::vector<std::complex<float>>& wide, double in_rate_hz,
                                                      const std::vector<double>& offsets_hz, double out_rate_hz,
                                                      unsigned threads) {
    const double ratio = in_rate_hz / out_rate_hz;
    if (!std::isfinite(ratio) || ratio < 2 || std::abs(ratio - std::round(ratio)) > 1e-9)
        throw std::invalid_argument("Channel rate must divide the capture rate by an integer >= 2");
    const size_t D = size_t(std::llround(ratio));
    const size_t N = M * D, S = 7 * N / 8, V2 = N / 16;  // block, kept span, half overlap
    const size_t keep = S / D, skip = V2 / D;            // output samples kept / skipped per block
    const double bin_hz = in_rate_hz / double(N);
    for (double off : offsets_hz)
        if (!std::isfinite(off) || std::abs(off) + out_rate_hz / 2 >= in_rate_hz / 2)
            throw std::invalid_argument("Channel outside the capture band");

    const size_t out_len = wide.size() / D;
    std::vector<std::vector<std::complex<float>>> out(offsets_hz.size(), std::vector<std::complex<float>>(out_len));
    if (out_len == 0 || offsets_hz.empty()) return out;

    // Flat-top mask over the M output bins (bin m <-> m*out_rate/M Hz, signed).
    std::vector<float> mask(M);
    for (size_t i = 0; i < M; ++i) {
        const long m = long(i) < long(M / 2) ? long(i) : long(i) - long(M);
        const double f = std::abs(double(m)) / double(M);  // fraction of out_rate
        mask[i] = f <= 0.48 ? 1.0f : f >= 0.5 ? 0.0f : float(0.5 * (1 + std::cos(M_PI * (f - 0.48) / 0.02)));
    }
    struct Channel { long k0; double residual_hz; };
    std::vector<Channel> ch;
    for (double off : offsets_hz) {
        const long k0 = std::lround(off / bin_hz);
        ch.push_back({k0, off - double(k0) * bin_hz});
    }

    const size_t blocks = (out_len + keep - 1) / keep;
    if (!threads) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = unsigned(std::min<size_t>(threads, blocks));
    const float scale = 1.0f / float(N);

    auto worker = [&](size_t first, size_t last) {
        Fft fwd(N, false), inv(M, true);
        std::vector<kiss_fft_cpx> in(N), spec(N), sel(M), res(M);
        for (size_t b = first; b < last; ++b) {
            // Block covers input [b*S - V2, b*S - V2 + N); zero outside the capture.
            const long start = long(b * S) - long(V2);
            for (size_t n = 0; n < N; ++n) {
                const long at = start + long(n);
                if (at >= 0 && size_t(at) < wide.size()) { in[n].r = wide[size_t(at)].real(); in[n].i = wide[size_t(at)].imag(); }
                else { in[n].r = 0; in[n].i = 0; }
            }
            kiss_fft(fwd.cfg, in.data(), spec.data());
            for (size_t c = 0; c < ch.size(); ++c) {
                for (size_t i = 0; i < M; ++i) {
                    const long m = long(i) < long(M / 2) ? long(i) : long(i) - long(M);
                    long k = (ch[c].k0 + m) % long(N);
                    if (k < 0) k += long(N);
                    sel[i].r = spec[size_t(k)].r * mask[i];
                    sel[i].i = spec[size_t(k)].i * mask[i];
                }
                kiss_fft(inv.cfg, sel.data(), res.data());
                // Block-local downconversion -> global: multiply by
                // exp(-j 2pi k0 start / N); then remove the sub-bin residual
                // at the output rate, referenced to global input time.
                const double block_phase = -2 * M_PI * std::fmod(double(ch[c].k0) * double(start), double(N)) / double(N);
                const size_t o0 = b * keep;
                // Rotation for output o0 exactly, then advanced by a constant
                // step per output (recurrence in double, re-anchored every
                // block): no per-sample cos/sin.
                const double ph0 = block_phase - 2 * M_PI * std::fmod(ch[c].residual_hz * double(o0 * D) / in_rate_hz, 1.0);
                const double dph = -2 * M_PI * ch[c].residual_hz * double(D) / in_rate_hz;
                std::complex<double> rot = std::polar(1.0, ph0);
                const std::complex<double> step = std::polar(1.0, dph);
                for (size_t p = 0; p < keep && o0 + p < out_len; ++p) {
                    const auto& v = res[skip + p];
                    out[c][o0 + p] = std::complex<float>(v.r * scale, v.i * scale) *
                                     std::complex<float>(float(rot.real()), float(rot.imag()));
                    rot *= step;
                }
            }
        }
    };
    std::vector<std::thread> pool;
    const size_t per = (blocks + threads - 1) / threads;
    for (unsigned t = 0; t < threads; ++t) {
        const size_t first = t * per, last = std::min(blocks, first + per);
        if (first < last) pool.emplace_back(worker, first, last);
    }
    for (auto& t : pool) t.join();
    return out;
}

}  // namespace rfmon::lora_channelizer
