#include "cyclostationary/chirp_structure.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
using namespace rfmon::cyclo;
namespace {
constexpr double pi = 3.14159265358979323846, rate = 20e6;
void check(bool b, const char* s) { if (!b) throw std::runtime_error(s); }
template<class F> void rejects(F f) { try { f(); } catch (const std::exception&) { return; } throw std::runtime_error("bad chirp input accepted"); }
std::vector<std::complex<float>> noise(std::size_t n, double amplitude = 1) {
    std::mt19937 rng(63031); std::normal_distribution<double> normal;
    std::vector<std::complex<float>> x(n);
    for (auto& z : x) z = {float(amplitude * normal(rng)), float(amplitude * normal(rng))};
    return x;
}
// Independent direct equation, no rolling sums or oscillator recurrence.
double oracle(const std::vector<std::complex<float>>& iq, const ChirpMeasurement& h, std::size_t start) {
    std::complex<double> mean{}; for (auto z : iq) mean += std::complex<double>(z); mean /= double(iq.size());
    std::complex<double> dot{}; double a = 0, b = 0;
    for (std::size_t j = 0; j < h.gate_samples; ++j) {
        const auto n = start + j;
        const auto x = std::complex<double>(iq[n]) - mean, y = std::complex<double>(iq[n + h.lag_samples]) - mean;
        dot += y * std::conj(x) * std::polar(1.0, -2 * pi * h.slope_hz_per_second * h.lag_samples * n / (rate * rate));
        a += std::norm(x); b += std::norm(y);
    }
    return std::norm(dot) / (a * b);
}
}
int main() {
    try {
        auto random = noise(2000); const auto original = random;
        const auto measured = measure_chirp_structure(random, rate);
        double best = -1; std::size_t offset = 0;
        for (std::size_t n = 0; n < measured[0].positions_examined; ++n) {
            const auto v = oracle(random, measured[0], n); if (v > best) { best = v; offset = n; }
        }
        check(std::abs(best - measured[0].peak_coherence_squared) < 1e-10 && offset == measured[0].peak_offset,
              "rolling chirp maximum disagrees with independent direct oracle");
        check(random == original, "chirp analysis modified input IQ");
        for (std::size_t which = 0; which < measured.size(); ++which) {
            for (double carrier : {0.0, which == 1 ? 0.3e6 : 4e6}) {
                auto x = noise(4096, 0.001); const auto& h = measured[which];
                const std::size_t start = 811, samples = std::llround(rate * 66.7e-6);
                for (std::size_t n = 0; n < samples; ++n) {
                    const double t = n / rate, begin = -std::copysign(h.nominal_sweep_hz / 2, h.slope_hz_per_second);
                    x[start + n] += std::complex<float>(std::polar(1.0, 2 * pi * ((carrier + begin) * t + h.slope_hz_per_second * t * t / 2)));
                }
                const auto r = measure_chirp_structure(x, rate);
                check(r[which].peak_coherence_squared > 0.998 && r[which].first_half_coherence_squared > 0.998 &&
                      r[which].second_half_coherence_squared > 0.998 && r[which].half_energy_balance > 0.99,
                      "injected slope/sign/carrier-shift chirp not measured correctly");
                std::complex<double> mean{}; for (auto z : x) mean += std::complex<double>(z); mean /= double(x.size());
                double perturbation = 0;
                for (std::size_t n = 0; n < samples; ++n) {
                    const double t = n / rate, begin = -std::copysign(h.nominal_sweep_hz / 2, h.slope_hz_per_second);
                    const auto ideal = std::polar(1.0, 2 * pi * ((carrier + begin) * t + h.slope_hz_per_second * t * t / 2));
                    perturbation = std::max(perturbation, std::abs(std::complex<double>(x[start + n]) - mean - ideal));
                }
                // An amplitude perturbation e on a unit phasor changes phase
                // by at most asin(e). Differencing two phases bounds frequency
                // error by Fs*asin(e)/pi; the least-squares residual is no worse.
                const double residual_bound = rate * std::asin(perturbation) / pi;
                check(r[which].frequency_status == "measured" && r[which].phase_unwraps == 0 &&
                      std::abs(r[which].frequency_slope_hz_per_second / h.slope_hz_per_second - 1) < 0.001 &&
                      r[which].frequency_rmse_hz <= residual_bound + 1, "independent phase-increment slope/residual disagrees with injected chirp");
                check(r[which].peak_offset >= start && r[which].peak_offset + 2 * r[which].lag_samples <= start + samples,
                      "chirp maximum does not lie inside injected symbol");
                const double fit_midpoint = (double(r[which].peak_offset - start) +
                    double(r[which].frequency_pairs) / 2) / rate;
                const double expected_mean = carrier - std::copysign(h.nominal_sweep_hz / 2, h.slope_hz_per_second) +
                    h.slope_hz_per_second * fit_midpoint;
                check(std::abs(r[which].frequency_mean_hz - expected_mean) <= residual_bound + 1,
                      "frequency mean disagrees with analytic span center");
                const auto opposite = which == 2 ? 0 : 2;
                check(oracle(x, r[opposite], r[which].peak_offset) < 0.02, "opposite chirp direction confused at injected symbol");
                for (float scale : {1e-25f, 1e25f}) {
                    auto scaled = x; for (auto& z : scaled) z *= scale;
                    check(std::abs(measure_chirp_structure(scaled, rate)[which].peak_coherence_squared - r[which].peak_coherence_squared) < 1e-6,
                          "chirp amplitude scale invariance failed");
                }
            }
        }
        auto n = noise(65536); for (const auto& h : measure_chirp_structure(n, rate))
            check(h.peak_coherence_squared < 0.04, "seeded noise null has excessive chirp score");
        std::vector<std::complex<float>> tone(65536);
        for (std::size_t j = 0; j < tone.size(); ++j) tone[j] = std::complex<float>(std::polar(1.0, 2 * pi * 2e6 * j / rate));
        for (const auto& h : measure_chirp_structure(tone, rate)) check(h.peak_coherence_squared < 0.001, "tone became a chirp maximum");
        // A two-tone mixture has a real shifted-lag response; it must not be
        // mistaken for an independently qualified drone signature by callers.
        const double shift = measured[0].slope_hz_per_second * measured[0].lag_samples / rate;
        for (std::size_t j = 0; j < tone.size(); ++j)
            tone[j] = std::complex<float>(std::polar(1.0, -pi * shift * j / rate) + std::polar(1.0, pi * shift * j / rate));
        const auto confusable = measure_chirp_structure(tone, rate);
        check(confusable[0].peak_coherence_squared > 0.2 && confusable[0].peak_coherence_squared < 0.3,
              "two-tone confusable response not disclosed by measurement");
        for (const auto& h : measure_chirp_structure(std::vector<std::complex<float>>(2048, {2, 3}), rate))
            check(h.status == "no_variation" && h.positions_eligible == 0, "constant DC input not qualified");
        for (const auto& h : measure_chirp_structure(noise(1279), rate)) check(h.status == "insufficient_samples", "short window accepted");
        check(measure_chirp_structure(noise(1280), rate)[0].positions_examined == 1, "minimum full window lost");
        check(measure_chirp_structure(noise(65536), 10e6)[1].status == "unsupported_sample_rate", "18 MHz undersampling accepted");
        check(measure_chirp_structure(noise(65536), 200e6)[0].lag_samples == 6400, "native rate timing incorrect");
        rejects([&] { measure_chirp_structure(noise(65537), rate); });
        n[1] = {std::numeric_limits<float>::infinity(), 0}; rejects([&] { measure_chirp_structure(n, rate); });
        for (double invalid : {0.0, -1.0, 1e10, std::numeric_limits<double>::quiet_NaN()}) rejects([&] { measure_chirp_structure({}, invalid); });
        std::cout << "chirp oracle, three slopes, carrier shift, nulls, confusable and bounds passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
