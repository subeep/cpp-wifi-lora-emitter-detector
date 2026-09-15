// Adversarial stress matrix for the 2.4GHz Wi-Fi modulation classifier
// (src/wifi_phy.cpp). Where test_wifi_phy.cpp asks "does it work at
// all", this asks "where exactly does it break, and does it ever lie".
//
// Two things make this independent of the code under test, which the
// original suite was not:
//
//  1. The OFDM generator builds a SPEC-FAITHFUL L-STF - the real
//     802.11a short training field, 12 active subcarriers at +/-4, 8,
//     12, 16, 20, 24 on a 64-point IFFT, which is what produces its
//     16-sample (0.8us at 20 Msps) periodicity - rather than "a random
//     block repeated N times". The sparse comb is a real property of
//     the standard, and the classifier's spectral gate has to cope with
//     it rather than with a convenient stand-in.
//
//  2. The DSSS generator pulse-shapes at 20 samples/chip and then
//     decimates, instead of reusing the zero-order-hold chip formula
//     (min(10, int((i/template_len)*11))) that barker_evidence() uses
//     to build its own template. The old test shared that formula with
//     the correlator, so it structurally could not detect a
//     template/waveform mismatch - it was testing the code against
//     itself.
//
// Everything here runs at 20 Msps, the X310's real capped rate on this
// hardware (DeviceProfile::max_sample_rate_hz), because that is the
// regime the live system actually operates in - 1.818 samples/chip for
// Barker, and no excess bandwidth around a 20MHz channel.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <kissfft/kiss_fft.h>

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

constexpr double kRate = 20e6;  // the X310's real capped rate here
constexpr int kFft = 64;        // 802.11a: 64 subcarriers x 312.5kHz = 20MHz

std::vector<std::complex<double>> ifft64(const std::vector<std::complex<double>>& freq) {
    size_t n = size_t(kFft);
    std::vector<kiss_fft_cpx> in(n), out(n);
    for (size_t i = 0; i < n; ++i) {
        in[i].r = float(freq[i].real());
        in[i].i = float(freq[i].imag());
    }
    kiss_fft_cfg cfg = kiss_fft_alloc(kFft, /*inverse=*/1, nullptr, nullptr);
    kiss_fft(cfg, in.data(), out.data());
    kiss_fft_free(cfg);
    std::vector<std::complex<double>> t(n);
    for (size_t i = 0; i < n; ++i) t[i] = std::complex<double>(out[i].r, out[i].i) / double(kFft);
    return t;
}

// The real 802.11a L-STF frequency-domain sequence: 12 non-zero
// subcarriers, every 4th one, which is exactly why the time-domain
// result repeats every 64/4 = 16 samples.
std::vector<std::complex<double>> lstf_time_symbol() {
    std::vector<std::complex<double>> freq(size_t(kFft), {0.0, 0.0});
    const int sc[12] = {-24, -20, -16, -12, -8, -4, 4, 8, 12, 16, 20, 24};
    const int sign[12] = {+1, -1, +1, -1, -1, +1, -1, -1, +1, +1, +1, +1};
    const double scale = std::sqrt(13.0 / 6.0);
    for (int i = 0; i < 12; ++i) {
        int bin = (sc[i] + kFft) % kFft;
        freq[size_t(bin)] = scale * std::complex<double>(sign[i], sign[i]);
    }
    auto t = ifft64(freq);
    // One short symbol is the first 16 samples of that 64-sample block.
    return std::vector<std::complex<double>>(t.begin(), t.begin() + 16);
}

