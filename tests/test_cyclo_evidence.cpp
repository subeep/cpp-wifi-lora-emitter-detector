#include "cyclostationary/link_evidence.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace rfmon::cyclo;
namespace {
constexpr double pi = 3.14159265358979323846, rate = 20e6;
void check(bool yes, const char* message) { if (!yes) throw std::runtime_error(message); }
template<class F> void rejects(F f) { try { f(); } catch (const std::exception&) { return; } throw std::runtime_error("invalid evidence input accepted"); }
LinkEvidence assess(const std::vector<std::complex<float>>& x, EvidenceContext c) {
    SpectralConfig config; config.sample_rate_hz = rate; config.max_frames = 256;
    return assess_link_evidence(measure_spectral_correlation(x, config),
        measure_ofdm_structure(x, rate, wlan_ofdm_hypotheses(rate)), measure_chirp_structure(x, rate), c);
}
const WaveformEvidence& up(const LinkEvidence& e) {
    for (const auto& m : e.candidates) if (m.kind == "linear_chirp_reference") return m;
    throw std::runtime_error("missing chirp evidence");
}
}
int main() {
    try {
        std::mt19937 rng(825314); std::normal_distribution<float> normal;
        std::vector<std::complex<float>> x(8192);
        for (auto& z : x) z = {normal(rng), normal(rng)};
        check(!up(assess(x, {rate, 16e6, -1})).pattern_consistent, "noise became a reference chirp");
        for (std::size_t n = 0; n < x.size(); ++n) x[n] = std::complex<float>(std::polar(1.0, 2*pi*2e6*n/rate));
        check(!assess(x, {rate, 16e6, -1}).observed_quality_passed, "tone domination was not rejected");
        // Analytic +2 MHz up-chirp: 9 MHz sweep fits 16 MHz, crosses 10 MHz.
        for (std::size_t n = 0; n < x.size(); ++n) x[n] = {.001f*normal(rng), .001f*normal(rng)};
        for (std::size_t n = 0; n < 1334; ++n) {
            const double t = n / rate;
            x[2000+n] += std::complex<float>(std::polar(1.0, 2*pi*((2e6-4.5e6)*t + 135.2e9*t*t/2)));
        }
        auto unknown = assess(x, {rate, 0, -1});
        check(up(unknown).pattern_consistent, "analytic chirp not matched");
        check(up(unknown).status == "passband_unverified", "Fs substituted for unknown passband");
        check(!unknown.quality_checks.back().known, "float amplitude pretended to reveal ADC rails");
        check(up(assess(x, {rate, 16e6, 0})).status == "experimental_match", "supported declared chirp extent rejected");
        check(up(assess(x, {rate, 10e6, 0})).status == "passband_rejected", "out-of-band chirp was qualified");
        check(up(assess(x, {rate, 16e6, .02})).status == "quality_rejected", "source rails ignored");
        for (auto& z : x) z += std::complex<float>(1, 2);
        check(up(assess(x, {rate, 16e6, -1})).status == "quality_rejected", "large DC artifact ignored");
        SpectralFeatures s; s.variance_power = NAN;
        rejects([&] { assess_link_evidence(s, {}, {}, {rate, 0, -1}); });
        rejects([&] { assess(x, {rate, rate+1, -1}); });
        rejects([&] { assess(x, {rate, 0, -.5}); });
        rejects([&] { assess_link_evidence({}, std::vector<OfdmMeasurement>(33), {}, {rate, 0, -1}); });
        std::cout << "evidence analytic chirp, confusables, DC/rails, passband abstention and bounds passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
