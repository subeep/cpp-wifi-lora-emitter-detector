#include "fingerprint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace rfmon::fingerprint {

namespace {

// Same formula as lora_phy.cpp's base_upchirp() - duplicated rather
// than shared, matching this project's established convention (see
// lora_phy_std.cpp's file header) of not risking a shared refactor of
// already-validated code for a new, separate consumer.
std::vector<std::complex<float>> base_upchirp(int sf) {
    int N = 1 << sf;
    std::vector<std::complex<float>> out(N);
    for (int n = 0; n < N; ++n) {
        double phase = 2.0 * M_PI * (double(n) * n / (2.0 * N) - n / 2.0);
        out[n] = std::complex<float>(float(std::cos(phase)), float(std::sin(phase)));
    }
    return out;
}

// Solves the fixed 3x3 complex linear system A*x = b by Gaussian
// elimination with partial pivoting (by magnitude) - small and fixed
// enough that a general linear-algebra dependency isn't worth adding
// just for this one fit (source spec §3.4's widely-linear IQ model has
// exactly 3 unknowns: mu, nu, c).
bool solve3x3(std::complex<double> a[3][3], std::complex<double> b[3], std::complex<double> x[3]) {
    for (int col = 0; col < 3; ++col) {
        int pivot = col;
        double best = std::abs(a[col][col]);
        for (int r = col + 1; r < 3; ++r) {
            double m = std::abs(a[r][col]);
            if (m > best) {
                best = m;
                pivot = r;
            }
        }
        if (best < 1e-15) return false;
        if (pivot != col) {
            std::swap(a[pivot], a[col]);
            std::swap(b[pivot], b[col]);
        }
        for (int r = col + 1; r < 3; ++r) {
            std::complex<double> factor = a[r][col] / a[col][col];
            for (int c = col; c < 3; ++c) a[r][c] -= factor * a[col][c];
            b[r] -= factor * b[col];
        }
    }
    for (int r = 2; r >= 0; --r) {
        std::complex<double> sum = b[r];
        for (int c = r + 1; c < 3; ++c) sum -= a[r][c] * x[c];
        x[r] = sum / a[r][r];
    }
    return true;
}

double round_to(double v, double res) { return std::round(v / res) * res; }

// JSON doesn't allow NaN/Infinity (source spec §4.3 rule 4) - callers
// only ever pass gated-out doubles through here as a defensive measure,
// since a NaN slipping through would otherwise silently corrupt the
// NDJSON line for every field after it.
std::string num_or_null(double v, double res) {
    if (!std::isfinite(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", round_to(v, res));
    return std::string(buf);
}

}  // namespace

LoraFingerprint extract_lora_fingerprint(const std::vector<std::complex<float>>& iq, int sf,
                                          double bandwidth_hz, double center_hz,
                                          long start_sample, int preamble_len, int cfo_bins) {
    LoraFingerprint fp;
    int N = 1 << sf;
    long preamble_samples = long(preamble_len) * N;

    if (preamble_len < 1 || start_sample < 0 ||
        size_t(start_sample + preamble_samples) > iq.size()) {
        fp.gated_out = true;
        fp.gate_reason = "insufficient preamble samples";
        return fp;
    }
    if (start_sample < N) {
        fp.gated_out = true;
        fp.gate_reason = "no pre-burst noise window available";
        return fp;
    }

    // --- SNR gate (source spec §2.2) - a low-SNR packet doesn't
    // produce a slightly worse parameter, it produces a wrong one, so
    // this rejects before any of the fitting below runs at all. ---
    double noise_power = 0.0;
    for (long i = start_sample - N; i < start_sample; ++i) {
        noise_power += double(iq[size_t(i)].real()) * iq[size_t(i)].real() +
                       double(iq[size_t(i)].imag()) * iq[size_t(i)].imag();
    }
    noise_power /= double(N);

    double burst_power = 0.0;
    for (long i = start_sample; i < start_sample + preamble_samples; ++i) {
        burst_power += double(iq[size_t(i)].real()) * iq[size_t(i)].real() +
                       double(iq[size_t(i)].imag()) * iq[size_t(i)].imag();
    }
    burst_power /= double(preamble_samples);

    if (noise_power <= 0.0 || burst_power <= noise_power) {
        fp.gated_out = true;
        fp.gate_reason = "SNR non-positive";
        return fp;
    }
    fp.snr_db = 10.0 * std::log10((burst_power - noise_power) / noise_power);
    if (fp.snr_db < LORA_SNR_FLOOR_DB) {
        fp.gated_out = true;
        fp.gate_reason = "below SNR floor";
        return fp;
    }

    // --- Unit-RMS normalize the preamble segment (source spec §2.3 -
    // this is what makes every ratio below receiver-gain-invariant).
    // Everything from here down uses only the first LORA_FP_MAX_FIT_SYMBOLS
    // of the (possibly much longer) measured preamble - see that
    // constant's own comment for why. burst_power/SNR above deliberately
    // stayed on the full preamble; a longer average there is strictly
    // better, it's only the CFO/LS fit that a long window hurts. ---
    int fit_len = std::min(preamble_len, LORA_FP_MAX_FIT_SYMBOLS);
    long fit_samples = long(fit_len) * N;
    size_t fit_samples_sz = size_t(fit_samples);
    double g = std::sqrt(burst_power);
    std::vector<std::complex<double>> r_tilde(fit_samples_sz);
    for (long i = 0; i < fit_samples; ++i) {
        r_tilde[size_t(i)] = std::complex<double>(iq[size_t(start_sample + i)].real(),
                                                    iq[size_t(start_sample + i)].imag()) /
                              g;
    }

    // --- Reference signal: the ideal ppm=sf upchirp, tiled once per
    // preamble symbol (source spec §3.2 - "for fixed preambles, s[n]
    // is known a priori... synthesise it once"). Built before the CFO
    // refinement below since that needs it too, not just the LS fit. ---
    auto up = base_upchirp(sf);
    std::vector<std::complex<double>> s(fit_samples_sz);
    for (long i = 0; i < fit_samples; ++i) {
        auto v = up[size_t(i % N)];
        s[size_t(i)] = std::complex<double>(v.real(), v.imag());
    }

    // --- CFO refinement (source spec §3.1 derotation, precision fixed
    // up front here) - cfo_bins is the codec's own raw FFT-peak bin
    // (see find_preamble() in lora_phy.cpp), with no sub-bin
    // resolution: up to +/-0.5 bin of true CFO always remains after
    // derotating by it alone. That residual's accumulated phase drift
    // across the whole preamble is pi*preamble_len radians regardless
    // of SF/BW (N and BW cancel exactly) - real captures run
    // 8-40+ symbols (see TARANGMINI_LORA_FINDINGS.md), so at
    // preamble_len=40 even the full +/-0.5 bin case wraps ~20 times,
    // which wrecks EVM/sync_corr below on every real capture, correct
    // (SF, BW) hypothesis included (confirmed: 0% of real captures
    // passed the gate before this was added). Refined here via the
    // classic differential (Moose-style) estimator: correlate each
    // preamble symbol against the known reference, then the phase
    // rotation between consecutive symbols' correlations is exactly
    // the leftover sub-bin frequency error - unambiguous over one
    // symbol of spacing precisely because the residual is bounded to
    // +/-0.5 bin by cfo_bins already being the nearest integer.
    double delta_f_hz = double(cfo_bins) * bandwidth_hz / double(N);
    double residual_bins = 0.0;
    if (fit_len >= 2) {
        double coarse_phase_inc = -2.0 * M_PI * delta_f_hz / bandwidth_hz;
        size_t fit_len_sz = size_t(fit_len);
        std::vector<std::complex<double>> sym_corr(fit_len_sz);
        double phase = 0.0;
        for (int m = 0; m < fit_len; ++m) {
            std::complex<double> corr(0.0, 0.0);
            for (int n = 0; n < N; ++n) {
                long i = long(m) * N + n;
                std::complex<double> rot(std::cos(phase), std::sin(phase));
                corr += (r_tilde[size_t(i)] * rot) * std::conj(s[size_t(i)]);
                phase += coarse_phase_inc;
                if (phase > M_PI) phase -= 2.0 * M_PI;
                else if (phase < -M_PI) phase += 2.0 * M_PI;
            }
            sym_corr[size_t(m)] = corr;
        }
        std::complex<double> diff_sum(0.0, 0.0);
        for (int m = 0; m + 1 < fit_len; ++m) {
            diff_sum += sym_corr[size_t(m + 1)] * std::conj(sym_corr[size_t(m)]);
        }
        if (std::abs(diff_sum) > 1e-12) {
            residual_bins = std::arg(diff_sum) / (2.0 * M_PI);
        }
    }
    double delta_f_hz_refined = delta_f_hz + residual_bins * bandwidth_hz / double(N);
    fp.cfo_ppm = (delta_f_hz_refined / center_hz) * 1e6;

    double phase_inc = -2.0 * M_PI * delta_f_hz_refined / bandwidth_hz;
    double phase = 0.0;
    std::vector<std::complex<double>> r_prime(fit_samples_sz);
    for (long i = 0; i < fit_samples; ++i) {
        std::complex<double> rot(std::cos(phase), std::sin(phase));
        r_prime[size_t(i)] = r_tilde[size_t(i)] * rot;
        phase += phase_inc;
        if (phase > M_PI) phase -= 2.0 * M_PI;
        else if (phase < -M_PI) phase += 2.0 * M_PI;
    }

    // --- I/Q imbalance + DC offset: one least-squares solve (source
    // spec §3.4). Widely-linear model r' = mu*s + nu*conj(s) + c,
    // solved via the normal equations on A = [s, conj(s), 1]. ---
    std::complex<double> ata[3][3] = {};
    std::complex<double> atr[3] = {};
    double sum_s2 = 0.0;   // sum |s[n]|^2, reused for sync_corr below
    double sum_r2 = 0.0;   // sum |r'[n]|^2, reused for sync_corr below
    std::complex<double> sum_s = 0.0, sum_s_sq = 0.0, sum_conj_s_sq = 0.0;
    for (long i = 0; i < fit_samples; ++i) {
        std::complex<double> si = s[size_t(i)];
        std::complex<double> ci = std::conj(si);
        sum_s += si;
        sum_s_sq += si * si;
        sum_conj_s_sq += ci * ci;
        sum_s2 += std::norm(si);
        sum_r2 += std::norm(r_prime[size_t(i)]);
        atr[0] += ci * r_prime[size_t(i)];
        atr[1] += si * r_prime[size_t(i)];
        atr[2] += r_prime[size_t(i)];
    }
    ata[0][0] = sum_s2;
    ata[0][1] = sum_conj_s_sq;
    ata[0][2] = std::conj(sum_s);
    ata[1][0] = sum_s_sq;
    ata[1][1] = sum_s2;
    ata[1][2] = sum_s;
    ata[2][0] = sum_s;
    ata[2][1] = std::conj(sum_s);
    ata[2][2] = double(fit_samples);

    std::complex<double> theta[3];
    if (!solve3x3(ata, atr, theta)) {
        fp.gated_out = true;
        fp.gate_reason = "IQ-imbalance fit singular";
        return fp;
    }
    std::complex<double> mu = theta[0], nu = theta[1], c = theta[2];
    if (std::abs(mu) < 1e-12) {
        fp.gated_out = true;
        fp.gate_reason = "degenerate mu in IQ-imbalance fit";
        return fp;
    }

    std::complex<double> q = nu / mu;
    fp.irr_db = 20.0 * std::log10(std::abs(q));
    fp.iq_eps = -2.0 * q.real();
    fp.iq_phi_deg = -2.0 * q.imag() * 180.0 / M_PI;
    fp.dc_dbc = 20.0 * std::log10(std::abs(c) / std::abs(mu));
    fp.dc_ang_deg = std::arg(c / mu) * 180.0 / M_PI;
    fp.n_samp = int(fit_samples);

    // --- Residual / EVM (source spec §3.4) ---
    double err_power = 0.0, s_power = 0.0;
    for (long i = 0; i < fit_samples; ++i) {
        std::complex<double> model = mu * s[size_t(i)] + nu * std::conj(s[size_t(i)]) + c;
        std::complex<double> e = r_prime[size_t(i)] - model;
        err_power += std::norm(e);
        s_power += std::norm(s[size_t(i)]);
    }
    fp.evm_pct = 100.0 * std::sqrt((err_power / fit_samples) / (s_power / fit_samples));

    // --- sync_corr (source spec §3.3), reusing `mu` from the fit above
    // as the channel-scalar stand-in rather than a separate plain-
    // channel fit - a deliberate simplification for this first pass. ---
    fp.sync_corr = std::min(1.0, std::abs(mu) * std::sqrt(sum_s2) / std::sqrt(sum_r2));

    // --- Fit-quality gate (thresholds calibrated from real controlled
    // TarangMini data, not the source spec's generic 10%/0.95 - see
    // LORA_EVM_CEILING_PCT's own comment in fingerprint.hpp for the
    // full story) - a high SNR alone doesn't mean this was a genuinely
    // correct (SF, BW) match: real strong ambient signal can sit behind
    // a WRONG hypothesis and still clear the SNR floor, while the fit
    // itself (evm_pct/sync_corr) reveals it doesn't actually explain
    // the captured signal. Confirmed worth adding: real captures showed
    // implausible CFO swings (thousands of ppm - no real crystal drifts
    // that much) specifically on readings with high SNR but poor
    // sync_corr/evm_pct, consistent with a cross-hypothesis false
    // match rather than a genuine preamble lock. evm_pct/sync_corr
    // stay populated even when this gate rejects (source spec's own
    // "record rejections, don't silently drop them") - the identity
    // parameters above (irr_db etc.) are still computed too, but the
    // caller should not trust them when gated_out is true regardless
    // of which specific gate caused it.
    if (fp.evm_pct > LORA_EVM_CEILING_PCT) {
        fp.gated_out = true;
        fp.gate_reason = "EVM above ceiling";
        return fp;
    }
    if (fp.sync_corr < LORA_SYNC_CORR_FLOOR) {
        fp.gated_out = true;
        fp.gate_reason = "sync_corr below floor";
        return fp;
    }

    fp.gated_out = false;
    return fp;
}

void append_fingerprint_record(const std::string& path, const std::string& time_hhmmss,
                                double freq_mhz, int sf, double bandwidth_khz,
                                const LoraFingerprint& fp) {
    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) return;

    out << "{\"t\":\"" << time_hhmmss << "\",\"proto\":\"lora\","
        << "\"rf\":{\"f_c_hz\":" << std::llround(freq_mhz * 1e6) << ",\"sf\":" << sf
        << ",\"bw_hz\":" << std::llround(bandwidth_khz * 1e3) << "},";

    if (fp.gated_out) {
        // Still record evm_pct/sync_corr/snr_db when the fit actually
        // ran (source spec's own "record rejections, don't silently
        // drop them" - a rejection caused by one of those two values
        // is meaningless to review later without them).
        out << "\"p\":null,";
        if (fp.n_samp > 0) {
            out << "\"q\":{\"snr_db\":" << num_or_null(fp.snr_db, 0.1)
                << ",\"evm_pct\":" << num_or_null(fp.evm_pct, 0.01)
                << ",\"sync_corr\":" << num_or_null(fp.sync_corr, 0.0001)
                << ",\"n_samp\":" << fp.n_samp << "},";
        }
        out << "\"flags\":[\"" << fp.gate_reason << "\"]}\n";
        return;
    }

    out << "\"q\":{\"snr_db\":" << num_or_null(fp.snr_db, 0.1)
        << ",\"evm_pct\":" << num_or_null(fp.evm_pct, 0.01)
        << ",\"sync_corr\":" << num_or_null(fp.sync_corr, 0.0001)
        << ",\"n_samp\":" << fp.n_samp << "},";
    out << "\"p\":{\"cfo_ppm\":" << num_or_null(fp.cfo_ppm, 0.001)
        << ",\"irr_db\":" << num_or_null(fp.irr_db, 0.01)
        << ",\"iq_eps\":" << num_or_null(fp.iq_eps, 0.00001)
        << ",\"iq_phi_deg\":" << num_or_null(fp.iq_phi_deg, 0.001)
        << ",\"dc_dbc\":" << num_or_null(fp.dc_dbc, 0.1)
        << ",\"dc_ang_deg\":" << num_or_null(fp.dc_ang_deg, 0.1) << "},";
    out << "\"flags\":[]}\n";
}

}  // namespace rfmon::fingerprint