// A structurally faithful Non-HT burst: 10 short symbols of L-STF (the
// part Schmidl-Cox keys on), then content that is NOT periodic at 16
// samples so the plateau genuinely ends the way a real L-LTF makes it
// end, then idle.
std::vector<std::complex<float>> gen_ofdm_burst(double snr_db, unsigned seed, int idle_before,
                                                 int idle_after) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);

    auto shortsym = lstf_time_symbol();
    std::vector<std::complex<double>> sig;
    for (int r = 0; r < 10; ++r) {
        for (auto s : shortsym) sig.push_back(s);
    }
    // Stand-in for L-LTF + data: random QPSK across all 52 data
    // subcarriers, 6 symbols' worth. Its only required property is that
    // it does not repeat with period 16.
    for (int symb = 0; symb < 6; ++symb) {
        std::vector<std::complex<double>> freq(size_t(kFft), {0.0, 0.0});
        for (int k = -26; k <= 26; ++k) {
            if (k == 0) continue;
            int bin = (k + kFft) % kFft;
            double re = (rng() % 2) ? 1.0 : -1.0;
            double im = (rng() % 2) ? 1.0 : -1.0;
            freq[size_t(bin)] = std::complex<double>(re, im);
        }
        auto t = ifft64(freq);
        for (auto s : t) sig.push_back(s);
    }

    double sig_power = 0.0;
    for (auto s : sig) sig_power += std::norm(s);
    sig_power /= double(sig.size());
    double noise_sigma = std::sqrt(sig_power / std::pow(10.0, snr_db / 10.0) / 2.0);

    std::vector<std::complex<float>> out;
    out.reserve(size_t(idle_before) + sig.size() + size_t(idle_after));
    auto push_noise = [&](int count) {
        for (int i = 0; i < count; ++i) {
            out.emplace_back(float(g(rng) * noise_sigma), float(g(rng) * noise_sigma));
        }
    };
    push_noise(idle_before);
    for (auto s : sig) {
        out.emplace_back(float(s.real() + g(rng) * noise_sigma),
                          float(s.imag() + g(rng) * noise_sigma));
    }
    push_noise(idle_after);
    return out;
}

// Pulse-shaped 802.11b DSSS: Barker-11 at 11 Mchip/s, rendered at 20
// samples/chip through a raised-cosine pulse, then resampled to 20
// Msps. Deliberately NOT the zero-order-hold the correlator's template
// assumes - that shared formula is what made the original test
// circular.
std::vector<std::complex<float>> gen_dsss_burst(double snr_db, unsigned seed, int n_bits,
                                                 int idle_before, int idle_after) {
    static const int barker[11] = {+1, -1, +1, +1, -1, +1, +1, +1, -1, -1, -1};
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    std::bernoulli_distribution coin(0.5);

    constexpr int kOversample = 20;  // samples per chip in the shaping domain
    const double chip_rate = 11e6;

    std::vector<double> chips;
    for (int b = 0; b < n_bits; ++b) {
        double bit = coin(rng) ? 1.0 : -1.0;
        for (int c = 0; c < 11; ++c) chips.push_back(bit * barker[c]);
    }

    // Raised-cosine shaping across +/-2 chips, beta = 0.5.
    const int span = 2;
    const double beta = 0.5;
    auto rc = [&](double t) {
        if (std::abs(t) < 1e-12) return 1.0;
        double denom = 1.0 - std::pow(2.0 * beta * t, 2.0);
        if (std::abs(denom) < 1e-9) denom = 1e-9;
        return std::sin(M_PI * t) / (M_PI * t) * std::cos(M_PI * beta * t) / denom;
    };

    size_t hi_len = chips.size() * size_t(kOversample);
    std::vector<double> shaped(hi_len, 0.0);
    for (size_t c = 0; c < chips.size(); ++c) {
        for (int k = -span * kOversample; k <= span * kOversample; ++k) {
            long idx = long(c) * kOversample + k;
            if (idx < 0 || idx >= long(hi_len)) continue;
            shaped[size_t(idx)] += chips[c] * rc(double(k) / kOversample);
        }
    }

    // Resample from (11e6 * kOversample) down to kRate by linear
    // interpolation at the true fractional sample positions.
    double hi_rate = chip_rate * kOversample;
    size_t n_out = size_t(double(hi_len) * kRate / hi_rate);
    std::vector<double> sig(n_out, 0.0);
    for (size_t i = 0; i < n_out; ++i) {
        double pos = double(i) * hi_rate / kRate;
        size_t i0 = size_t(pos);
        double frac = pos - double(i0);
        double a = (i0 < hi_len) ? shaped[i0] : 0.0;
        double b = (i0 + 1 < hi_len) ? shaped[i0 + 1] : 0.0;
        sig[i] = a * (1.0 - frac) + b * frac;
    }

    double sig_power = 0.0;
    for (double s : sig) sig_power += s * s;
    sig_power /= double(sig.size());
    double noise_sigma = std::sqrt(sig_power / std::pow(10.0, snr_db / 10.0) / 2.0);

    std::vector<std::complex<float>> out;
    out.reserve(size_t(idle_before) + n_out + size_t(idle_after));
    auto push_noise = [&](int count) {
        for (int i = 0; i < count; ++i) {
            out.emplace_back(float(g(rng) * noise_sigma), float(g(rng) * noise_sigma));
        }
    };
    push_noise(idle_before);
    for (double s : sig) {
        out.emplace_back(float(s + g(rng) * noise_sigma), float(g(rng) * noise_sigma));
    }
    push_noise(idle_after);
    return out;
}

