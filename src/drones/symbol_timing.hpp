#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <memory>

namespace rfmon::symbol_timing {
inline constexpr char version[] = "symbol-timing-v1";
inline constexpr std::size_t block_size = 131072, max_blocks = 24, spike_chunk = 256;
inline constexpr float spike_cap = 20, flag_ratio = 4;
inline constexpr double group_threshold = .3;
struct Hypothesis { const char* name; double seconds; };
inline constexpr std::array hypotheses{
    Hypothesis{"wifi_312.5kHz", 3.2e-6}, Hypothesis{"wifi_ax_78.125kHz", 12.8e-6},
    Hypothesis{"lte_like_15kHz", 1./15000}, Hypothesis{"lte_like_30kHz", 1./30000},
    Hypothesis{"lte_like_7.5kHz", 1./7500}, Hypothesis{"measured_32.8kHz", 1./32812.5}};
using Ratios = std::array<float, hypotheses.size()>;
using Fractions = std::array<double, hypotheses.size()>;
bool supported_rate(double rate);
std::size_t block_start(std::size_t total, std::size_t index, std::size_t count);
const char* group(const Fractions& fractions);

class Probe {
public:
    Probe();
    ~Probe();
    // Input must contain one full block. Returns false for nonfinite samples.
    bool measure(const std::complex<float>* samples, double rate, Ratios& ratios);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
