// Synthetic-signal correctness check for the detection/classification
// pipeline, independent of whatever happens to be in the air right now.
//
// Injects a band-limited burst of known center frequency, bandwidth,
// and duty cycle into complex noise, runs it through the exact same
// spectrum -> detector -> classifier path the real scanner uses, and
// checks the recovered segment matches ground truth. Direct C++ port
// of the Python prototype's tests/test_dsp.py (same parameters, same
// tolerances) so both implementations are held to the same bar.

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include <kissfft/kiss_fft.h>

#include "../src/classifier.hpp"
#include "../src/config.hpp"
#include "../src/detector.hpp"
#include "../src/spectrum.hpp"

using namespace rfmon;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
        ++g_failures;
    }
}

// Band-limit `burst_len` samples of complex white noise to +/- bandwidth_hz/2
// via FFT-domain zeroing, then modulate to offset_hz.
std::vector<std::complex<float>> make_band_limited_burst(int burst_len, double sample_rate_hz,
                                                          double offset_hz, double bandwidth_hz,
                                                          double amplitude, std::mt19937& rng) {
    std::normal_distribution<double> normal(0.0, 1.0);
    std::vector<kiss_fft_cpx> in(burst_len), out(burst_len);
    for (int i = 0; i < burst_len; ++i) {
        in[i].r = static_cast<float>(normal(rng));
        in[i].i = static_cast<float>(normal(rng));
    }
    kiss_fft_cfg fwd = kiss_fft_alloc(burst_len, 0, nullptr, nullptr);
    kiss_fft(fwd, in.data(), out.data());
    kiss_fft_free(fwd);

    double bin_hz = sample_rate_hz / burst_len;
    for (int i = 0; i < burst_len; ++i) {
        int shifted = (i <= burst_len / 2) ? i : i - burst_len;  // fftfreq-style index
        double freq = shifted * bin_hz;
        if (std::abs(freq) > bandwidth_hz / 2.0) {
            out[i].r = 0.0f;
            out[i].i = 0.0f;
        }
    }

    kiss_fft_cfg inv = kiss_fft_alloc(burst_len, 1, nullptr, nullptr);
    std::vector<kiss_fft_cpx> band_limited(burst_len);
    kiss_fft(inv, out.data(), band_limited.data());
    kiss_fft_free(inv);

    std::vector<std::complex<float>> result(burst_len);
    double mean_sq = 0.0;
    for (int i = 0; i < burst_len; ++i) {
        // kissfft's inverse does not normalize by N.
        double re = band_limited[i].r / burst_len;
        double im = band_limited[i].i / burst_len;
        result[i] = std::complex<float>(float(re), float(im));
        mean_sq += re * re + im * im;
    }
    double rms = std::sqrt(mean_sq / burst_len);
    double scale = amplitude / (rms + 1e-12);

    for (int i = 0; i < burst_len; ++i) {
        double t = i / sample_rate_hz;
        std::complex<double> carrier(std::cos(2 * M_PI * offset_hz * t),
                                      std::sin(2 * M_PI * offset_hz * t));
        std::complex<double> v = std::complex<double>(result[i].real(), result[i].imag()) * scale *
                                  carrier;
        result[i] = std::complex<float>(float(v.real()), float(v.imag()));
    }
    return result;
}

std::vector<std::complex<float>> make_burst_capture(double sample_rate_hz, int n_samples,
                                                     double offset_hz, double bandwidth_hz,
                                                     double duty_cycle, double amplitude,
                                                     double noise_amplitude, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> normal(0.0, 1.0);

    std::vector<std::complex<float>> iq(n_samples);
    for (int i = 0; i < n_samples; ++i) {
        iq[i] = std::complex<float>(float(normal(rng) * noise_amplitude),
                                     float(normal(rng) * noise_amplitude));
    }

    int burst_len = int(n_samples * duty_cycle);
    int start = (n_samples - burst_len) / 2;
    auto burst = make_band_limited_burst(burst_len, sample_rate_hz, offset_hz, bandwidth_hz,
                                          amplitude, rng);
    for (int i = 0; i < burst_len; ++i) {
        iq[start + i] += burst[i];
    }
    return iq;
}

