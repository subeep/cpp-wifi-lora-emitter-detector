#include "ofdm_structure.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
constexpr double pi = 3.14159265358979323846;
struct Moment {
    std::complex<double> cross{};
    double a = 0, b = 0;
    Moment& operator+=(const Moment& m) { cross += m.cross; a += m.a; b += m.b; return *this; }
    Moment& operator-=(const Moment& m) { cross -= m.cross; a -= m.a; b -= m.b; return *this; }
};
double coherence(const Moment& m) {
    if (m.a <= 0 || m.b <= 0) return 0;
    return std::clamp(std::norm(m.cross) / (m.a * m.b), 0.0, 1.0);
}
std::vector<Moment> fold(const std::vector<std::complex<double>>& x, std::size_t lag,
                         std::size_t period, std::size_t first_symbol, std::size_t symbols) {
    std::vector<Moment> moments(period);
    for (std::size_t n = first_symbol * period; n < (first_symbol + symbols) * period; ++n) {
        const auto a = x[n + lag], b = x[n];
        auto& m = moments[n % period];
        m.cross += a * std::conj(b); m.a += std::norm(a); m.b += std::norm(b);
    }
    return moments;
}
Moment total(const std::vector<Moment>& m) {
    Moment sum;
    for (const auto& v : m) sum += v;
    return sum;
}
Moment gate(const std::vector<Moment>& m, std::size_t phase, std::size_t length) {
    Moment sum;
    for (std::size_t j = 0; j < length; ++j) sum += m[(phase + j) % m.size()];
    return sum;
}
void validate_rate(double rate) {
    if (!std::isfinite(rate) || rate <= 0 || rate > 1e9)
        throw std::invalid_argument("OFDM sample rate must be in (0, 1e9]");
}
} // namespace

std::vector<OfdmHypothesis> wlan_ofdm_hypotheses(double rate) {
    validate_rate(rate);
    std::vector<OfdmHypothesis> out;
    struct Timing { const char* label; double useful_us, prefix_us; };
    // NI: Introduction to 802.11ax High-Efficiency Wireless, OFDM timing table.
    for (const auto& t : {Timing{"3.2us+0.8us", 3.2, 0.8}, {"3.2us+0.4us", 3.2, 0.4},
                          {"12.8us+0.8us", 12.8, 0.8}, {"12.8us+1.6us", 12.8, 1.6},
                          {"12.8us+3.2us", 12.8, 3.2}}) {
        const double u = rate * t.useful_us * 1e-6, p = rate * t.prefix_us * 1e-6;
        if (u >= 2 && p >= 1 && u <= 8192 && u + p <= 16384 &&
            std::abs(u - std::round(u)) < 1e-6 && std::abs(p - std::round(p)) < 1e-6)
            out.push_back({t.label, std::size_t(std::llround(u)), std::size_t(std::llround(p))});
    }
    return out;
}

std::vector<OfdmMeasurement> measure_ofdm_structure(
    const std::vector<std::complex<float>>& iq, double rate,
    const std::vector<OfdmHypothesis>& hypotheses, std::size_t max_samples) {
    validate_rate(rate);
    if (!max_samples || max_samples > 262144 || hypotheses.size() > 32)
        throw std::invalid_argument("OFDM sample/hypothesis budget exceeded");
    for (const auto& h : hypotheses)
        if (h.label.size() > 96 || h.useful_samples < 2 || h.useful_samples > 8192 ||
            !h.prefix_samples || h.prefix_samples >= h.useful_samples ||
            h.prefix_samples + h.useful_samples > 16384)
            throw std::invalid_argument("invalid OFDM timing hypothesis");
    const auto count = std::min(iq.size(), max_samples);
    std::vector<std::complex<double>> x(count);
    std::complex<double> mean{};
    for (std::size_t i = 0; i < count; ++i) {
        x[i] = iq[i];
        if (!std::isfinite(x[i].real()) || !std::isfinite(x[i].imag()))
            throw std::invalid_argument("non-finite OFDM input sample");
        mean += x[i];
    }
    if (count) mean /= double(count);
    double power = 0;
    for (auto& z : x) { z -= mean; power += std::norm(z); }
    // Normalize before cross-products, including finite float inputs near FLT_MAX.
    if (power > 0) for (auto& z : x) z /= std::sqrt(power / count);
    std::vector<OfdmMeasurement> out;
    for (const auto& h : hypotheses) {
        OfdmMeasurement r; r.hypothesis = h;
        const auto period = h.useful_samples + h.prefix_samples;
        r.symbol_rate_hz = rate / period;
        const auto symbols = count > h.useful_samples ? (count - h.useful_samples) / period : 0;
        r.train_symbols = symbols / 2; r.holdout_symbols = symbols - r.train_symbols;
        r.samples_used = symbols ? symbols * period + h.useful_samples : 0;
        if (r.train_symbols < 8 || r.holdout_symbols < 8) { out.push_back(r); continue; }
        if (power <= 0) { r.status = "no_variation"; out.push_back(r); continue; }
        // Leave one entire symbol of lagged-pair start coordinates unused.
        // This keeps both endpoints of training pairs out of holdout pairs.
        --r.train_symbols;
        if (r.train_symbols < 8) { out.push_back(r); continue; }
        const auto train = fold(x, h.useful_samples, period, 0, r.train_symbols);
        const auto all_train = total(train);
        auto inside = gate(train, 0, h.prefix_samples);
        double best = -2;
        for (std::size_t phase = 0; phase < period; ++phase) {
            auto outside = all_train; outside -= inside;
            const double score = coherence(inside) - coherence(outside);
            if (score > best) { best = score; r.prefix_phase_samples = phase; }
            inside -= train[phase]; inside += train[(phase + h.prefix_samples) % period];
        }
        r.train_prefix_coherence_squared = coherence(gate(train, r.prefix_phase_samples, h.prefix_samples));
        const auto held = fold(x, h.useful_samples, period, r.train_symbols + 1, r.holdout_symbols);
        const auto all_held = total(held);
        inside = gate(held, r.prefix_phase_samples, h.prefix_samples);
        auto outside = all_held; outside -= inside;
        r.holdout_prefix_coherence_squared = coherence(inside);
        r.holdout_outside_coherence_squared = coherence(outside);
        r.holdout_contrast = r.holdout_prefix_coherence_squared - r.holdout_outside_coherence_squared;
        // Arbitrary alpha=Fs/(useful+CP), including values between FFT bins.
        // Sum absolute-time lag products: integer periods make n%period exact.
        auto cyclic = all_held; cyclic.cross = {};
        for (std::size_t phase = 0; phase < period; ++phase)
            cyclic.cross += held[phase].cross * std::polar(1.0, -2 * pi * phase / period);
        r.holdout_symbol_cyclic_coherence_squared = coherence(cyclic);
        r.status = "measured";
        out.push_back(r);
    }
    return out;
}
} // namespace rfmon::cyclo