std::vector<std::complex<float>> gen_noise(size_t n, double sigma, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, sigma);
    std::vector<std::complex<float>> out(n);
    for (auto& s : out) s = std::complex<float>(float(g(rng)), float(g(rng)));
    return out;
}

std::vector<std::complex<float>> gen_cw(size_t n, double cnr_db, double offset_hz, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    double amp = std::sqrt(2.0 * std::pow(10.0, cnr_db / 10.0));
    std::vector<std::complex<float>> out(n);
    double phase = 0.0, inc = 2.0 * M_PI * offset_hz / kRate;
    for (size_t i = 0; i < n; ++i) {
        out[i] = std::complex<float>(float(amp * std::cos(phase) + g(rng)),
                                      float(amp * std::sin(phase) + g(rng)));
        phase += inc;
        if (phase > M_PI) phase -= 2 * M_PI;
    }
    return out;
}

// Channel 6 geometry, matching scanner.cpp: the capture is tuned
// WIFI_CHANNEL_CAPTURE_OFFSET_HZ ABOVE the real channel centre, so in a
// real capture the signal sits 1.5MHz BELOW the tuned centre and
// classify_modulation() mixes it back up to baseband.
constexpr double kCaptureCenter = 2437e6 + 1.5e6;
constexpr double kSegmentCenter = 2437e6;
constexpr double kChannelOffset = kSegmentCenter - kCaptureCenter;  // -1.5MHz

// Puts a baseband-generated signal where it would really sit inside the
// capture. Getting this wrong is not a cosmetic detail: the generators
// emit at 0Hz, and without this the classifier's own de-mixing shifts
// the signal 1.5MHz AWAY from baseband - 1.5 full phase rotations
// within a single 1us Barker symbol, which annihilates a coherent
// matched filter. Schmidl-Cox shrugs it off (a delayed-conjugate
// autocorrelation is frequency-offset immune by construction - that is
// precisely what makes it a CFO ESTIMATOR), so an error here silently
// kills the DSSS branch while OFDM keeps passing.
std::vector<std::complex<float>> place_in_channel(const std::vector<std::complex<float>>& iq,
                                                   double offset_hz = kChannelOffset) {
    std::vector<std::complex<float>> out(iq.size());
    double phase = 0.0, inc = 2.0 * M_PI * offset_hz / kRate;
    for (size_t i = 0; i < iq.size(); ++i) {
        out[i] = iq[i] * std::complex<float>(float(std::cos(phase)), float(std::sin(phase)));
        phase += inc;
        if (phase > M_PI) phase -= 2 * M_PI;
        else if (phase < -M_PI) phase += 2 * M_PI;
    }
    return out;
}