struct CaseResult {
    bool detected = false;
    Segment best{};
    std::string label;
};

CaseResult run_case(const std::string& name, const std::string& band, double sample_rate_hz,
                     double offset_hz, double bandwidth_hz, double duty_cycle,
                     const std::string& expect_label_substr, double freq_tol_hz,
                     double bw_tol_hz) {
    int n_samples = int(sample_rate_hz * SUB_CAPTURE_DURATION_S);
    std::vector<std::vector<std::complex<float>>> captures;
    for (int i = 0; i < SUB_CAPTURES_PER_STEP; ++i) {
        captures.push_back(make_burst_capture(sample_rate_hz, n_samples, offset_hz, bandwidth_hz,
                                               duty_cycle, /*amplitude=*/5.0,
                                               /*noise_amplitude=*/0.2, /*seed=*/i));
    }

    double tuned_center_hz = 1000e6;  // arbitrary; only offsets matter for this test
    Spectrum spec = max_hold_spectrum(captures, sample_rate_hz);
    double guard_hz = std::max(sample_rate_hz * DC_GUARD_FRACTION, DC_GUARD_MIN_HZ);
    std::vector<bool> dc_mask = mask_dc_guard(spec.freqs_offset_hz, guard_hz);
    std::vector<bool> edge_mask =
        mask_edge_guard(spec.freqs_offset_hz, sample_rate_hz, EDGE_GUARD_FRACTION);

    // Mirrors scanner.cpp's own policy: hysteresis growth only applies
    // outside the LoRa/sub-GHz band (see HYSTERESIS_LOW_RATIO's
    // comment in config.hpp).
    double hysteresis_low = (band == BAND_SUB_GHZ) ? DEFAULT_DETECTION_THRESHOLD_DB
                                                     : DEFAULT_DETECTION_THRESHOLD_DB * HYSTERESIS_LOW_RATIO;
    std::vector<Segment> segments =
        find_segments(spec.freqs_offset_hz, spec.psd_db, tuned_center_hz, edge_mask, dc_mask,
                      NOISE_FLOOR_PERCENTILE, DEFAULT_DETECTION_THRESHOLD_DB, MIN_SEGMENT_BINS,
                      MERGE_GAP_BINS, hysteresis_low);

    CaseResult result;
    check(!segments.empty(), "[" + name + "] expected a detection, got none");
    if (segments.empty()) return result;

    Segment best = segments[0];
    for (const auto& s : segments)
        if (s.peak_db > best.peak_db) best = s;

    double expected_center = tuned_center_hz + offset_hz;
    double freq_err = std::abs(best.center_hz - expected_center);
    double bw_err = std::abs(best.bandwidth_hz - bandwidth_hz);
    check(freq_err <= freq_tol_hz,
          "[" + name + "] center off by " + std::to_string(freq_err / 1e3) + "kHz (got " +
              std::to_string(best.center_hz / 1e6) + "MHz, want " +
              std::to_string(expected_center / 1e6) + "MHz)");
    check(bw_err <= bw_tol_hz,
          "[" + name + "] bandwidth off by " + std::to_string(bw_err / 1e3) + "kHz (got " +
              std::to_string(best.bandwidth_hz / 1e3) + "kHz, want " +
              std::to_string(bandwidth_hz / 1e3) + "kHz)");

    std::string label = classify(band, best);
    check(label.find(expect_label_substr) != std::string::npos,
          "[" + name + "] classified as '" + label + "', expected to contain '" +
              expect_label_substr + "'");

    std::printf("PASS [%s]: center=%.4fMHz bw=%.1fkHz peak=%.1fdB label='%s'\n", name.c_str(),
                best.center_hz / 1e6, best.bandwidth_hz / 1e3, best.peak_db, label.c_str());
    result.detected = true;
    result.best = best;
    result.label = label;
    return result;
}

void test_wifi_like_burst_is_detected() {
    run_case("wifi_20mhz_burst", BAND_WIFI_2G4, 50e6, -5e6, 20e6, 0.02, "WiFi-like", 1e6, 4e6);
}

