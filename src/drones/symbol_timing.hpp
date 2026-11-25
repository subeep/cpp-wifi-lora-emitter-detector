#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <memory>

namespace rfmon::symbol_timing {
inline constexpr char version[] = "symbol-timing-v2";  // v2: Lightbridge 10.9375 kHz check added
inline constexpr std::size_t block_size = 131072, max_blocks = 24, spike_chunk = 256;
inline constexpr float spike_cap = 20, flag_ratio = 4;
inline constexpr double group_threshold = .3;
struct Hypothesis { const char* name; double seconds; };
inline constexpr std::array hypotheses{
    Hypothesis{"wifi_312.5kHz", 3.2e-6}, Hypothesis{"wifi_ax_78.125kHz", 12.8e-6},
    Hypothesis{"lte_like_15kHz", 1./15000}, Hypothesis{"lte_like_30kHz", 1./30000},
    Hypothesis{"lte_like_7.5kHz", 1./7500}, Hypothesis{"measured_32.8kHz", 1./32812.5},
    // Lightbridge's useful symbol: 4096 points at 44.8 MSps = 91.43 us (lb_decode, the user's
    // Phantom 4 Pro). The 32.8 kHz lag above is a third of it and only catches part of the
    // signal; at the app's gain a close Phantom clips and that third-lag peak disappears, while
    // this one stays (X310, 20 Nov 2026: 83-100% of blocks at gain 31.5, 32.8 kHz 0-4%).
    Hypothesis{"lightbridge_10.9375kHz", 1./10937.5}};
// Indices into hypotheses for the groups below.
inline constexpr std::size_t lte_15k = 2, lte_30k = 3, lightbridge_third = 5, lightbridge = 6;
using Ratios = std::array<float, hypotheses.size()>;
using Fractions = std::array<double, hypotheses.size()>;
// Correlation at each hypothesis's useful-symbol lag, divided by the zero lag. Its phase is
// 2 pi x (baseband frequency of the subcarrier grid) / (subcarrier spacing), so it places the
// grid modulo one spacing. At a fractional lag the phase is interpolated between the two
// neighbouring samples (whose phase difference is the signal's own baseband frequency).
using Phases = std::array<std::complex<double>, hypotheses.size()>;
bool supported_rate(double rate);
std::size_t block_start(std::size_t total, std::size_t index, std::size_t count);
const char* group(const Fractions& fractions);

class Probe {
public:
    Probe();
    ~Probe();
    // Input must contain one full block. Returns false for nonfinite samples.
    bool measure(const std::complex<float>* samples, double rate, Ratios& ratios, Phases* phases = nullptr);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
