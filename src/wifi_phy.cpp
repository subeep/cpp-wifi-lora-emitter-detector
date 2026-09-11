#include "wifi_phy.hpp"

#include <algorithm>
#include <cmath>

#include <kissfft/kiss_fft.h>

namespace rfmon::wifi {

namespace {

// --- shared helpers -------------------------------------------------

// Same boxcar decimator as scanner.cpp's decimate_boxcar() - duplicated
// rather than shared (small enough, and keeps this file's build
// dependencies to just its own header, matching how lora_phy_std.cpp
// already duplicates rather than reaches into other modules).
std::vector<std::complex<float>> decimate_boxcar(const std::vector<std::complex<float>>& in,
                                                  int factor) {
    if (factor <= 1) return in;
    std::vector<std::complex<float>> out(in.size() / size_t(factor));
    for (size_t i = 0; i < out.size(); ++i) {
        std::complex<float> sum(0.0f, 0.0f);
        for (int j = 0; j < factor; ++j) sum += in[i * size_t(factor) + size_t(j)];
        out[i] = sum / float(factor);
    }
    return out;
}

// Shifts the candidate `freq_offset_hz` away from the tuned center down
// to 0 Hz. Phase is kept wrapped to +/-pi rather than accumulated
// unboundedly - these buffers run into the millions of samples.
std::vector<std::complex<float>> mix_to_baseband(const std::vector<std::complex<float>>& iq,
                                                  double sample_rate_hz, double freq_offset_hz) {
    std::vector<std::complex<float>> out(iq.size());
    double phase_inc = -2.0 * M_PI * freq_offset_hz / sample_rate_hz;
    double phase = 0.0;
    for (size_t n = 0; n < iq.size(); ++n) {
        std::complex<float> rot(float(std::cos(phase)), float(std::sin(phase)));
        out[n] = iq[n] * rot;
        phase += phase_inc;
        if (phase > M_PI) phase -= 2.0 * M_PI;
        else if (phase < -M_PI) phase += 2.0 * M_PI;
    }
    return out;
}

// Decimate the mixed baseband slice before correlating - both
// correlators below only need a handful of samples per chip/symbol,
// not the full capture rate, and this is where that cost reduction
// happens (see wifi_phy.hpp's file header re: skipping VOLK for now).
constexpr int MOD_DECIM_FACTOR = 2;

// --- Barker-11 / DSSS-CCK correlator ---------------------------------

constexpr double BARKER_CHIP_RATE_HZ = 11e6;
constexpr float BARKER_CHIPS[11] = {+1, -1, +1, +1, -1, +1, +1, +1, -1, -1, -1};
constexpr double BARKER_PEAK_RATIO_THRESHOLD = 0.5;
constexpr int MIN_BARKER_CONSISTENT_PEAKS = 16;  // consecutive 1us bit periods

// Longest run, anywhere in `baseband`, of consecutive periodic peaks
// spaced one Barker-symbol (11 chips = 1us) apart that each clear
// BARKER_PEAK_RATIO_THRESHOLD - the "clean, periodic peak train" a
// real 802.11b DSSS/CCK preamble produces (matched-filter magnitude is
// insensitive to the DBPSK data bit's sign, so this fires regardless
// of the actual preamble bit values).
double barker_confidence(const std::vector<std::complex<float>>& baseband, double sample_rate_hz) {
    int template_len = int(std::lround(sample_rate_hz / 1e6));  // 1 Barker symbol = 1us
    if (template_len < 3 || baseband.size() < size_t(template_len) * (MIN_BARKER_CONSISTENT_PEAKS + 1)) {
        return 0.0;
    }

    size_t template_len_sz = size_t(template_len);
    std::vector<std::complex<float>> tmpl(template_len_sz);
    for (int i = 0; i < template_len; ++i) {
        int chip = std::min(10, int((double(i) / template_len) * 11.0));
        tmpl[size_t(i)] = std::complex<float>(BARKER_CHIPS[chip], 0.0f);
    }
    float template_energy = float(template_len);  // sum of +-1 squared

    size_t n_positions = baseband.size() - size_t(template_len) + 1;
    std::vector<float> ratio(n_positions);
    for (size_t n = 0; n < n_positions; ++n) {
        std::complex<float> corr(0.0f, 0.0f);
        float sig_energy = 0.0f;
        for (int k = 0; k < template_len; ++k) {
            std::complex<float> x = baseband[n + size_t(k)];
            corr += tmpl[size_t(k)] * x;  // template is real, conj is a no-op
            sig_energy += x.real() * x.real() + x.imag() * x.imag();
        }
        float denom = std::sqrt(sig_energy * template_energy) + 1e-12f;
        ratio[n] = std::abs(corr) / denom;
    }

    // Walk every chip-timing phase; within each, find the longest run
    // of consecutive one-symbol-spaced positions clearing threshold.
    int best_run = 0;
    for (int phase = 0; phase < template_len; ++phase) {
        int run = 0;
        for (size_t n = size_t(phase); n < n_positions; n += size_t(template_len)) {
            if (ratio[n] >= BARKER_PEAK_RATIO_THRESHOLD) {
                ++run;
                best_run = std::max(best_run, run);
            } else {
                run = 0;
            }
        }
    }
    return double(best_run) / double(MIN_BARKER_CONSISTENT_PEAKS);
}

// --- Schmidl-Cox / OFDM correlator -----------------------------------

constexpr double SC_SHORT_SYMBOL_S = 0.8e-6;  // one 802.11 L-STF short symbol
constexpr double SC_PLATEAU_THRESHOLD = 0.6;
constexpr int MIN_SC_PLATEAU_SYMBOLS = 4;  // consecutive short-symbol repeats

// Longest run, anywhere in `baseband`, of consecutive samples where
// M(d) = |P(d)|^2 / R(d)^2 clears SC_PLATEAU_THRESHOLD - the sustained
// near-1.0 plateau a repeated OFDM short training symbol produces.
// P/R use the standard incremental sliding-window recurrence so this
// stays O(N) rather than re-summing an L-sample window per position.
double schmidl_cox_confidence(const std::vector<std::complex<float>>& baseband,
                               double sample_rate_hz) {
    int L = int(std::lround(SC_SHORT_SYMBOL_S * sample_rate_hz));
    if (L < 3 || baseband.size() < size_t(2 * L) + size_t(MIN_SC_PLATEAU_SYMBOLS * L)) {
        return 0.0;
    }

    size_t n_d = baseband.size() - size_t(2 * L);
    std::complex<float> P(0.0f, 0.0f);
    float R = 0.0f;
    for (int k = 0; k < L; ++k) {
        P += std::conj(baseband[size_t(k)]) * baseband[size_t(k + L)];
        float m = std::abs(baseband[size_t(k + L)]);
        R += m * m;
    }

    int best_run = 0;
    int run = 0;
    for (size_t d = 0; d < n_d; ++d) {
        float p_mag2 = P.real() * P.real() + P.imag() * P.imag();
        float m_metric = p_mag2 / (R * R + 1e-12f);
        if (m_metric >= SC_PLATEAU_THRESHOLD) {
            ++run;
            best_run = std::max(best_run, run);
        } else {
            run = 0;
        }

        // Advance to d+1: drop x[d]/x[d+L] term, add x[d+L]/x[d+2L] term.
        if (d + 1 < n_d) {
            P += std::conj(baseband[d + size_t(L)]) * baseband[d + size_t(2 * L)] -
                 std::conj(baseband[d]) * baseband[d + size_t(L)];
            float old_m = std::abs(baseband[d + size_t(L)]);
            float new_m = std::abs(baseband[d + size_t(2 * L)]);
            R += new_m * new_m - old_m * old_m;
        }
    }
    return double(best_run) / double(MIN_SC_PLATEAU_SYMBOLS * L);
}

}  // namespace

ModClassification classify_modulation(const std::vector<std::complex<float>>& iq,
                                       double sample_rate_hz, double capture_center_hz,
                                       double segment_center_hz, bool try_dsss) {
    double offset_hz = segment_center_hz - capture_center_hz;
    std::vector<std::complex<float>> baseband = mix_to_baseband(iq, sample_rate_hz, offset_hz);

    // Schmidl-Cox's sliding recurrence is O(N) regardless of L, so
    // there's no cost reason to decimate before running it - and not
    // decimating means L is computed from the real sample rate, closer
    // to the true 0.8us short-symbol duration and less sensitive to
    // rounding than a smaller, decimated L would be.
    double sc_conf = schmidl_cox_confidence(baseband, sample_rate_hz);

    // Barker is O(N * template_len) - only this one decimates first, to
    // bound that cost (see wifi_phy.hpp's file header). Only decimate
    // if there's still enough headroom above the 11 Mchip/s Barker rate
    // afterward (>= 2 samples/chip) - this project's captures don't all
    // run at the same rate (e.g. the X310 here is capped to 20 Msps by
    // DeviceProfile::max_sample_rate_hz, well under the 56 Msps this
    // was tuned against), and decimating a capture that's already
    // barely above the chip rate would leave under 1 sample/chip,
    // breaking the matched filter rather than just saving time.
    double barker_conf = 0.0;
    if (try_dsss) {
        int decim = MOD_DECIM_FACTOR;
        if (sample_rate_hz / decim / BARKER_CHIP_RATE_HZ < 2.0) decim = 1;
        auto for_barker = decimate_boxcar(baseband, decim);
        barker_conf = barker_confidence(for_barker, sample_rate_hz / decim);
    }

    ModClassification result;
    if (barker_conf >= 1.0 && barker_conf >= sc_conf) {
        result.mod = ModClass::DSSS;
        result.confidence = barker_conf;
    } else if (sc_conf >= 1.0) {
        result.mod = ModClass::OFDM;
        result.confidence = sc_conf;
    }
    return result;
}

double estimate_occupied_bandwidth_hz(const std::complex<float>* x, size_t n,
                                       double sample_rate_hz, double threshold_db) {
    if (n == 0 || sample_rate_hz <= 0.0) return 0.0;

    constexpr int kFftSize = 256;  // coarse periodogram, not decode-grade
    int fft_n = std::min<int>(kFftSize, int(n));
    if (fft_n < 8) return 0.0;

    size_t fft_n_sz = size_t(fft_n);
    std::vector<kiss_fft_cpx> in(fft_n_sz);
    std::vector<kiss_fft_cpx> out(fft_n_sz);
    for (int i = 0; i < fft_n; ++i) {
        // Rectangular window is fine for a coarse occupied-BW estimate.
        in[size_t(i)].r = x[i].real();
        in[size_t(i)].i = x[i].imag();
    }
    kiss_fft_cfg cfg = kiss_fft_alloc(fft_n, 0, nullptr, nullptr);
    kiss_fft(cfg, in.data(), out.data());
    kiss_fft_free(cfg);

    // Bin order is [0..+Nyquist), [-Nyquist..0) - reorder to monotonic
    // frequency for a simple contiguous-index occupied-band scan.
    std::vector<float> mag2(fft_n_sz);
    for (int i = 0; i < fft_n; ++i) {
        int shifted = (i + fft_n / 2) % fft_n;
        float re = out[size_t(i)].r, im = out[size_t(i)].i;
        mag2[size_t(shifted)] = re * re + im * im;
    }

    float peak = *std::max_element(mag2.begin(), mag2.end());
    if (peak <= 0.0f) return 0.0;
    float thresh_lin = peak * float(std::pow(10.0, threshold_db / 10.0));

    int lo = 0, hi = fft_n - 1;
    while (lo < fft_n && mag2[size_t(lo)] < thresh_lin) ++lo;
    while (hi >= 0 && mag2[size_t(hi)] < thresh_lin) --hi;
    if (lo > hi) return 0.0;

    int occupied_bins = hi - lo + 1;
    double bin_hz = sample_rate_hz / fft_n;
    return occupied_bins * bin_hz;
}

double estimate_mean_power_db(const std::complex<float>* x, size_t n) {
    if (n == 0) return -200.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum += double(x[i].real()) * x[i].real() + double(x[i].imag()) * x[i].imag();
    }
    return 10.0 * std::log10(sum / double(n) + 1e-15);
}

}  // namespace rfmon::wifi
