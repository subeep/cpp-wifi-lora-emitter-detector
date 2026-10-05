#include "chirp_structure.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
constexpr double pi = 3.14159265358979323846;
struct Moment {
    std::complex<double> cross{};
    double a = 0, b = 0;
    void add(std::complex<double> p, double pa, double pb, double sign = 1) {
        cross += sign * p; a += sign * pa; b += sign * pb;
    }
};
double coherence(const Moment& m, double floor) {
    if (m.a <= floor || m.b <= floor) return 0;
    return std::clamp(std::norm(m.cross) / (m.a * m.b), 0.0, 1.0);
}
} // namespace

std::vector<ChirpMeasurement> measure_chirp_structure(
    const std::vector<std::complex<float>>& iq, double rate) {
    if (!std::isfinite(rate) || rate <= 0 || rate > 1e9 || iq.size() > 65536)
        throw std::invalid_argument("invalid chirp rate or sample budget");
    std::complex<double> mean{};
    for (auto z : iq) {
        if (!std::isfinite(z.real()) || !std::isfinite(z.imag()))
            throw std::invalid_argument("non-finite chirp sample");
        mean += std::complex<double>(z);
    }
    if (!iq.empty()) mean /= double(iq.size());
    std::vector<std::complex<double>> x(iq.size());
    double scale = 0;
    for (std::size_t i = 0; i < x.size(); ++i) { x[i] = std::complex<double>(iq[i]) - mean; scale = std::max(scale, std::abs(x[i])); }
    double energy = 0;
    if (scale > 0) for (auto& z : x) { z /= scale; energy += std::norm(z); }
    struct Hypothesis { const char* label; double slope, sweep; };
    std::vector<ChirpMeasurement> results;
    for (const auto& h : {Hypothesis{"up_9MHz_135.2kHz_per_us", 135.2e9, 9e6},
                         Hypothesis{"up_18MHz_270.2kHz_per_us", 270.2e9, 18e6},
                         Hypothesis{"down_9MHz_135.3kHz_per_us", -135.3e9, 9e6}}) {
        ChirpMeasurement r; r.label = h.label; r.slope_hz_per_second = h.slope; r.nominal_sweep_hz = h.sweep;
        r.lag_samples = r.gate_samples = std::size_t(std::llround(rate * 32e-6));
        const auto lag = r.lag_samples, gate = r.gate_samples;
        if (rate <= h.sweep || lag < 8) { r.status = "unsupported_sample_rate"; results.push_back(r); continue; }
        if (x.size() < lag + gate) { results.push_back(r); continue; }
        r.positions_examined = x.size() - lag - gate + 1;
        if (energy <= 0) { r.status = "no_variation"; results.push_back(r); continue; }
        const auto count = x.size() - lag;
        std::vector<std::complex<double>> products(count);
        // x[n+L] conj(x[n]) has +slope*L/Fs frequency. Cancel that
        // shift; a carrier offset changes only the constant phase.
        const double step_phase = -2 * pi * h.slope * lag / (rate * rate);
        const auto step = std::polar(1.0, step_phase);
        std::complex<double> phase{1, 0};
        for (std::size_t n = 0; n < count; ++n) {
            if (n % 1024 == 0) phase = std::polar(1.0, std::remainder(step_phase * n, 2 * pi));
            products[n] = x[n + lag] * std::conj(x[n]) * phase; phase *= step;
        }
        const double floor = energy / x.size() * gate * 1e-10;
        Moment m;
        double best = -1;
        for (std::size_t start = 0; start < r.positions_examined; ++start) {
            // Rebuild periodically to bound cancellation after strong bursts.
            if (start % 1024 == 0) {
                m = {};
                for (std::size_t j = start; j < start + gate; ++j)
                    m.add(products[j], std::norm(x[j]), std::norm(x[j + lag]));
            }
            if (m.a > floor && m.b > floor) {
                ++r.positions_eligible;
                const double score = coherence(m, floor);
                if (score > best) { best = score; r.peak_offset = start; r.peak_coherence_squared = score;
                    r.half_energy_balance = std::min(m.a, m.b) / std::max(m.a, m.b); }
            }
            if (start + 1 < r.positions_examined) {
                m.add(products[start], std::norm(x[start]), std::norm(x[start + lag]), -1);
                const auto end = start + gate;
                m.add(products[end], std::norm(x[end]), std::norm(x[end + lag]));
            }
        }
        if (best < 0) r.status = "no_eligible_energy";
        else {
            r.status = "measured";
            Moment first, second;
            for (std::size_t j = 0; j < gate; ++j) {
                const auto n = r.peak_offset + j;
                (j < gate / 2 ? first : second).add(products[n], std::norm(x[n]), std::norm(x[n + lag]));
            }
            r.first_half_coherence_squared = coherence(first, floor / 2);
            r.second_half_coherence_squared = coherence(second, floor / 2);
            // Separate shape diagnostic: differentiate complex phase and fit
            // instantaneous frequency against time over the selected span.
            // All adjacent pairs must be powered. Unwrapping cannot establish
            // absence of aliasing or resolve mixtures; expose branch changes.
            const auto span = lag + gate;
            std::vector<double> frequency;
            frequency.reserve(span - 1);
            double previous = 0, branch = 0;
            for (std::size_t j = 0; j + 1 < span; ++j) {
                const auto n = r.peak_offset + j;
                if (std::norm(x[n]) <= floor / gate || std::norm(x[n + 1]) <= floor / gate) continue;
                const double angle = std::arg(x[n + 1] * std::conj(x[n]));
                if (!frequency.empty()) {
                    const double jump = angle - previous;
                    if (jump > pi) { branch -= 2 * pi; ++r.phase_unwraps; }
                    else if (jump < -pi) { branch += 2 * pi; ++r.phase_unwraps; }
                }
                frequency.push_back((angle + branch) * rate / (2 * pi)); previous = angle;
            }
            r.frequency_pairs = frequency.size();
            if (frequency.size() != span - 1) r.frequency_status = "partial_phase_support";
            else {
                r.frequency_status = "measured";
                // Centered coordinates give a stable direct least-squares
                // measurement, with no learned parameters or classifier.
                double sum = 0; for (auto f : frequency) sum += f;
                const double mean_f = sum / frequency.size(), center = (frequency.size() - 1) / 2.0;
                r.frequency_mean_hz = mean_f;
                double cross = 0, time_power = 0;
                for (std::size_t j = 0; j < frequency.size(); ++j) {
                    const double t = (j - center) / rate;
                    cross += t * (frequency[j] - mean_f); time_power += t * t;
                }
                r.frequency_slope_hz_per_second = cross / time_power;
                double residual = 0;
                for (std::size_t j = 0; j < frequency.size(); ++j) {
                    const double error = frequency[j] - mean_f - r.frequency_slope_hz_per_second * ((j - center) / rate);
                    residual += error * error;
                }
                r.frequency_rmse_hz = std::sqrt(residual / frequency.size());
            }
        }
        results.push_back(r);
    }
    return results;
}
} // namespace rfmon::cyclo
