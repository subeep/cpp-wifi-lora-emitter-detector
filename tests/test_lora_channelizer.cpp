// FFT channelizer (src/lora_channelizer.cpp) against the reference
// derive_channel() and analytic tones; also times a 6 s, 20 Msps capture.
#include "channelize.hpp"
#include "lora_channelizer.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

using namespace rfmon;
namespace {
int failures = 0;
void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s [%s] %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    failures += !ok;
}
std::vector<std::complex<float>> tones(size_t n, double rate, const std::vector<double>& f, float noise) {
    std::mt19937 g(7);
    std::normal_distribution<float> nd(0, noise);
    std::vector<std::complex<float>> x(n);
    for (size_t i = 0; i < n; ++i) {
        std::complex<double> s;
        for (double fi : f) s += std::polar(1.0, 2 * M_PI * std::fmod(fi * double(i) / rate, 1.0));
        x[i] = std::complex<float>(float(s.real()) + nd(g), float(s.imag()) + nd(g));
    }
    return x;
}
}  // namespace

int main() {
    {  // Matches derive_channel() on tones + noise (4 Msps, 4 channels, off-bin offsets).
        const double rate = 4e6;
        const std::vector<double> offs = {-1.2695e6, -0.9295e6, -0.347e6, 0.568e6};
        auto x = tones(size_t(rate * 0.5), rate, {-1.2695e6 + 31e3, 0.568e6 - 117e3, 1.5e6}, 0.05f);
        auto got = lora_channelizer::extract(x, rate, offs, 500e3);
        bool ok = got.size() == offs.size();
        double worst = 1;
        for (size_t c = 0; c < offs.size() && ok; ++c) {
            auto ref = derive_channel(x, rate, offs[c], 500e3);
            const size_t n = std::min(ref.size(), got[c].size());
            ok = ok && n + 2 >= ref.size() && got[c].size() == x.size() / 8;
            // Compare away from the edges (filters differ in their start-up).
            std::complex<double> dot;
            double ea = 0, eb = 0;
            for (size_t i = 2000; i + 2000 < n; ++i) {
                dot += std::complex<double>(got[c][i]) * std::conj(std::complex<double>(ref[i]));
                ea += std::norm(got[c][i]);
                eb += std::norm(ref[i]);
            }
            const double corr = std::abs(dot) / std::sqrt(ea * eb);
            const double gain = std::sqrt(ea / eb);
            std::printf("  channel %zu: correlation %.6f gain %.4f phase %.4f\n", c, corr, gain, std::arg(dot));
            // Channels 0 and 3 carry a tone; 1 and 2 are noise only, where the
            // two filters' slightly different transition bands dominate.
            const bool tone = c == 0 || c == 3;
            worst = std::min(worst, tone ? corr : 1.0);
            ok = ok && corr > (tone ? 0.9995 : 0.98) && std::abs(gain - 1) < (tone ? 0.01 : 0.05) &&
                 std::abs(std::arg(dot)) < 0.02;
        }
        check(ok, "matches_derive_channel", "worst correlation " + std::to_string(worst));
    }
    {  // Tone lands at the right baseband frequency with unit gain and continuous phase.
        const double rate = 20e6, off = 3.21e6, tone = off + 41.3e3;
        auto x = tones(size_t(rate * 0.2), rate, {tone, -5e6}, 0.0f);
        auto y = lora_channelizer::extract(x, rate, {off}, 500e3)[0];
        double max_err = 0, max_amp_err = 0;
        for (size_t j = 4000; j + 4000 < y.size(); ++j) {
            const std::complex<double> want = std::polar(1.0, 2 * M_PI * std::fmod(tone * double(j * 40) / rate, 1.0)) *
                                              std::polar(1.0, -2 * M_PI * std::fmod(off * double(j * 40) / rate, 1.0));
            max_err = std::max(max_err, std::abs(std::complex<double>(y[j]) - want));
            max_amp_err = std::max(max_amp_err, std::abs(std::abs(double(std::abs(y[j]))) - 1.0));
        }
        check(max_err < 0.01, "tone_phase_continuous_across_blocks", std::to_string(max_err));
        check(max_amp_err < 0.005, "unit_gain", std::to_string(max_amp_err));
    }
    {  // Out-of-channel energy is rejected (signal 300 kHz outside a BW-500 channel edge).
        const double rate = 20e6;
        auto x = tones(size_t(rate * 0.1), rate, {1e6 + 550e3}, 0.0f);
        auto y = lora_channelizer::extract(x, rate, {1e6}, 500e3)[0];
        double p = 0;
        for (size_t j = 2000; j + 2000 < y.size(); ++j) p += std::norm(y[j]);
        p /= double(y.size() - 4000);
        check(10 * std::log10(p + 1e-30) < -60, "stopband_rejection", std::to_string(10 * std::log10(p + 1e-30)) + " dB");
    }
    {  // Invalid input.
        bool threw = false;
        try { lora_channelizer::extract(std::vector<std::complex<float>>(1000), 4e6, {1.9e6}, 500e3); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "rejects_channel_outside_band");
    }
    {  // Timing: 6 s at 20 Msps, 4 channels.
        std::vector<std::complex<float>> x(size_t(20e6 * 6), std::complex<float>(0.01f, -0.01f));
        const auto t0 = std::chrono::steady_clock::now();
        auto y = lora_channelizer::extract(x, 20e6, {-1.27e6, -0.93e6, -0.35e6, 0.57e6}, 500e3);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(y.size() == 4 && y[0].size() == 3000000, "full_capture_size");
        std::printf("INFO 6 s x 20 Msps, 4 channels: %.2f s\n", s);
    }
    if (failures) { std::printf("%d channelizer check(s) failed\n", failures); return 1; }
    std::printf("All LoRa channelizer checks passed\n");
}
