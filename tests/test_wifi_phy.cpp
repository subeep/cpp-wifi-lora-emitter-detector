// Synthetic correctness tests for the Wi-Fi DSSS/OFDM correlators
// (src/wifi_phy.cpp). Pure software, no hardware - mirrors
// test_lora_phy_std.cpp's style.
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "wifi_phy.hpp"

using rfmon::wifi::classify_modulation;
using rfmon::wifi::ModClass;

namespace {

int failures = 0;

void check(bool cond, const std::string& name, const std::string& detail) {
    if (cond) {
        std::printf("PASS [%s]: %s\n", name.c_str(), detail.c_str());
    } else {
        std::printf("FAIL [%s]: %s\n", name.c_str(), detail.c_str());
        ++failures;
    }
}

constexpr float BARKER_CHIPS[11] = {+1, -1, +1, +1, -1, +1, +1, +1, -1, -1, -1};

// A synthetic 802.11b-style DSSS/CCK preamble: `n_bits` DBPSK bits (each
// +-1, matched-filter magnitude doesn't care about the sign so a fixed
// alternating pattern is as good as real scrambled data for this test),
// each spread by the 11-chip Barker sequence at 11 Mchip/s, rendered at
// `sample_rate_hz` via the same zero-order-hold the correlator itself
// assumes, plus a trailing stretch of unrelated noise (a real preamble
// is followed by a different-structured header/payload, not silence).
std::vector<std::complex<float>> synth_dsss(double sample_rate_hz, int n_bits, float noise_amp,
                                             unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, noise_amp);
    int symbol_len = int(std::lround(sample_rate_hz / 1e6));
    std::vector<std::complex<float>> out;
    out.reserve(size_t(n_bits) * size_t(symbol_len) + size_t(symbol_len) * 20);
    for (int b = 0; b < n_bits; ++b) {
        float bit_sign = (b % 2 == 0) ? 1.0f : -1.0f;  // alternating DBPSK bits
        for (int i = 0; i < symbol_len; ++i) {
            int chip = std::min(10, int((double(i) / symbol_len) * 11.0));
            float val = bit_sign * BARKER_CHIPS[chip];
            out.emplace_back(val + noise(rng), noise(rng));
        }
    }
    for (int i = 0; i < symbol_len * 20; ++i) out.emplace_back(noise(rng), noise(rng));
    return out;
}

// A synthetic OFDM-style preamble: one random short-training-symbol
// block of length L repeated `n_repeats` times back to back (exactly
// what Schmidl-Cox's delayed-conjugate autocorrelation is built to
// find), followed by unrelated noise standing in for the rest of the
// packet, which does not keep repeating that way.
std::vector<std::complex<float>> synth_ofdm(double sample_rate_hz, int n_repeats, float noise_amp,
                                             unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    std::normal_distribution<float> extra_noise(0.0f, noise_amp);
    int L = int(std::lround(0.8e-6 * sample_rate_hz));
    size_t l_sz = size_t(L);
    std::vector<std::complex<float>> block(l_sz);
    for (auto& s : block) s = std::complex<float>(noise(rng), noise(rng));

    std::vector<std::complex<float>> out;
    out.reserve(size_t(n_repeats) * size_t(L) + size_t(L) * 20);
    for (int r = 0; r < n_repeats; ++r) {
        for (auto s : block) out.push_back(s + std::complex<float>(extra_noise(rng), extra_noise(rng)));
    }
    for (int i = 0; i < L * 20; ++i) out.emplace_back(noise(rng), noise(rng));
    return out;
}

std::vector<std::complex<float>> synth_noise(size_t n, float amp, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, amp);
    std::vector<std::complex<float>> out(n);
    for (auto& s : out) s = std::complex<float>(noise(rng), noise(rng));
    return out;
}

// Shift a baseband-synthesized test signal up by `offset_hz` so the
// end-to-end classify_modulation() call (which mixes back down given
// that same offset) is exercised the same way scanner.cpp uses it -
// candidates rarely sit exactly at the capture's tuned center.
std::vector<std::complex<float>> shift_up(const std::vector<std::complex<float>>& iq,
                                            double sample_rate_hz, double offset_hz) {
    std::vector<std::complex<float>> out(iq.size());
    double phase = 0.0;
    double inc = 2.0 * M_PI * offset_hz / sample_rate_hz;
    for (size_t n = 0; n < iq.size(); ++n) {
        std::complex<float> rot(float(std::cos(phase)), float(std::sin(phase)));
        out[n] = iq[n] * rot;
        phase += inc;
        if (phase > M_PI) phase -= 2 * M_PI;
    }
    return out;
}

}  // namespace

