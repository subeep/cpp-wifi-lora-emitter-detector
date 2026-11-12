#include "symbol_timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <kiss_fft.h>
#include <new>
#include <vector>

namespace rfmon::symbol_timing {
namespace {
float median(float* values, std::size_t count) {
    auto* middle = values + count / 2;
    std::nth_element(values, middle, values + count);
    if (count % 2) return *middle;
    return (*middle + *std::max_element(values, middle)) * .5f;
}
}

bool supported_rate(double rate) { return rate >= 20e6 && rate <= 25e6; }

std::size_t block_start(std::size_t total, std::size_t index, std::size_t count) {
    return count <= 1 ? 0 : index * (total - block_size) / (count - 1);
}

const char* group(const Fractions& f) {
    if (f[5] >= group_threshold) return "32.8kHz";
    if (f[3] >= group_threshold) return f[2] >= group_threshold ? "15kHz+30kHz" : "30kHz";
    return f[2] >= group_threshold ? "15kHz" : nullptr;
}

struct Probe::State {
    std::vector<kiss_fft_cpx> input = std::vector<kiss_fft_cpx>(block_size);
    std::vector<kiss_fft_cpx> output = std::vector<kiss_fft_cpx>(block_size);
    std::vector<float> hann = std::vector<float>(block_size);
    std::unique_ptr<kiss_fft_state, decltype(&std::free)> forward{kiss_fft_alloc(block_size, 0, nullptr, nullptr), std::free};
    std::unique_ptr<kiss_fft_state, decltype(&std::free)> inverse{kiss_fft_alloc(block_size, 1, nullptr, nullptr), std::free};
    State() {
        if (!forward || !inverse) throw std::bad_alloc();
        for (std::size_t i = 0; i < block_size; ++i)
            hann[i] = static_cast<float>(.5 - .5 * std::cos(2 * std::acos(-1.) * i / (block_size - 1)));
    }
};

Probe::Probe() : state_(std::make_unique<State>()) {}
Probe::~Probe() = default;

bool Probe::measure(const std::complex<float>* samples, double rate, Ratios& ratios) {
    ratios.fill(0);
    if (!supported_rate(rate)) return false;
    auto& s = *state_;
    std::complex<double> sum{};
    for (std::size_t i = 0; i < block_size; ++i) {
        if (!std::isfinite(samples[i].real()) || !std::isfinite(samples[i].imag())) return false;
        sum += samples[i];
    }
    const auto mean = static_cast<std::complex<float>>(sum / double(block_size));
    for (std::size_t i = 0; i < block_size; ++i) {
        const auto x = (samples[i] - mean) * s.hann[i];
        s.input[i] = {x.real(), x.imag()};
    }
    kiss_fft(s.forward.get(), s.input.data(), s.output.data());
    std::array<float, spike_chunk> powers;
    for (std::size_t begin = 0; begin < block_size; begin += spike_chunk) {
        for (std::size_t i = 0; i < spike_chunk; ++i) {
            const auto x = s.output[begin + i];
            powers[i] = x.r*x.r + x.i*x.i;
        }
        auto sorted = powers;
        const float cap = spike_cap * median(sorted.data(), sorted.size());
        for (std::size_t i = 0; i < spike_chunk; ++i)
            s.input[begin + i] = {std::min(powers[i], cap), 0};
    }
    kiss_fft(s.inverse.get(), s.input.data(), s.output.data());
    const float zero = s.output[0].r;
    if (!std::isfinite(zero)) return false;
    if (zero <= 0) return true;
    const int max_lag = std::min<int>(block_size / 4, std::lround(rate * 150e-6));
    const auto profile = [&](int lag) {
        const auto x = s.output[lag];
        return std::hypot(x.r, x.i) / zero;
    };
    for (std::size_t i = 0; i < hypotheses.size(); ++i) {
        const double lag = rate * hypotheses[i].seconds;
        if (lag < 58 || lag > max_lag - 60) continue;
        const bool fractional = std::abs(lag - std::round(lag)) > 1e-6;
        const int first = fractional ? std::floor(lag) : std::round(lag);
        const int last = fractional ? first + 1 : first;
        std::array<float, 94> neighbors;
        std::size_t count = 0;
        for (int k = first - 50; k <= last + 50; ++k)
            if (k < first - 3 || k > last + 3) neighbors[count++] = profile(k);
        const float base = median(neighbors.data(), count);
        if (base > 0) ratios[i] = std::max(profile(first), profile(last)) / base;
    }
    return true;
}
}