// A strong, narrow "core" plus a wider but weaker "skirt" superimposed
// at the same center - like a real WiFi channel's stronger central
// energy tailing off toward its edges. Growth should extend the
// measured segment out into the skirt (not just the core), and the
// low-threshold band (WiFi) should measure meaningfully wider than the
// high-threshold-only band (LoRa/sub-GHz, where growth is disabled).
void test_hysteresis_grows_into_weak_skirt() {
    double sample_rate_hz = 50e6;
    int n_samples = int(sample_rate_hz * SUB_CAPTURE_DURATION_S);
    std::mt19937 rng(42);
    double core_bw = 6e6, skirt_bw = 18e6;

    auto core = make_band_limited_burst(n_samples, sample_rate_hz, 0.0, core_bw, 5.0, rng);
    auto skirt = make_band_limited_burst(n_samples, sample_rate_hz, 0.0, skirt_bw, 0.5, rng);
    std::vector<std::complex<float>> iq(n_samples);
    std::normal_distribution<double> normal(0.0, 1.0);
    for (int i = 0; i < n_samples; ++i) {
        iq[i] = core[i] + skirt[i] +
                std::complex<float>(float(normal(rng) * 0.2), float(normal(rng) * 0.2));
    }

    double tuned_center_hz = 2434.5e6;
    Spectrum spec = max_hold_spectrum({iq}, sample_rate_hz);
    double guard_hz = std::max(sample_rate_hz * DC_GUARD_FRACTION, DC_GUARD_MIN_HZ);
    std::vector<bool> dc_mask = mask_dc_guard(spec.freqs_offset_hz, guard_hz);
    std::vector<bool> edge_mask =
        mask_edge_guard(spec.freqs_offset_hz, sample_rate_hz, EDGE_GUARD_FRACTION);

    auto best_of = [&](double hysteresis_low) -> Segment {
        auto segs = find_segments(spec.freqs_offset_hz, spec.psd_db, tuned_center_hz, edge_mask,
                                   dc_mask, NOISE_FLOOR_PERCENTILE, DEFAULT_DETECTION_THRESHOLD_DB,
                                   MIN_SEGMENT_BINS, MERGE_GAP_BINS, hysteresis_low);
        Segment best{};
        for (const auto& s : segs)
            if (s.peak_db > best.peak_db) best = s;
        return best;
    };

    Segment no_growth = best_of(DEFAULT_DETECTION_THRESHOLD_DB);  // low == high: disabled
    Segment grown = best_of(DEFAULT_DETECTION_THRESHOLD_DB * HYSTERESIS_LOW_RATIO);

    check(no_growth.bandwidth_hz > 0.0,
          "[hysteresis_grows_into_weak_skirt] expected the ungrown core to be detected at all");
    check(grown.bandwidth_hz > no_growth.bandwidth_hz * 1.5,
          "[hysteresis_grows_into_weak_skirt] grown bandwidth (" +
              std::to_string(grown.bandwidth_hz / 1e3) + "kHz) not meaningfully wider than core-only (" +
              std::to_string(no_growth.bandwidth_hz / 1e3) + "kHz)");
    check(grown.bandwidth_hz <= skirt_bw * 1.2,
          "[hysteresis_grows_into_weak_skirt] grown bandwidth (" +
              std::to_string(grown.bandwidth_hz / 1e3) +
              "kHz) overshot the true skirt width (" + std::to_string(skirt_bw / 1e3) + "kHz)");

    if (grown.bandwidth_hz > no_growth.bandwidth_hz * 1.5 && grown.bandwidth_hz <= skirt_bw * 1.2) {
        std::printf(
            "PASS [hysteresis_grows_into_weak_skirt]: core-only=%.1fkHz grown=%.1fkHz (true "
            "core=%.0fkHz, true skirt=%.0fkHz)\n",
            no_growth.bandwidth_hz / 1e3, grown.bandwidth_hz / 1e3, core_bw / 1e3, skirt_bw / 1e3);
    }
}

void test_lora_like_burst_is_detected() {
    run_case("lora_125khz_burst", BAND_SUB_GHZ, 5e6, 1.2e6, 125e3, 0.4, "LoRa-like", 20e3, 40e3);
}