ModClass classify(const std::vector<std::complex<float>>& baseband_iq, double extra_cfo_hz = 0.0) {
    auto iq = place_in_channel(baseband_iq, kChannelOffset + extra_cfo_hz);
    return classify_modulation(iq, kRate, kCaptureCenter, kSegmentCenter, /*try_dsss=*/true).mod;
}

}  // namespace

int main() {
    constexpr int kTrials = 40;
    const double snrs[] = {30.0, 20.0, 15.0, 10.0, 5.0, 0.0, -5.0};

    std::printf("=== 2.4GHz classifier stress matrix (20 Msps, %d trials/cell) ===\n\n", kTrials);

    // --- Detection vs SNR -------------------------------------------
    std::printf("%-8s %-26s %-26s\n", "SNR dB", "OFDM  correct/wrong/miss", "DSSS  correct/wrong/miss");
    int ofdm_correct_at_10 = 0, dsss_correct_at_10 = 0;
    int ofdm_wrong_total = 0, dsss_wrong_total = 0;

    for (double snr : snrs) {
        int o_ok = 0, o_wrong = 0, o_miss = 0;
        int d_ok = 0, d_wrong = 0, d_miss = 0;
        for (int t = 0; t < kTrials; ++t) {
            unsigned seed = unsigned(1000 + t) * 7919u + unsigned(int(snr) + 100) * 104729u;
            ModClass m = classify(gen_ofdm_burst(snr, seed, 400, 400));
            if (m == ModClass::OFDM) ++o_ok;
            else if (m == ModClass::DSSS) ++o_wrong;
            else ++o_miss;

            ModClass d = classify(gen_dsss_burst(snr, seed + 13u, 40, 400, 400));
            if (d == ModClass::DSSS) ++d_ok;
            else if (d == ModClass::OFDM) ++d_wrong;
            else ++d_miss;
        }
        ofdm_wrong_total += o_wrong;
        dsss_wrong_total += d_wrong;
        if (snr == 10.0) {
            ofdm_correct_at_10 = o_ok;
            dsss_correct_at_10 = d_ok;
        }
        std::printf("%-8.0f %3d / %3d / %3d%14s %3d / %3d / %3d\n", snr, o_ok, o_wrong, o_miss, "",
                    d_ok, d_wrong, d_miss);
    }
    std::printf("\n");

    // A classifier may legitimately MISS a weak signal. It must never
    // confidently report the wrong modulation - that is a lie, not a
    // sensitivity limit, and it is what a "confusion" column is for.
    check(ofdm_wrong_total == 0, "ofdm_never_misclassified_as_dsss",
          "OFDM reported as DSSS in " + std::to_string(ofdm_wrong_total) + " trials across all SNRs");
    check(dsss_wrong_total == 0, "dsss_never_misclassified_as_ofdm",
          "DSSS reported as OFDM in " + std::to_string(dsss_wrong_total) + " trials across all SNRs");

    // At a comfortable 10dB SNR both should be found essentially always.
    check(ofdm_correct_at_10 >= int(kTrials * 0.9), "ofdm_detected_at_10db",
          std::to_string(ofdm_correct_at_10) + "/" + std::to_string(kTrials) + " detected");
    check(dsss_correct_at_10 >= int(kTrials * 0.9), "dsss_detected_at_10db",
          std::to_string(dsss_correct_at_10) + "/" + std::to_string(kTrials) + " detected");

    // --- False positives on things that are NOT Wi-Fi ---------------
    std::printf("=== false-positive battery (any non-Unknown result is a failure) ===\n");
    auto run_fp = [&](const char* name, int trials,
                       const std::function<std::vector<std::complex<float>>(unsigned)>& make) {
        int fp = 0;
        for (int t = 0; t < trials; ++t) {
            if (classify(make(unsigned(t) * 2654435761u)) != ModClass::Unknown) ++fp;
        }
        std::printf("  %-38s %d/%d false positives\n", name, fp, trials);
        check(fp == 0, std::string("no_false_positive_") + name,
              std::to_string(fp) + "/" + std::to_string(trials));
    };

    run_fp("pure_noise", 30, [](unsigned s) { return gen_noise(40000, 1.0, s); });
    run_fp("lo_leakage_dc_carrier", 30, [](unsigned s) { return gen_cw(40000, 7.0, 0.0, s); });
    run_fp("strong_lo_leakage", 30, [](unsigned s) { return gen_cw(40000, 25.0, 0.0, s); });
    run_fp("cw_interferer_+3MHz", 30, [](unsigned s) { return gen_cw(40000, 15.0, 3e6, s); });
    run_fp("cw_interferer_-5MHz", 30, [](unsigned s) { return gen_cw(40000, 15.0, -5e6, s); });
    run_fp("cw_interferer_+8MHz", 30, [](unsigned s) { return gen_cw(40000, 15.0, 8e6, s); });

    // --- Beacon clustering must not invent sources at real density ---
    // The "never over-counts" claim held at 60 aperiodic bursts and
    // collapsed at 400: measured 26 phantom sources per capture, because
    // 400 events saturate a 102.4ms phase space at ~1ms resolution and
    // periodicity stops carrying information. scanner.cpp therefore
    // feeds this ONLY beacon-plausible bursts (DSSS, >=1ms), which is
    // tens per second rather than hundreds. These densities reflect
    // that filtered input - the guard rail is that the false rate must
    // stay low across the range a real channel can produce.
    std::printf("=== beacon clustering false-source rate vs burst density ===\n");
    for (int density : {10, 20, 40, 80}) {
        int total_phantoms = 0;
        constexpr int kRuns = 25;
        for (int run = 0; run < kRuns; ++run) {
            std::mt19937 rng(unsigned(density) * 7919u + unsigned(run));
            std::uniform_real_distribution<double> t(0.0, 1.0);
            std::vector<double> starts, powers;
            for (int i = 0; i < density; ++i) {
                starts.push_back(t(rng));
                powers.push_back(-40.0);
            }
            total_phantoms += int(rfmon::wifi::find_beacon_sources(starts, powers).size());
        }
        double per_run = double(total_phantoms) / kRuns;
        std::printf("  %4d aperiodic beacon-candidates/s -> %.2f phantom sources per capture\n",
                    density, per_run);
        check(per_run <= 0.2, "beacon_clustering_low_false_rate_at_" + std::to_string(density),
              std::to_string(per_run) + " phantoms/capture");
    }

    // And it must still FIND real trains buried in that same traffic.
    {
        std::mt19937 rng(4242);
        std::uniform_real_distribution<double> t(0.0, 1.0);
        std::vector<double> starts, powers;
        for (int i = 0; i < 40; ++i) {
            starts.push_back(t(rng));
            powers.push_back(-40.0);
        }
        double phases[4] = {0.003, 0.028, 0.055, 0.081};
        for (double ph : phases) {
            for (int k = 0; k < 9; ++k) {
                starts.push_back(ph + k * rfmon::wifi::BEACON_INTERVAL_S);
                powers.push_back(-30.0);
            }
        }
        auto src = rfmon::wifi::find_beacon_sources(starts, powers);
        std::printf("  4 real trains buried in 40 aperiodic candidates -> %zu sources\n", src.size());
        check(src.size() >= 4 && src.size() <= 6, "beacon_clustering_finds_trains_under_load",
              "expected 4-6 (4 real + slack), got " + std::to_string(src.size()));
    }

    std::printf("\n");
    if (failures > 0) {
        std::printf("%d check(s) FAILED.\n", failures);
        return 1;
    }
    std::printf("All Wi-Fi stress checks passed.\n");
    return 0;
}
