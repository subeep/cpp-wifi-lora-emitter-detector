#include "wifi_fingerprint.hpp"

#include <algorithm>
#include <cmath>

namespace rfmon::wifi_fingerprint {

namespace {

// Fixed number of raw pre-burst samples used for the OFDM noise-power
// measurement - not tied to L-STF length or sample rate, unlike LoRa's
// exactly-one-symbol noise window, because a WiFi burst window is
// sliced tightly to the detected energy edges (wifi_phy.cpp's
// detect_bursts(), ~1us block resolution) with no guaranteed multiple
// of any one WiFi timing constant available before it. 256 samples is
// enough for a stable power estimate at any of this project's capture
// rates (20-56 Msps) without assuming a specific duration.
constexpr size_t kNoiseWindowSamples = 256;

// Solves the fixed 3x3 complex linear system A*x = b by Gaussian
// elimination with partial pivoting - same algorithm as
// fingerprint.cpp's solve3x3(), reimplemented independently here per
// this file's own header (separate engine, not a shared call).
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

// Solves the reduced 2x2 complex system for a REAL-valued reference
// (see extract_dsss_fingerprint()'s own comment for why DSSS needs
// this instead of the full 3x3 widely-linear fit): r' = A*s + c, s
// real, A and c complex.
bool solve2x2(std::complex<double> a[2][2], std::complex<double> b[2], std::complex<double> x[2]) {
    for (int col = 0; col < 2; ++col) {
        int pivot = col;
        double best = std::abs(a[col][col]);
        for (int r = col + 1; r < 2; ++r) {
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
        for (int r = col + 1; r < 2; ++r) {
            std::complex<double> factor = a[r][col] / a[col][col];
            for (int c = col; c < 2; ++c) a[r][c] -= factor * a[col][c];
            b[r] -= factor * b[col];
        }
    }
    for (int r = 1; r >= 0; --r) {
        std::complex<double> sum = b[r];
        for (int c = r + 1; c < 2; ++c) sum -= a[r][c] * x[c];
        x[r] = sum / a[r][r];
    }
    return true;
}

// Standard IEEE 802.11a/g L-LTF frequency-domain training sequence,
// subcarriers -26..+26 (53 entries, index p maps to subcarrier k=p-26;
// p=26 is DC, always 0). Public, standard, checkable against any
// published 802.11 PHY reference (e.g. IEEE 802.11-2016 Section
// 17.3.5.10) - the same class of fixed, standard constant as this
// project's existing Barker-11 chip sequence (wifi_phy.cpp) or LoRa's
// SYNC_WORD_DEFAULT (lora_phy.hpp).
constexpr int kLltfFreq[53] = {
    1,  1,  -1, -1, 1,  1,  -1, 1,  -1, 1,  1,  1,  1,  1,  1,  -1, -1, 1, 1,
    -1, 1,  -1, 1,  1,  1,  1,  0,  1,  -1, -1, 1,  1,  -1, 1,  -1, 1,  -1, -1,
    -1, -1, -1, 1,  1,  -1, -1, 1,  -1, 1,  -1, 1,  1,  1,  1,
};
constexpr double kSubcarrierSpacingHz = 20e6 / 64.0;  // 312.5kHz, fixed regardless of our own Fs
constexpr int kActiveSubcarriers = 52;

// Evaluates the L-LTF's known waveform at time `t_s` seconds relative
// to its own start - a direct sum over the 52 active subcarriers
// rather than an FFT, so this works at ANY sample_rate_hz (this
// project's two device profiles disagree: the X310 here lands on
// exactly 20 Msps, the B210 on 56 Msps, neither a clean ratio of the
// other) without assuming a specific FFT size or resampling first.
std::complex<double> l_ltf_reference(double t_s) {
    std::complex<double> sum(0.0, 0.0);
    for (int p = 0; p < 53; ++p) {
        if (kLltfFreq[p] == 0) continue;  // DC
        double k = double(p - 26);
        double phase = 2.0 * M_PI * k * kSubcarrierSpacingHz * t_s;
        sum += double(kLltfFreq[p]) * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return sum / std::sqrt(double(kActiveSubcarriers));
}

}  // namespace

WifiFingerprint extract_ofdm_fingerprint(const std::complex<float>* cap, size_t cap_len,
                                          size_t burst_start, size_t burst_length,
                                          double sample_rate_hz, double capture_center_hz,
                                          double segment_center_hz, double channel_center_hz,
                                          const wifi::ModClassification& mc) {
    (void)capture_center_hz;
    (void)segment_center_hz;
    WifiFingerprint fp;

    if (mc.mod != wifi::ModClass::OFDM || !mc.has_preamble_range || mc.l_ltf_length == 0) {
        fp.gated_out = true;
        fp.gate_reason = "no L-LTF range available";
        return fp;
    }
    if (sample_rate_hz <= 0.0 || burst_start + burst_length > cap_len) {
        fp.gated_out = true;
        fp.gate_reason = "invalid burst window";
        return fp;
    }

    size_t abs_l_stf_start = burst_start + mc.l_stf_start;
    size_t abs_l_ltf_start = burst_start + mc.l_ltf_start;
    size_t abs_l_ltf_end = abs_l_ltf_start + mc.l_ltf_length;
    if (abs_l_stf_start >= abs_l_ltf_start || abs_l_ltf_end > cap_len || mc.l_ltf_length < 8) {
        fp.gated_out = true;
        fp.gate_reason = "degenerate preamble range";
        return fp;
    }
    if (abs_l_stf_start < kNoiseWindowSamples) {
        fp.gated_out = true;
        fp.gate_reason = "no pre-burst noise window available";
        return fp;
    }

    // --- Mix down to baseband over the whole range this function
    // needs, in ONE call so phase stays coherent across it (see
    // wifi_phy.hpp's mix_to_baseband() comment). This is NOT optional:
    // WiFi channels are captured WIFI_CHANNEL_CAPTURE_OFFSET_HZ off
    // the true channel center (config.hpp - keeps the channel's peak
    // off the DC-guard notch), and classify_modulation() removed that
    // offset internally before ever measuring mc.l_stf_start/
    // cfo_coarse_hz - operating on raw `cap` here without repeating
    // that same mix left a 1.5MHz offset in every sample, which is
    // WAY outside either CFO estimator's unambiguous range (+/-625kHz
    // coarse, +/-156.25kHz fine) and aliases to a plausible-looking
    // but wrong small ppm value while destroying the fit entirely -
    // confirmed live: every real OFDM burst gated out on EVM (94-157%,
    // ceiling 80%) with a suspiciously narrow, consistent ~19-20ppm
    // CFO cluster across many different real access points, the
    // signature of a systematic alignment bug, not per-device
    // variation or real multipath noise.
    //
    // The absolute phase reference this mix starts at does not need to
    // match classify_modulation()'s own (different start position) -
    // every quantity this function reports (cfo via phase
    // differences; irr_db/iq_eps/iq_phi_deg/dc_ang_deg via nu/mu and
    // c/mu ratios) is invariant to a constant phase rotation applied
    // to the whole signal, so only RELATIVE phase coherence within
    // this one mix matters.
    // DC/receiver-leakage bias is estimated from the pre-burst NOISE
    // samples only, not wifi::remove_dc()'s whole-buffer mean (which
    // classify_modulation() uses because it has no clean noise-only
    // segment available in the buffers it's handed). The burst region
    // carries the widely-linear model's own transmitted `c` term,
    // which rotates with CFO and only averages toward zero over many
    // full rotation cycles - blending it into the leakage estimate
    // biases dc_dbc/dc_ang_deg by however much of a cycle the burst
    // window happens to span (confirmed: a synthetic dc_dbc recovery
    // improved but did not fully converge as injected CFO increased,
    // the signature of exactly this averaging-incompleteness, not a
    // fixed error). The noise window has no such content at all, so
    // its own mean is a clean, CFO-independent leakage estimate.
    size_t mix_start = abs_l_stf_start - kNoiseWindowSamples;
    size_t mix_len = abs_l_ltf_end - mix_start;
    std::complex<double> dc_bias(0.0, 0.0);
    for (size_t i = 0; i < kNoiseWindowSamples; ++i) {
        dc_bias += std::complex<double>(cap[mix_start + i].real(), cap[mix_start + i].imag());
    }
    dc_bias /= double(kNoiseWindowSamples);
    std::complex<float> dc_bias_f(float(dc_bias.real()), float(dc_bias.imag()));
    std::vector<std::complex<float>> raw_slice(mix_len);
    for (size_t i = 0; i < mix_len; ++i) raw_slice[i] = cap[mix_start + i] - dc_bias_f;
    std::vector<std::complex<float>> mixed = wifi::mix_to_baseband(
        raw_slice, sample_rate_hz, segment_center_hz - capture_center_hz);
    size_t local_l_stf_start = abs_l_stf_start - mix_start;
    size_t local_l_ltf_start = abs_l_ltf_start - mix_start;
    size_t local_l_ltf_end = abs_l_ltf_end - mix_start;

    // --- SNR gate (mirrors fingerprint.cpp's identical reasoning: a
    // low-SNR packet doesn't produce a slightly-worse parameter, it
    // produces a wrong one, so this rejects before any fitting runs). --
    double noise_power = 0.0;
    for (size_t i = 0; i < local_l_stf_start; ++i) {
        noise_power +=
            double(mixed[i].real()) * mixed[i].real() + double(mixed[i].imag()) * mixed[i].imag();
    }
    noise_power /= double(local_l_stf_start);

    double burst_power = 0.0;
    size_t preamble_samples = local_l_ltf_end - local_l_stf_start;
    for (size_t i = local_l_stf_start; i < local_l_ltf_end; ++i) {
        burst_power +=
            double(mixed[i].real()) * mixed[i].real() + double(mixed[i].imag()) * mixed[i].imag();
    }
    burst_power /= double(preamble_samples);

    if (noise_power <= 0.0 || burst_power <= noise_power) {
        fp.gated_out = true;
        fp.gate_reason = "SNR non-positive";
        return fp;
    }
    fp.snr_db = 10.0 * std::log10((burst_power - noise_power) / noise_power);
    if (fp.snr_db < WIFI_SNR_FLOOR_DB) {
        fp.gated_out = true;
        fp.gate_reason = "below SNR floor";
        return fp;
    }

    // --- Fine CFO: derotate by the coarse (Schmidl-Cox) estimate
    // first, then a Moose-style delay-and-conjugate over the L-LTF's
    // own two 3.2us long symbols - the textbook two-stage OFDM CFO
    // estimator. The longer L-LTF lag gives a finer but narrower
    // (+/-156.25kHz) unambiguous range than the coarse L-STF-based
    // estimate (+/-625kHz), so coarse correction first is what keeps
    // the residual the fine stage needs to resolve inside that range -
    // same architecture as fingerprint.cpp's coarse-then-residual CFO
    // refinement for LoRa, applied to a genuinely different preamble
    // structure here. ---
    size_t lag = mc.l_ltf_length / 2;
    if (lag < 4) {
        fp.gated_out = true;
        fp.gate_reason = "L-LTF too short for fine CFO";
        return fp;
    }
    double coarse_phase_inc = -2.0 * M_PI * mc.cfo_coarse_hz / sample_rate_hz;
    std::complex<double> p2(0.0, 0.0);
    double phase = 0.0;
    std::vector<std::complex<double>> derot(2 * lag);
    for (size_t n = 0; n < 2 * lag; ++n) {
        std::complex<double> x(mixed[local_l_ltf_start + n].real(),
                                mixed[local_l_ltf_start + n].imag());
        std::complex<double> rot(std::cos(phase), std::sin(phase));
        derot[n] = x * rot;
        phase += coarse_phase_inc;
        if (phase > M_PI) phase -= 2.0 * M_PI;
        else if (phase < -M_PI) phase += 2.0 * M_PI;
    }
    for (size_t n = 0; n < lag; ++n) p2 += std::conj(derot[n]) * derot[n + lag];
    double cfo_fine_hz = 0.0;
    if (std::abs(p2) > 1e-12) {
        cfo_fine_hz = std::arg(p2) * sample_rate_hz / (2.0 * M_PI * double(lag));
    }
    double cfo_total_hz = mc.cfo_coarse_hz + cfo_fine_hz;
    fp.cfo_ppm = (cfo_total_hz / channel_center_hz) * 1e6;

    // --- Build the known L-LTF reference at this capture's own sample
    // rate, and the fully-derotated (coarse+fine) received L-LTF
    // segment, unit-RMS-normalized by its own measured power. ---
    size_t fit_len = 2 * lag;
    double g = std::sqrt(burst_power);
    double fine_phase_inc = -2.0 * M_PI * cfo_fine_hz / sample_rate_hz;
    std::vector<std::complex<double>> r_prime(fit_len), s(fit_len);
    double fphase = 0.0;
    for (size_t n = 0; n < fit_len; ++n) {
        std::complex<double> rot(std::cos(fphase), std::sin(fphase));
        r_prime[n] = (derot[n] / g) * rot;
        fphase += fine_phase_inc;
        if (fphase > M_PI) fphase -= 2.0 * M_PI;
        else if (fphase < -M_PI) fphase += 2.0 * M_PI;
        s[n] = l_ltf_reference(double(n) / sample_rate_hz);
    }

    // --- I/Q imbalance + DC offset: widely-linear fit, identical
    // formula to fingerprint.cpp's (r' = mu*s + nu*conj(s) + c), a
    // genuinely well-posed 3-parameter system here because s (unlike
    // DSSS's real BPSK reference below) is complex-valued - see
    // l_ltf_reference()'s own comment. ---
    std::complex<double> ata[3][3] = {};
    std::complex<double> atr[3] = {};
    double sum_s2 = 0.0, sum_r2 = 0.0;
    std::complex<double> sum_s = 0.0, sum_s_sq = 0.0, sum_conj_s_sq = 0.0;
    for (size_t n = 0; n < fit_len; ++n) {
        std::complex<double> si = s[n];
        std::complex<double> ci = std::conj(si);
        sum_s += si;
        sum_s_sq += si * si;
        sum_conj_s_sq += ci * ci;
        sum_s2 += std::norm(si);
        sum_r2 += std::norm(r_prime[n]);
        atr[0] += ci * r_prime[n];
        atr[1] += si * r_prime[n];
        atr[2] += r_prime[n];
    }
    ata[0][0] = sum_s2;
    ata[0][1] = sum_conj_s_sq;
    ata[0][2] = std::conj(sum_s);
    ata[1][0] = sum_s_sq;
    ata[1][1] = sum_s2;
    ata[1][2] = sum_s;
    ata[2][0] = sum_s;
    ata[2][1] = std::conj(sum_s);
    ata[2][2] = double(fit_len);

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
    fp.n_samp = int(fit_len);

    double err_power = 0.0;
    for (size_t n = 0; n < fit_len; ++n) {
        std::complex<double> model = mu * s[n] + nu * std::conj(s[n]) + c;
        err_power += std::norm(r_prime[n] - model);
    }
    fp.evm_pct = 100.0 * std::sqrt((err_power / double(fit_len)) / (sum_s2 / double(fit_len)));
    fp.sync_corr = std::min(1.0, std::abs(mu) * std::sqrt(sum_s2) / std::sqrt(sum_r2));

    if (fp.evm_pct > WIFI_EVM_CEILING_PCT) {
        fp.gated_out = true;
        fp.gate_reason = "EVM above ceiling";
        return fp;
    }
    if (fp.sync_corr < WIFI_SYNC_CORR_FLOOR) {
        fp.gated_out = true;
        fp.gate_reason = "sync_corr below floor";
        return fp;
    }

    fp.gated_out = false;
    return fp;
}

WifiFingerprint extract_dsss_fingerprint(double channel_center_hz,
                                          const wifi::DsssDecodeResult& dec) {
    WifiFingerprint fp;
    if (!dec.preamble_found || dec.sync_symbols.size() < 16) {
        fp.gated_out = true;
        fp.gate_reason = "insufficient SYNC symbols";
        return fp;
    }
    const auto& syms = dec.sync_symbols;
    size_t m = syms.size();

    // --- snr_db proxy: magnitude consistency of the despread symbols
    // themselves (mean^2/variance) - see this function's declaration
    // in wifi_fingerprint.hpp for why this differs from the OFDM/LoRa
    // pre-burst-noise-power SNR. ---
    double mean_mag = 0.0;
    std::vector<double> mags(m);
    for (size_t k = 0; k < m; ++k) {
        mags[k] = std::abs(syms[k]);
        mean_mag += mags[k];
    }
    mean_mag /= double(m);
    double var_mag = 0.0;
    for (double v : mags) var_mag += (v - mean_mag) * (v - mean_mag);
    var_mag /= double(m);
    if (mean_mag <= 0.0 || var_mag <= 0.0) {
        fp.gated_out = true;
        fp.gate_reason = "SNR non-positive";
        return fp;
    }
    fp.snr_db = 10.0 * std::log10((mean_mag * mean_mag) / var_mag);
    if (fp.snr_db < WIFI_SNR_FLOOR_DB) {
        fp.gated_out = true;
        fp.gate_reason = "below SNR floor";
        return fp;
    }

    // --- Re-integrate DBPSK's differential encoding to recover an
    // absolute (up to one unknown constant phase, which the fit's own
    // complex `A` below absorbs) BPSK reference - see
    // wifi_dsss_rx.hpp's DsssDecodeResult::sync_symbols comment for why
    // this needs no knowledge of the scrambler state. Same one-line
    // differential-decision formula wifi_dsss_rx.cpp uses for
    // raw_bits, applied here independently (not re-imported) per this
    // file's separate-engine convention. ---
    std::vector<std::complex<double>> ref(m);
    ref[0] = std::complex<double>(1.0, 0.0);
    for (size_t k = 1; k < m; ++k) {
        double d = (syms[k] * std::conj(syms[k - 1])).real();
        ref[k] = ref[k - 1] * (d < 0.0 ? -1.0 : 1.0);
    }

    // --- CFO: align every symbol to the common reference phase (undo
    // the DBPSK modulation itself, not just decode it), then the SAME
    // delay-and-conjugate differential estimator LoRa/OFDM both use -
    // valid here because `aligned[k]` now carries ONLY the CFO's phase
    // drift, exactly like a repeated-identical-symbol preamble would.
    // T_sym is a fixed 1us regardless of the original capture rate -
    // wifi_dsss_rx.cpp always resamples to its own 22 Msps chip grid
    // first (11 chips/symbol * 2 samples/chip / 22 Msps = 1e-6s). ---
    constexpr double kSymPeriodS = 1e-6;
    std::vector<std::complex<double>> aligned(m);
    for (size_t k = 0; k < m; ++k) aligned[k] = syms[k] * ref[k];  // ref[k] real +-1

    std::complex<double> diff_sum(0.0, 0.0);
    for (size_t k = 0; k + 1 < m; ++k) diff_sum += aligned[k + 1] * std::conj(aligned[k]);
    double cfo_hz = 0.0;
    if (std::abs(diff_sum) > 1e-12) cfo_hz = std::arg(diff_sum) / (2.0 * M_PI * kSymPeriodS);
    fp.cfo_ppm = (cfo_hz / channel_center_hz) * 1e6;

    // --- Derotate by the CFO estimate, unit-RMS-normalize. ---
    double g = mean_mag;
    double phase_inc = -2.0 * M_PI * cfo_hz * kSymPeriodS;
    double phase = 0.0;
    std::vector<std::complex<double>> r_prime(m);
    for (size_t k = 0; k < m; ++k) {
        std::complex<double> rot(std::cos(phase), std::sin(phase));
        r_prime[k] = (syms[k] / g) * rot;
        phase += phase_inc;
        if (phase > M_PI) phase -= 2.0 * M_PI;
        else if (phase < -M_PI) phase += 2.0 * M_PI;
    }

    // --- DC offset only: `ref` is purely real (+-1), so conj(ref)==ref
    // identically for every sample - the widely-linear model's `s` and
    // `conj(s)` columns collapse to the SAME vector, making a 3x3 fit
    // singular by construction. This is not a bug to work around: gain
    // and phase imbalance are fundamentally unidentifiable against a
    // real-valued reference constellation by this technique (this is
    // exactly why QPSK/OFDM references can resolve IQ imbalance and a
    // BPSK one structurally cannot) - a genuinely complex DSSS
    // reference (e.g. the raw CHIP-domain Barker pattern rather than
    // the despread symbol domain) could recover this in future work,
    // out of scope here. irr_db/iq_eps/iq_phi_deg are left at their
    // struct defaults (0.0) rather than fit to a meaningless number.
    // The reduced model r' = A*s + c (A complex, s real) still
    // identifies DC offset cleanly. ---
    std::complex<double> ata[2][2] = {};
    std::complex<double> atr[2] = {};
    double sum_s2 = 0.0, sum_r2 = 0.0;
    std::complex<double> sum_s = 0.0;
    for (size_t k = 0; k < m; ++k) {
        double si = ref[k].real();  // purely real by construction
        sum_s += si;
        sum_s2 += si * si;
        sum_r2 += std::norm(r_prime[k]);
        atr[0] += si * r_prime[k];
        atr[1] += r_prime[k];
    }
    ata[0][0] = sum_s2;
    ata[0][1] = sum_s;
    ata[1][0] = std::conj(sum_s);
    ata[1][1] = double(m);

    std::complex<double> theta[2];
    if (!solve2x2(ata, atr, theta)) {
        fp.gated_out = true;
        fp.gate_reason = "DC-offset fit singular";
        return fp;
    }
    std::complex<double> A = theta[0], c = theta[1];
    if (std::abs(A) < 1e-12) {
        fp.gated_out = true;
        fp.gate_reason = "degenerate gain in DC-offset fit";
        return fp;
    }
    fp.dc_dbc = 20.0 * std::log10(std::abs(c) / std::abs(A));
    fp.dc_ang_deg = std::arg(c / A) * 180.0 / M_PI;
    fp.n_samp = int(m);

    double err_power = 0.0;
    for (size_t k = 0; k < m; ++k) {
        std::complex<double> model = A * ref[k].real() + c;
        err_power += std::norm(r_prime[k] - model);
    }
    fp.evm_pct = 100.0 * std::sqrt((err_power / double(m)) / (sum_s2 / double(m)));
    fp.sync_corr = std::min(1.0, std::abs(A) * std::sqrt(sum_s2) / std::sqrt(sum_r2));

    if (fp.evm_pct > WIFI_EVM_CEILING_PCT) {
        fp.gated_out = true;
        fp.gate_reason = "EVM above ceiling";
        return fp;
    }
    if (fp.sync_corr < WIFI_SYNC_CORR_FLOOR) {
        fp.gated_out = true;
        fp.gate_reason = "sync_corr below floor";
        return fp;
    }

    fp.gated_out = false;
    return fp;
}

}  // namespace rfmon::wifi_fingerprint