void test_dc_guard_rejects_center_spike() {
    double sample_rate_hz = 50e6;
    int n_samples = int(sample_rate_hz * SUB_CAPTURE_DURATION_S);
    std::mt19937 rng(0);
    std::normal_distribution<double> normal(0.0, 1.0);
    std::vector<std::complex<float>> iq(n_samples);
    for (int i = 0; i < n_samples; ++i) {
        iq[i] = std::complex<float>(float(normal(rng) * 0.2 + 5.0), float(normal(rng) * 0.2));
    }

    double tuned_center_hz = 2422e6;
    Spectrum spec = max_hold_spectrum({iq}, sample_rate_hz);
    double guard_hz = std::max(sample_rate_hz * DC_GUARD_FRACTION, DC_GUARD_MIN_HZ);
    std::vector<bool> dc_mask = mask_dc_guard(spec.freqs_offset_hz, guard_hz);
    std::vector<bool> edge_mask =
        mask_edge_guard(spec.freqs_offset_hz, sample_rate_hz, EDGE_GUARD_FRACTION);

    std::vector<Segment> segments =
        find_segments(spec.freqs_offset_hz, spec.psd_db, tuned_center_hz, edge_mask, dc_mask,
                      NOISE_FLOOR_PERCENTILE, DEFAULT_DETECTION_THRESHOLD_DB, MIN_SEGMENT_BINS,
                      MERGE_GAP_BINS, DEFAULT_DETECTION_THRESHOLD_DB * HYSTERESIS_LOW_RATIO);
    bool bogus = false;
    for (const auto& s : segments) {
        if (std::abs(s.center_hz - tuned_center_hz) < guard_hz) bogus = true;
    }
    check(!bogus, "DC guard failed to reject center spike");
    if (!bogus) std::printf("PASS [dc_guard_rejects_center_spike]: no false detection at tuned center\n");
}

void test_edge_guard_rejects_nyquist_edge_artifact() {
    double sample_rate_hz = 56e6;
    int n_samples = int(sample_rate_hz * SUB_CAPTURE_DURATION_S);
    double edge_offset_hz = (sample_rate_hz / 2.0) * 0.97;  // well inside the excluded outer edge

    auto iq = make_burst_capture(sample_rate_hz, n_samples, edge_offset_hz, 4e6, 1.0, 5.0, 0.2, 0);

    double tuned_center_hz = 2434.5e6;
    Spectrum spec = max_hold_spectrum({iq}, sample_rate_hz);
    double guard_hz = std::max(sample_rate_hz * DC_GUARD_FRACTION, DC_GUARD_MIN_HZ);
    std::vector<bool> dc_mask = mask_dc_guard(spec.freqs_offset_hz, guard_hz);
    std::vector<bool> edge_mask =
        mask_edge_guard(spec.freqs_offset_hz, sample_rate_hz, EDGE_GUARD_FRACTION);

    std::vector<Segment> segments =
        find_segments(spec.freqs_offset_hz, spec.psd_db, tuned_center_hz, edge_mask, dc_mask,
                      NOISE_FLOOR_PERCENTILE, DEFAULT_DETECTION_THRESHOLD_DB, MIN_SEGMENT_BINS,
                      MERGE_GAP_BINS, DEFAULT_DETECTION_THRESHOLD_DB * HYSTERESIS_LOW_RATIO);
    double expected_center = tuned_center_hz + edge_offset_hz;
    bool bogus = false;
    for (const auto& s : segments) {
        if (std::abs(s.center_hz - expected_center) < 2e6) bogus = true;
    }
    check(!bogus, "Edge guard failed to reject Nyquist-edge artifact");
    if (!bogus)
        std::printf("PASS [edge_guard_rejects_nyquist_edge_artifact]: no false detection at Nyquist edge\n");
}

}  // namespace

int main() {
    test_wifi_like_burst_is_detected();
    test_hysteresis_grows_into_weak_skirt();
    test_lora_like_burst_is_detected();
    test_dc_guard_rejects_center_spike();
    test_edge_guard_rejects_nyquist_edge_artifact();

    if (g_failures == 0) {
        std::printf("\nAll DSP pipeline checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) failed.\n", g_failures);
    return 1;
}
