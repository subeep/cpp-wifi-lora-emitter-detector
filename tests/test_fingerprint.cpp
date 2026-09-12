// Synthetic correctness tests for src/fingerprint.cpp - per the source
// spec's own instruction (§8 step 3): "Synthesise a reference, inject a
// known eps, phi and DC offset, and confirm the estimator recovers them
// to within resolution... before you look at a single real device."
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "fingerprint.hpp"

using namespace rfmon::fingerprint;

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

std::vector<std::complex<float>> base_upchirp(int sf) {
    int N = 1 << sf;
    std::vector<std::complex<float>> out(N);
    for (int n = 0; n < N; ++n) {
        double phase = 2.0 * M_PI * (double(n) * n / (2.0 * N) - n / 2.0);
        out[n] = std::complex<float>(float(std::cos(phase)), float(std::sin(phase)));
    }
    return out;
}

// Builds a synthetic capture: `pad_symbols` symbols of pure noise, then
// `preamble_len` symbols of an impaired+CFO'd+scaled preamble (source
// spec §3.4's widely-linear model + a CFO rotation), plus noise
// throughout. Returns the buffer and the start_sample of the preamble.
std::vector<std::complex<float>> synth_capture(int sf, double bandwidth_hz, int pad_symbols,
                                                int preamble_len, double eps, double phi_deg,
                                                double dc_dbc, double dc_ang_deg,
                                                double cfo_bins, double channel_gain,
                                                double noise_amp, long& start_sample_out,
                                                unsigned seed) {
    int N = 1 << sf;
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, noise_amp);

    double phi = phi_deg * M_PI / 180.0;
    std::complex<double> mu = (1.0 + (1.0 + eps) * std::polar(1.0, -phi)) / 2.0;
    std::complex<double> nu = (1.0 - (1.0 + eps) * std::polar(1.0, +phi)) / 2.0;
    // Sanity identity the source spec calls out explicitly (§3.4):
    // "the true (unscaled) coefficients always satisfy mu + conj(nu) = 1".
    std::complex<double> identity_check = mu + std::conj(nu);
    if (std::abs(identity_check - 1.0) > 1e-9) {
        std::printf("FAIL [mu_nu_identity]: mu+conj(nu)=%.9f%+.9fi, expected 1\n",
                    identity_check.real(), identity_check.imag());
        ++failures;
    }
    std::complex<double> c_offset =
        std::abs(mu) * std::pow(10.0, dc_dbc / 20.0) *
        std::polar(1.0, std::arg(mu) + dc_ang_deg * M_PI / 180.0);

    auto up = base_upchirp(sf);
    long total_symbols = long(pad_symbols) + preamble_len;
    std::vector<std::complex<float>> iq(size_t(total_symbols) * size_t(N));

    long start_sample = long(pad_symbols) * N;
    start_sample_out = start_sample;

    double delta_f_hz = double(cfo_bins) * bandwidth_hz / double(N);
    double phase_inc = 2.0 * M_PI * delta_f_hz / bandwidth_hz;  // + : matches extractor's -derotation
    double phase = 0.0;

    for (long i = 0; i < long(iq.size()); ++i) {
        std::complex<double> sample;
        if (i >= start_sample && i < start_sample + long(preamble_len) * N) {
            std::complex<double> s(up[size_t(i % N)].real(), up[size_t(i % N)].imag());
            std::complex<double> impaired = mu * s + nu * std::conj(s) + c_offset;
            std::complex<double> rot(std::cos(phase), std::sin(phase));
            sample = channel_gain * impaired * rot;
            phase += phase_inc;
            if (phase > M_PI) phase -= 2 * M_PI;
        } else {
            sample = 0.0;
        }
        sample += std::complex<double>(noise(rng), noise(rng));
        iq[size_t(i)] = std::complex<float>(float(sample.real()), float(sample.imag()));
    }
    return iq;
}

}  // namespace