int main() {
    // 1. DSSS at a generous oversampling rate.
    {
        auto iq = synth_dsss(88e6, 40, 0.05f, 1);
        auto r = classify_modulation(iq, 88e6, 2440e6, 2440e6, /*try_dsss=*/true);
        check(r.mod == ModClass::DSSS, "dsss_basic",
              "expected DSSS, got " + std::to_string(int(r.mod)) +
                  " conf=" + std::to_string(r.confidence));
    }

    // 2. DSSS at our real WIFI_2G4_SAMPLE_RATE_HZ (56 Msps).
    {
        auto iq = synth_dsss(56e6, 40, 0.05f, 2);
        auto r = classify_modulation(iq, 56e6, 2434.5e6, 2434.5e6, /*try_dsss=*/true);
        check(r.mod == ModClass::DSSS, "dsss_realistic_rate",
              "expected DSSS, got " + std::to_string(int(r.mod)) +
                  " conf=" + std::to_string(r.confidence));
    }

    // 3. OFDM at a generous oversampling rate.
    {
        auto iq = synth_ofdm(88e6, 12, 0.05f, 3);
        auto r = classify_modulation(iq, 88e6, 2440e6, 2440e6, /*try_dsss=*/true);
        check(r.mod == ModClass::OFDM, "ofdm_basic",
              "expected OFDM, got " + std::to_string(int(r.mod)) +
                  " conf=" + std::to_string(r.confidence));
    }

    // 4. OFDM at our real WIFI_2G4_SAMPLE_RATE_HZ (56 Msps).
    {
        auto iq = synth_ofdm(56e6, 12, 0.05f, 4);
        auto r = classify_modulation(iq, 56e6, 2434.5e6, 2434.5e6, /*try_dsss=*/true);
        check(r.mod == ModClass::OFDM, "ofdm_realistic_rate",
              "expected OFDM, got " + std::to_string(int(r.mod)) +
                  " conf=" + std::to_string(r.confidence));
    }

    // 4b. DSSS/OFDM at the X310's actual capped rate in this project
    // (DeviceProfile::max_sample_rate_hz = 20 Msps here, well under the
    // 56 Msps WIFI_2G4_SAMPLE_RATE_HZ this was otherwise tuned
    // against) - only ~1.8 samples/chip for Barker, which is why
    // classify_modulation() skips its own decimation step below a
    // 2-samples/chip floor rather than applying it unconditionally.
    {
        auto iq = synth_dsss(20e6, 40, 0.05f, 8);
        auto r = classify_modulation(iq, 20e6, 2434.5e6, 2434.5e6, /*try_dsss=*/true);
        check(r.mod == ModClass::DSSS, "dsss_x310_capped_rate",
              "expected DSSS at 20Msps, got " + std::to_string(int(r.mod)) +
                  " conf=" + std::to_string(r.confidence));
    }
    {
        auto iq = synth_ofdm(20e6, 12, 0.05f, 9);
        auto r = classify_modulation(iq, 20e6, 2434.5e6, 2434.5e6, /*try_dsss=*/true);
        check(r.mod == ModClass::OFDM, "ofdm_x310_capped_rate",
              "expected OFDM at 20Msps, got " + std::to_string(int(r.mod)) +
                  " conf=" + std::to_string(r.confidence));
    }

    // 5. Pure noise - neither correlator should fire.
    {
        auto iq = synth_noise(size_t(56e6 * 0.05), 1.0f, 5);
        auto r = classify_modulation(iq, 56e6, 2434.5e6, 2434.5e6, /*try_dsss=*/true);
        check(r.mod == ModClass::Unknown, "pure_noise",
              "expected Unknown, got " + std::to_string(int(r.mod)));
    }

    // 6. DSSS signal actually sitting away from the capture's tuned
    // center - exercises classify_modulation()'s own mix-to-baseband
    // step, not a pre-baseband signal handed in for free.
    {
        auto baseband = synth_dsss(56e6, 40, 0.05f, 6);
        auto shifted = shift_up(baseband, 56e6, 15e6);
        auto r = classify_modulation(shifted, 56e6, 2434.5e6, 2434.5e6 + 15e6,
                                       /*try_dsss=*/true);
        check(r.mod == ModClass::DSSS, "dsss_with_frequency_offset",
              "expected DSSS after de-mixing +15MHz offset, got " + std::to_string(int(r.mod)) +
                  " conf=" + std::to_string(r.confidence));
    }

    // 7. try_dsss=false (5GHz path) must never report DSSS, even against
    // a genuine DSSS waveform - Barker/CCK doesn't exist in that band.
    {
        auto iq = synth_dsss(56e6, 40, 0.05f, 7);
        auto r = classify_modulation(iq, 56e6, 5240e6, 5240e6, /*try_dsss=*/false);
        check(r.mod != ModClass::DSSS, "try_dsss_false_never_reports_dsss",
              "expected not-DSSS with try_dsss=false, got " + std::to_string(int(r.mod)));
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED.\n", failures);
        return 1;
    }
    std::printf("\nAll Wi-Fi modulation classifier checks passed.\n");
    return 0;
}
