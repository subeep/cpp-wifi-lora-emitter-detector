#include "cyclostationary/roi_measurements.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace rfmon::cyclo;
namespace {
constexpr double pi = 3.14159265358979323846, rate = 20e6;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) { try { f(); } catch (const std::exception&) { return; } throw std::runtime_error("bad ROI input accepted"); }
std::vector<std::complex<float>> noise(std::size_t n, float scale = 0.001f) {
    std::mt19937 rng(27020); std::normal_distribution<float> normal;
    std::vector<std::complex<float>> x(n); for (auto& z : x) z = {scale * normal(rng), scale * normal(rng)}; return x;
}
void tone(std::vector<std::complex<float>>& x, std::size_t start, std::size_t length, int bin, double amplitude) {
    for (std::size_t n = start; n < start + length; ++n)
        x[n] += std::complex<float>(std::polar(amplitude, 2 * pi * bin * (n % 512) / 512));
}
bool contains(const EnergyRegion& r, double frequency) {
    return std::any_of(r.bands.begin(), r.bands.end(), [&](const auto& b) { return b.low_hz <= frequency && b.high_hz >= frequency; });
}
}
int main() {
    try {
        auto x = noise(65536);
        tone(x, 5120, 4096, 64, 1); tone(x, 20480, 8192, -96, 0.7);
        const auto original = x; const auto r = measure_rois(x, rate);
        check(r.status == "contrast_regions" && r.samples_examined == x.size() && r.regions_seen == 2 && r.regions.size() == 2,
              "two separated bursts not resolved in block envelope");
        check(r.regions[0].offset == 5120 && r.regions[0].samples == 4096 &&
              r.regions[1].offset == 20480 && r.regions[1].samples == 8192 && r.contrast_samples == 12288,
              "time ranges differ from independent injected bounds");
        check(contains(r.regions[0], 2.5e6) && contains(r.regions[1], -3.75e6), "frequency sign or sample-rate mapping is wrong");
        check(r.regions[0].bands.size() == 1 && r.regions[1].bands.size() == 1 &&
              !r.regions[0].bands[0].edge_bin && !r.regions[0].bands[0].contains_dc,
              "isolated tones produced incorrect interval flags");
        const double expected = 4096.0 / (4096 + 8192 * 0.49);
        check(std::abs(r.regions[0].energy_fraction - expected) < 0.001, "region power disagrees with independent tone-energy oracle");
        check(r.regions[0].spectral_frames == 8 && r.regions[1].spectral_frames == 16 &&
              r.regions[0].spectral_samples == 4096, "PSD sample coverage is wrong");
        check(x == original, "ROI measurements modified original IQ");
        auto shifted = noise(65536); tone(shifted, 5197, 4069, 64, 1);
        const auto unaligned = measure_rois(shifted, rate);
        const auto& estimated = unaligned.regions[0];
        check(estimated.offset <= 5197 && 5197 - estimated.offset < 128 &&
              estimated.offset + estimated.samples >= 9266 && estimated.offset + estimated.samples - 9266 < 128,
              "unaligned burst bounds exceed declared block resolution");
        auto tail = noise(8197); tone(tail, 7000, 1197, 64, 1);
        const auto tail_result = measure_rois(tail, rate);
        check(tail_result.regions[0].offset + tail_result.regions[0].samples == tail.size() &&
              tail_result.regions[0].touches_window_edge && tail_result.regions[0].spectral_status == "few_frames",
              "partial final block or edge/short-spectrum qualification lost");
        for (float scale : {1e25f, 1e-25f}) {
            auto changed = x; for (auto& z : changed) z *= scale;
            const auto scaled = measure_rois(changed, rate);
            check(scaled.regions_seen == 2 && scaled.regions[0].offset == r.regions[0].offset &&
                  std::abs(scaled.regions[0].energy_fraction - r.regions[0].energy_fraction) < 1e-6 &&
                  contains(scaled.regions[1], -3.75e6), "ROI scale invariance failed");
        }
        auto dc = x; for (auto& z : dc) z += std::complex<float>(10, -7);
        const auto dc_result = measure_rois(dc, rate);
        check(dc_result.dc_fraction > 0.99 && dc_result.regions[0].offset == 5120 &&
              contains(dc_result.regions[0], 2.5e6), "disclosed global DC removal failed");
        auto stationary = noise(65536, 1);
        const auto null = measure_rois(stationary, rate);
        check(null.status == "uniform_energy" && null.regions.size() == 1 && !null.regions[0].contrast_selected &&
              null.regions[0].samples == stationary.size() && null.regions[0].spectral_frames == 32,
              "stationary noise must retain bounded context rather than signal-absence verdict");
        auto continuous = noise(65536);
        tone(continuous, 0, continuous.size(), 64, 1); tone(continuous, 0, continuous.size(), -96, 0.5);
        const auto mixture = measure_rois(continuous, rate);
        check(mixture.status == "uniform_energy" && mixture.regions.size() == 1 && mixture.regions[0].bands.size() == 2 &&
              contains(mixture.regions[0], 2.5e6) && contains(mixture.regions[0], -3.75e6),
              "continuous two-tone mixture lost context or merged separate spectral intervals");
        // This is exactly FFT-grid Nyquist; keep the interval marked partial.
        auto edge = noise(8192); tone(edge, 0, edge.size(), -256, 1);
        const auto edge_result = measure_rois(edge, rate);
        check(edge_result.regions[0].bands[0].edge_bin, "Nyquist-edge interval not flagged");
        auto tiny = noise(8192); tone(tiny, 2048, 128, 64, 1);
        const auto short_region = measure_rois(tiny, rate);
        check(short_region.regions_seen == 1 && short_region.regions[0].spectral_status == "insufficient_samples" &&
              short_region.regions[0].spectral_frames == 0, "sub-FFT burst should not fabricate spectrum");
        auto many = noise(65536);
        for (std::size_t i = 0; i < 12; ++i) tone(many, 2048 + i * 4096, 1024, 64, i + 1);
        const auto bounded = measure_rois(many, rate);
        check(bounded.regions_seen == 12 && bounded.regions.size() == 8 && bounded.regions[0].offset == 2048 + 4 * 4096,
              "region limit must retain strongest eight and report omissions");
        auto bands = noise(65536);
        const int frequencies[] = {-128, -64, 32, 96, 160};
        for (int i = 0; i < 5; ++i) tone(bands, 0, bands.size(), frequencies[i], 0.2 * (i + 1));
        const auto frequency_cap = measure_rois(bands, rate);
        check(frequency_cap.regions[0].bands_seen == 5 && frequency_cap.regions[0].bands.size() == 3,
              "frequency interval limit must report omitted intervals");
        for (auto value : {std::complex<float>(0, 0), std::complex<float>(2, -3)}) {
            const auto constant = measure_rois(std::vector<std::complex<float>>(2048, value), rate);
            check(constant.status == "no_variation" && constant.regions.empty(), "constant IQ must abstain");
        }
        check(measure_rois(std::vector<std::complex<float>>(127), rate).status == "insufficient_samples", "short tile did not abstain");
        rejects([&] { measure_rois(std::vector<std::complex<float>>(65537), rate); });
        rejects([&] { measure_rois(x, 0); }); rejects([&] { measure_rois(x, NAN); });
        x.back() = {INFINITY, 0}; rejects([&] { measure_rois(x, rate); });
        std::cout << "ROI time/frequency/mixture/scale/DC/coverage/cap/input checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