int main() {
    int sf = 7;
    double bandwidth_hz = 125e3;
    double center_hz = 866.9e6;
    int preamble_len = 8;
    int pad_symbols = 4;

    // 1. Clean-ish injection at good SNR - the main correctness check.
    {
        double eps = 0.02, phi_deg = 2.0, dc_dbc = -40.0, dc_ang_deg = 30.0;
        int cfo_bins = 5;
        long start_sample;
        auto iq = synth_capture(sf, bandwidth_hz, pad_symbols, preamble_len, eps, phi_deg, dc_dbc,
                                 dc_ang_deg, cfo_bins, /*channel_gain=*/3.7, /*noise_amp=*/0.01,
                                 start_sample, 1);
        auto fp = extract_lora_fingerprint(iq, sf, bandwidth_hz, center_hz, start_sample,
                                            preamble_len, cfo_bins);

        check(!fp.gated_out, "clean_injection_not_gated", fp.gate_reason);

        double expected_cfo_ppm = (double(cfo_bins) * bandwidth_hz / (1 << sf) / center_hz) * 1e6;
        // Tolerance is noise-limited, not exact: the sub-bin refinement
        // (see fingerprint.cpp) estimates a residual from the actual
        // (noisy) preamble samples even when the true residual is
        // exactly zero, as it is here - it recovers to within a small
        // fraction of this test's own noise floor, not to bit-exactness.
        check(std::abs(fp.cfo_ppm - expected_cfo_ppm) < 1e-3, "cfo_ppm_exact",
              "got " + std::to_string(fp.cfo_ppm) + ", want " + std::to_string(expected_cfo_ppm));

        double phi = phi_deg * M_PI / 180.0;
        std::complex<double> mu = (1.0 + (1.0 + eps) * std::polar(1.0, -phi)) / 2.0;
        std::complex<double> nu = (1.0 - (1.0 + eps) * std::polar(1.0, +phi)) / 2.0;
        double expected_irr_db = 20.0 * std::log10(std::abs(nu / mu));
        check(std::abs(fp.irr_db - expected_irr_db) < 0.5, "irr_db_recovered",
              "got " + std::to_string(fp.irr_db) + "dB, want " + std::to_string(expected_irr_db) +
                  "dB");
        check(std::abs(fp.iq_eps - eps) < 0.002, "iq_eps_recovered",
              "got " + std::to_string(fp.iq_eps) + ", want " + std::to_string(eps));
        check(std::abs(fp.iq_phi_deg - phi_deg) < 0.2, "iq_phi_deg_recovered",
              "got " + std::to_string(fp.iq_phi_deg) + "deg, want " + std::to_string(phi_deg) +
                  "deg");
        check(std::abs(fp.dc_dbc - dc_dbc) < 0.5, "dc_dbc_recovered",
              "got " + std::to_string(fp.dc_dbc) + "dBc, want " + std::to_string(dc_dbc) + "dBc");
        check(std::abs(fp.dc_ang_deg - dc_ang_deg) < 2.0, "dc_ang_deg_recovered",
              "got " + std::to_string(fp.dc_ang_deg) + "deg, want " + std::to_string(dc_ang_deg) +
                  "deg");
        check(fp.snr_db > LORA_SNR_FLOOR_DB, "snr_above_floor",
              "snr_db=" + std::to_string(fp.snr_db));
        check(fp.evm_pct < 10.0, "evm_reasonable", "evm_pct=" + std::to_string(fp.evm_pct));
        check(fp.sync_corr > 0.9, "sync_corr_high", "sync_corr=" + std::to_string(fp.sync_corr));
    }

    // 2. G-invariance: a much larger/smaller channel gain must not
    // change the recovered ratios at all (the whole point of Rule 1).
    {
        double eps = 0.03, phi_deg = -1.5, dc_dbc = -35.0, dc_ang_deg = -60.0;
        int cfo_bins = -3;
        long start_sample;
        auto iq_small = synth_capture(sf, bandwidth_hz, pad_symbols, preamble_len, eps, phi_deg,
                                       dc_dbc, dc_ang_deg, cfo_bins, /*channel_gain=*/0.2, 0.002,
                                       start_sample, 2);
        auto iq_large = synth_capture(sf, bandwidth_hz, pad_symbols, preamble_len, eps, phi_deg,
                                       dc_dbc, dc_ang_deg, cfo_bins, /*channel_gain=*/50.0, 0.5,
                                       start_sample, 2);
        auto fp_small = extract_lora_fingerprint(iq_small, sf, bandwidth_hz, center_hz,
                                                  start_sample, preamble_len, cfo_bins);
        auto fp_large = extract_lora_fingerprint(iq_large, sf, bandwidth_hz, center_hz,
                                                  start_sample, preamble_len, cfo_bins);
        check(!fp_small.gated_out && !fp_large.gated_out, "g_invariance_not_gated",
              "small.gated=" + std::to_string(fp_small.gated_out) +
                  " large.gated=" + std::to_string(fp_large.gated_out));
        check(std::abs(fp_small.irr_db - fp_large.irr_db) < 0.5, "g_invariance_irr_db",
              "small=" + std::to_string(fp_small.irr_db) +
                  "dB large=" + std::to_string(fp_large.irr_db) + "dB");
        check(std::abs(fp_small.dc_dbc - fp_large.dc_dbc) < 0.5, "g_invariance_dc_dbc",
              "small=" + std::to_string(fp_small.dc_dbc) +
                  "dBc large=" + std::to_string(fp_large.dc_dbc) + "dBc");
    }

    // 2b. The quality gate this was all added for: a genuinely clean
    // preamble at its TRUE SF, analyzed against the WRONG SF hypothesis
    // (a stand-in for a real cross-hypothesis false match, where a real
    // strong signal sits behind an incorrect (SF, BW) guess and still
    // clears the SNR floor). The mismatched chirp-rate reference should
    // produce a poor fit that gets rejected on EVM/sync_corr, even
    // though the underlying signal is strong and clean.
    {
        int true_sf = 7, wrong_sf = 8;
        long start_sample;
        auto iq = synth_capture(true_sf, bandwidth_hz, /*pad_symbols=*/8, /*preamble_len=*/16,
                                 0.02, 2.0, -40.0, 30.0, /*cfo_bins=*/5, /*channel_gain=*/3.7,
                                 /*noise_amp=*/0.01, start_sample, 99);
        auto fp_wrong = extract_lora_fingerprint(iq, wrong_sf, bandwidth_hz, center_hz,
                                                  start_sample, /*preamble_len=*/8, /*cfo_bins=*/5);
        check(fp_wrong.gated_out, "wrong_sf_hypothesis_rejected",
              fp_wrong.gated_out ? "gate_reason='" + fp_wrong.gate_reason + "'"
                                  : "expected gated_out, was NOT gated (evm_pct=" +
                                        std::to_string(fp_wrong.evm_pct) +
                                        " sync_corr=" + std::to_string(fp_wrong.sync_corr) + ")");

        // The correct SF, same signal, should still pass cleanly -
        // confirms the rejection above is really about the mismatch,
        // not some side effect of this test's own parameters.
        auto fp_right = extract_lora_fingerprint(iq, true_sf, bandwidth_hz, center_hz,
                                                  start_sample, /*preamble_len=*/16,
                                                  /*cfo_bins=*/5);
        check(!fp_right.gated_out, "correct_sf_hypothesis_still_passes", fp_right.gate_reason);
    }

    // 2c. The bug this fix targets: a real device's CFO always lands
    // between integer bins (find_preamble()'s FFT peak has no sub-bin
    // resolution), and real preambles run long (TarangMini measured
    // 40-41 symbols on real hardware). Inject a truth of 5.35 bins but
    // hand the extractor the coarse-rounded 5 - exactly what the real
    // codec would produce - over a realistic 40-symbol preamble. The
    // 0.35-bin residual, uncorrected, accumulates to ~14 full
    // rotations of phase drift across that window (2*pi*0.35*40 rad) -
    // enough to fail EVM outright without the refinement this fix adds.
    {
        double true_cfo_bins = 5.35;
        int coarse_cfo_bins = int(std::lround(true_cfo_bins));
        int long_preamble_len = 40;
        long start_sample;
        auto iq = synth_capture(sf, bandwidth_hz, /*pad_symbols=*/8, long_preamble_len, 0.02, 2.0,
                                 -40.0, 30.0, true_cfo_bins, /*channel_gain=*/3.7,
                                 /*noise_amp=*/0.01, start_sample, 42);
        auto fp = extract_lora_fingerprint(iq, sf, bandwidth_hz, center_hz, start_sample,
                                            long_preamble_len, coarse_cfo_bins);
        check(!fp.gated_out, "fractional_cfo_long_preamble_not_gated",
              fp.gate_reason + " (evm_pct=" + std::to_string(fp.evm_pct) +
                  " sync_corr=" + std::to_string(fp.sync_corr) + ")");

        double expected_cfo_ppm =
            (true_cfo_bins * bandwidth_hz / (1 << sf) / center_hz) * 1e6;
        double naive_cfo_ppm =
            (double(coarse_cfo_bins) * bandwidth_hz / (1 << sf) / center_hz) * 1e6;
        double naive_error = std::abs(naive_cfo_ppm - expected_cfo_ppm);
        double refined_error = std::abs(fp.cfo_ppm - expected_cfo_ppm);
        // The refined estimate should land far closer to the true
        // fractional value than the coarse integer-bin snap would -
        // proves this is genuine sub-bin recovery, not just a gate
        // that happens to pass.
        check(refined_error < naive_error * 0.2, "cfo_ppm_subbin_refinement",
              "refined_error=" + std::to_string(refined_error) +
                  "ppm, naive_error=" + std::to_string(naive_error) + "ppm");
    }

    // 3. Pure noise (no real signal at all in the "preamble" window)
    // must gate out on SNR, never emit a fabricated fingerprint.
    {
        int cfo_bins = 0;
        long start_sample;
        auto iq = synth_capture(sf, bandwidth_hz, pad_symbols, preamble_len, 0.0, 0.0, -100.0, 0.0,
                                 cfo_bins, /*channel_gain=*/0.0, /*noise_amp=*/0.5, start_sample,
                                 3);
        auto fp = extract_lora_fingerprint(iq, sf, bandwidth_hz, center_hz, start_sample,
                                            preamble_len, cfo_bins);
        check(fp.gated_out, "pure_noise_gated_out", "gate_reason='" + fp.gate_reason + "'");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED.\n", failures);
        return 1;
    }
    std::printf("\nAll fingerprint extraction checks passed.\n");
    return 0;
}
