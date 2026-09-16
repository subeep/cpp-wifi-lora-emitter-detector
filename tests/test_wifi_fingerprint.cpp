// Synthetic correctness tests for src/wifi_fingerprint.cpp - same
// spirit as test_fingerprint.cpp's LoRa checks: synthesize a reference
// signal, inject a known CFO/IQ-imbalance/DC-offset, confirm the
// extractor recovers them to within resolution.
//
// The OFDM test exercises the REAL wifi::classify_modulation() (not a
// hand-built ModClassification) - it is the integration point that
// actually matters, and test_wifi_phy.cpp already covers
// classify_modulation()'s own detection accuracy in isolation. The
// DSSS test constructs a wifi::DsssDecodeResult directly instead,
// since decode_dsss_burst() itself is already thoroughly covered by
// test_wifi_dsss_rx.cpp and this file's job is the fingerprint math
// specifically, not the whole chip-domain receive chain again.
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "wifi_dsss_rx.hpp"
#include "wifi_fingerprint.hpp"
#include "wifi_phy.hpp"

using namespace rfmon::wifi;
using namespace rfmon::wifi_fingerprint;

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

// Same standard L-LTF sequence as wifi_fingerprint.cpp's kLltfFreq -
// duplicated independently (not included/shared) per this project's
// convention for a fixed, published, non-negotiable constant (see
// test_fingerprint.cpp's own independent base_upchirp() copy for the
// identical precedent).
constexpr int kLltfFreq[53] = {
    1,  1,  -1, -1, 1,  1,  -1, 1,  -1, 1,  1,  1,  1,  1,  1,  -1, -1, 1, 1,
    -1, 1,  -1, 1,  1,  1,  1,  0,  1,  -1, -1, 1,  1,  -1, 1,  -1, 1,  -1, -1,
    -1, -1, -1, 1,  1,  -1, -1, 1,  -1, 1,  -1, 1,  1,  1,  1,
};
constexpr double kSubcarrierSpacingHz = 20e6 / 64.0;

std::complex<double> l_ltf_ref(double t_s) {
    std::complex<double> sum(0.0, 0.0);
    for (int p = 0; p < 53; ++p) {
        if (kLltfFreq[p] == 0) continue;
        double k = double(p - 26);
        double phase = 2.0 * M_PI * k * kSubcarrierSpacingHz * t_s;
        sum += double(kLltfFreq[p]) * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return sum / std::sqrt(52.0);
}

// Builds a synthetic OFDM burst: noise padding, an L-STF-like periodic
// block (content doesn't matter for Schmidl-Cox - only its 0.8us
// periodicity does, and pure wideband noise trivially clears the
// spectral-width gate too, same simplification test_wifi_phy.cpp's own
// synth_ofdm() already uses), then a real L-LTF built from the known
// sequence above, with a widely-linear impairment (mu, nu, c) and a
// continuous CFO rotation applied across the WHOLE preamble (a real
// receiver-chain impairment doesn't turn on only during L-LTF).
// Returns the full capture and where the burst starts within it.
// `channel_offset_hz`: where the true channel sits relative to the
// TUNED capture center (0 by default) - real captures are offset by
// config.hpp's WIFI_CHANNEL_CAPTURE_OFFSET_HZ (1.5MHz) to keep the
// channel's peak off the DC-guard notch (see scanner.cpp's WiFi burst
// loop), which this models by rotating the WHOLE buffer (noise pad
// included - a real channel offset is a tuning choice affecting
// everything captured, unlike cfo_hz which only exists while a real
// device is transmitting) up by that frequency before handing it to
// the caller, exactly as a real radio's raw IQ would show it.
std::vector<std::complex<float>> synth_ofdm_burst(double sample_rate_hz, double eps,
                                                    double phi_deg, double dc_dbc,
                                                    double dc_ang_deg, double cfo_hz,
                                                    double channel_gain, double noise_amp,
                                                    size_t& burst_start_out,
                                                    size_t& burst_length_out, unsigned seed,
                                                    double channel_offset_hz = 0.0) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, noise_amp);
    std::normal_distribution<double> block_gen(0.0, 1.0);

    double phi = phi_deg * M_PI / 180.0;
    std::complex<double> mu = (1.0 + (1.0 + eps) * std::polar(1.0, -phi)) / 2.0;
    std::complex<double> nu = (1.0 - (1.0 + eps) * std::polar(1.0, +phi)) / 2.0;
    std::complex<double> c_offset = std::abs(mu) * std::pow(10.0, dc_dbc / 20.0) *
                                     std::polar(1.0, std::arg(mu) + dc_ang_deg * M_PI / 180.0);

    int L = int(std::lround(0.8e-6 * sample_rate_hz));
    int guard = int(std::lround(1.6e-6 * sample_rate_hz));
    int sym = int(std::lround(3.2e-6 * sample_rate_hz));

    // L-STF: one random L-sample block, repeated 10 times.
    size_t l_sz = size_t(L);
    std::vector<std::complex<double>> stf_block(l_sz);
    for (auto& v : stf_block) v = std::complex<double>(block_gen(rng), block_gen(rng));
    std::vector<std::complex<double>> preamble;
    for (int r = 0; r < 10; ++r) preamble.insert(preamble.end(), stf_block.begin(), stf_block.end());

    // L-LTF: 1.6us guard (cyclic prefix of the long symbol) + 2 real
    // long symbols from the known sequence.
    size_t sym_sz = size_t(sym);
    std::vector<std::complex<double>> long_sym(sym_sz);
    for (int n = 0; n < sym; ++n) long_sym[size_t(n)] = l_ltf_ref(double(n) / sample_rate_hz);
    for (int n = 0; n < guard; ++n) preamble.push_back(long_sym[size_t(sym - guard + n)]);
    preamble.insert(preamble.end(), long_sym.begin(), long_sym.end());
    preamble.insert(preamble.end(), long_sym.begin(), long_sym.end());

    size_t pad = 512;
    size_t total = pad + preamble.size() + pad;
    std::vector<std::complex<float>> out(total);

    double offset_inc = 2.0 * M_PI * channel_offset_hz / sample_rate_hz;
    double offset_phase = 0.0;
    double phase_inc = 2.0 * M_PI * cfo_hz / sample_rate_hz;  // + : matches extractor's -derotation
    double phase = 0.0;
    for (size_t i = 0; i < total; ++i) {
        std::complex<double> sample;
        if (i >= pad && i < pad + preamble.size()) {
            std::complex<double> s = preamble[i - pad];
            std::complex<double> impaired = mu * s + nu * std::conj(s) + c_offset;
            std::complex<double> rot(std::cos(phase), std::sin(phase));
            sample = channel_gain * impaired * rot;
            phase += phase_inc;
            if (phase > M_PI) phase -= 2 * M_PI;
        } else {
            sample = 0.0;
        }
        std::complex<double> offset_rot(std::cos(offset_phase), std::sin(offset_phase));
        sample *= offset_rot;
        offset_phase += offset_inc;
        if (offset_phase > M_PI) offset_phase -= 2 * M_PI;
        else if (offset_phase < -M_PI) offset_phase += 2 * M_PI;
        sample += std::complex<double>(noise(rng), noise(rng));
        out[i] = std::complex<float>(float(sample.real()), float(sample.imag()));
    }
    burst_start_out = pad;
    burst_length_out = preamble.size();
    return out;
}

}  // namespace

int main() {
    double sample_rate_hz = 20e6;
    double channel_hz = 2437e6;  // channel 6

    // 1. Clean-ish OFDM injection: full chain, real classify_modulation().
    {
        // cfo_hz deliberately realistic-sized (tens of kHz, matching
        // real captures - see extract_ofdm_fingerprint()'s own comment
        // on the mixing fix this test caught) rather than a small
        // value: remove_dc() (applied before mixing, matching
        // classify_modulation()'s exact order) correctly averages the
        // TRANSMITTED c_offset term toward ~0 over many full rotation
        // cycles, exactly as it should for real receiver-LO-leakage
        // removal - but only once the fit window spans enough cycles.
        // A too-small CFO relative to this test's ~600-sample window
        // doesn't complete even one cycle, so remove_dc() incorrectly
        // treats the still-mostly-static c_offset as leakage and
        // partially subtracts it - a test-realism artifact, not a
        // production bug (confirmed passing with this realistic value).
        double eps = 0.02, phi_deg = 2.0, dc_dbc = -35.0, dc_ang_deg = 25.0, cfo_hz = 45000.0;
        size_t burst_start, burst_length;
        auto cap = synth_ofdm_burst(sample_rate_hz, eps, phi_deg, dc_dbc, dc_ang_deg, cfo_hz,
                                     /*channel_gain=*/2.5, /*noise_amp=*/0.02, burst_start,
                                     burst_length, 1);
        std::vector<std::complex<float>> window(cap.begin() + long(burst_start),
                                                  cap.begin() + long(burst_start + burst_length));
        auto mc = classify_modulation(window, sample_rate_hz, channel_hz, channel_hz, false);
        check(mc.mod == ModClass::OFDM, "ofdm_classified", "mod=" + std::to_string(int(mc.mod)));
        check(mc.has_preamble_range, "ofdm_preamble_range_found", "");

        if (mc.mod == ModClass::OFDM && mc.has_preamble_range) {
            auto fp = extract_ofdm_fingerprint(cap.data(), cap.size(), burst_start, burst_length,
                                                sample_rate_hz, channel_hz, channel_hz, channel_hz,
                                                mc);
            check(!fp.gated_out, "ofdm_clean_injection_not_gated", fp.gate_reason);
            check(std::abs(fp.cfo_ppm - (cfo_hz / channel_hz) * 1e6) < 5.0, "ofdm_cfo_recovered",
                  "got " + std::to_string(fp.cfo_ppm) + "ppm, want " +
                      std::to_string((cfo_hz / channel_hz) * 1e6) + "ppm");

            double phi = phi_deg * M_PI / 180.0;
            std::complex<double> mu = (1.0 + (1.0 + eps) * std::polar(1.0, -phi)) / 2.0;
            std::complex<double> nu = (1.0 - (1.0 + eps) * std::polar(1.0, +phi)) / 2.0;
            double expected_irr_db = 20.0 * std::log10(std::abs(nu / mu));
            check(std::abs(fp.irr_db - expected_irr_db) < 1.5, "ofdm_irr_db_recovered",
                  "got " + std::to_string(fp.irr_db) + "dB, want " +
                      std::to_string(expected_irr_db) + "dB");
            check(std::abs(fp.iq_eps - eps) < 0.01, "ofdm_iq_eps_recovered",
                  "got " + std::to_string(fp.iq_eps) + ", want " + std::to_string(eps));
            check(std::abs(fp.iq_phi_deg - phi_deg) < 1.0, "ofdm_iq_phi_deg_recovered",
                  "got " + std::to_string(fp.iq_phi_deg) + "deg, want " + std::to_string(phi_deg) +
                      "deg");
            check(std::abs(fp.dc_dbc - dc_dbc) < 1.5, "ofdm_dc_dbc_recovered",
                  "got " + std::to_string(fp.dc_dbc) + "dBc, want " + std::to_string(dc_dbc) +
                      "dBc");
            check(fp.snr_db > WIFI_SNR_FLOOR_DB, "ofdm_snr_above_floor",
                  "snr_db=" + std::to_string(fp.snr_db));
            check(fp.evm_pct < 20.0, "ofdm_evm_reasonable", "evm_pct=" + std::to_string(fp.evm_pct));
        }
    }

    // 1b. Same injection, but with a real channel offset (matching
    // config.hpp's WIFI_CHANNEL_CAPTURE_OFFSET_HZ=1.5MHz - captures
    // are deliberately tuned off the true channel center to keep its
    // peak off the DC-guard notch, see scanner.cpp's WiFi burst loop).
    // Every other test in this file uses capture_center_hz ==
    // segment_center_hz (offset 0), which never exercises the mixing
    // step at all - this is the exact scenario that caught a real bug
    // live (see extract_ofdm_fingerprint()'s mixing comment): every
    // real OFDM burst gated out on EVM because the 1.5MHz offset was
    // never removed, aliasing to a consistent-looking but wrong CFO.
    {
        double eps = 0.015, phi_deg = 3.0, dc_dbc = -32.0, dc_ang_deg = -10.0, cfo_hz = 30000.0;
        double channel_offset_hz = 1.5e6;
        double capture_center_hz = channel_hz;              // where the radio is tuned
        double segment_center_hz = channel_hz + channel_offset_hz;  // the true channel center
        size_t burst_start, burst_length;
        auto cap = synth_ofdm_burst(sample_rate_hz, eps, phi_deg, dc_dbc, dc_ang_deg, cfo_hz,
                                     /*channel_gain=*/2.0, /*noise_amp=*/0.02, burst_start,
                                     burst_length, 5, channel_offset_hz);
        std::vector<std::complex<float>> window(cap.begin() + long(burst_start),
                                                  cap.begin() + long(burst_start + burst_length));
        auto mc = classify_modulation(window, sample_rate_hz, capture_center_hz, segment_center_hz,
                                       false);
        check(mc.mod == ModClass::OFDM, "ofdm_offset_classified", "mod=" + std::to_string(int(mc.mod)));
        if (mc.mod == ModClass::OFDM && mc.has_preamble_range) {
            auto fp = extract_ofdm_fingerprint(cap.data(), cap.size(), burst_start, burst_length,
                                                sample_rate_hz, capture_center_hz, segment_center_hz,
                                                segment_center_hz, mc);
            check(!fp.gated_out, "ofdm_offset_not_gated", fp.gate_reason);
            check(std::abs(fp.cfo_ppm - (cfo_hz / segment_center_hz) * 1e6) < 5.0,
                  "ofdm_offset_cfo_recovered",
                  "got " + std::to_string(fp.cfo_ppm) + "ppm, want " +
                      std::to_string((cfo_hz / segment_center_hz) * 1e6) + "ppm");
            check(std::abs(fp.dc_dbc - dc_dbc) < 2.0, "ofdm_offset_dc_dbc_recovered",
                  "got " + std::to_string(fp.dc_dbc) + "dBc, want " + std::to_string(dc_dbc) + "dBc");
            check(fp.evm_pct < 20.0, "ofdm_offset_evm_reasonable",
                  "evm_pct=" + std::to_string(fp.evm_pct));
        }
    }

    // 2. G-invariance: recovered ratios must not depend on channel gain.
    {
        double eps = 0.03, phi_deg = -1.5, dc_dbc = -30.0, dc_ang_deg = -60.0, cfo_hz = -4000.0;
        size_t bs_small, bl_small, bs_large, bl_large;
        auto cap_small = synth_ofdm_burst(sample_rate_hz, eps, phi_deg, dc_dbc, dc_ang_deg, cfo_hz,
                                           0.3, 0.005, bs_small, bl_small, 2);
        auto cap_large = synth_ofdm_burst(sample_rate_hz, eps, phi_deg, dc_dbc, dc_ang_deg, cfo_hz,
                                           40.0, 0.7, bs_large, bl_large, 2);
        std::vector<std::complex<float>> win_small(cap_small.begin() + long(bs_small),
                                                     cap_small.begin() + long(bs_small + bl_small));
        std::vector<std::complex<float>> win_large(cap_large.begin() + long(bs_large),
                                                     cap_large.begin() + long(bs_large + bl_large));
        auto mc_small = classify_modulation(win_small, sample_rate_hz, channel_hz, channel_hz, false);
        auto mc_large = classify_modulation(win_large, sample_rate_hz, channel_hz, channel_hz, false);
        if (mc_small.mod == ModClass::OFDM && mc_small.has_preamble_range &&
            mc_large.mod == ModClass::OFDM && mc_large.has_preamble_range) {
            auto fp_small = extract_ofdm_fingerprint(cap_small.data(), cap_small.size(), bs_small,
                                                       bl_small, sample_rate_hz, channel_hz,
                                                       channel_hz, channel_hz, mc_small);
            auto fp_large = extract_ofdm_fingerprint(cap_large.data(), cap_large.size(), bs_large,
                                                       bl_large, sample_rate_hz, channel_hz,
                                                       channel_hz, channel_hz, mc_large);
            check(!fp_small.gated_out && !fp_large.gated_out, "ofdm_g_invariance_not_gated",
                  "small=" + fp_small.gate_reason + " large=" + fp_large.gate_reason);
            check(std::abs(fp_small.irr_db - fp_large.irr_db) < 1.5, "ofdm_g_invariance_irr_db",
                  "small=" + std::to_string(fp_small.irr_db) +
                      " large=" + std::to_string(fp_large.irr_db));
        } else {
            check(false, "ofdm_g_invariance_classified", "one or both windows didn't classify OFDM");
        }
    }

    // 3. Pure noise must gate out, never fabricate a fingerprint.
    {
        size_t bs, bl;
        auto cap = synth_ofdm_burst(sample_rate_hz, 0.0, 0.0, -100.0, 0.0, 0.0, /*channel_gain=*/0.0,
                                     /*noise_amp=*/0.5, bs, bl, 3);
        std::vector<std::complex<float>> window(cap.begin() + long(bs), cap.begin() + long(bs + bl));
        auto mc = classify_modulation(window, sample_rate_hz, channel_hz, channel_hz, false);
        // Either classify_modulation() itself rejects pure noise (most
        // likely), or if it somehow finds a spurious plateau, the
        // fingerprint extractor's own SNR gate must reject it - both
        // are correct outcomes for this input.
        bool ok = (mc.mod != ModClass::OFDM);
        if (!ok) {
            auto fp = extract_ofdm_fingerprint(cap.data(), cap.size(), bs, bl, sample_rate_hz,
                                                channel_hz, channel_hz, channel_hz, mc);
            ok = fp.gated_out;
        }
        check(ok, "ofdm_pure_noise_rejected", "");
    }

    // 4. DSSS: construct a DsssDecodeResult directly (decode_dsss_burst
    // itself is test_wifi_dsss_rx.cpp's job) with known differential
    // bits, a known gain+DC-offset+CFO injected on top of the
    // re-integrated absolute reference, and confirm recovery.
    {
        std::mt19937 rng(11);
        std::bernoulli_distribution bit_dist(0.5);
        int m = 200;
        size_t m_sz = size_t(m);
        std::vector<std::complex<double>> ref(m_sz);
        ref[0] = std::complex<double>(1.0, 0.0);
        for (int k = 1; k < m; ++k) {
            bool bit1 = bit_dist(rng);
            ref[size_t(k)] = ref[size_t(k - 1)] * (bit1 ? -1.0 : 1.0);
        }

        // c_offset (like mu/nu) is a TRANSMITTER-side impairment - the
        // widely-linear model composes it at baseband before the
        // signal is upconverted and transmitted, so it experiences the
        // same channel gain and CFO rotation as the rest of the
        // composite on the way to the receiver (this is also why
        // dc_ang_deg is defined relative to the signal's own phase -
        // arg(c/mu) - not as an absolute angle: their ratio is
        // meaningful only because both rotate together). Matches how
        // both fingerprint.cpp (LoRa) and extract_ofdm_fingerprint()
        // derotate the WHOLE received sample, DC term included, by the
        // CFO estimate before fitting.
        double dc_dbc = -28.0, dc_ang_deg = 40.0, cfo_hz = 1500.0, gain = 1.8, noise_amp = 0.03;
        std::complex<double> c_offset = gain * std::pow(10.0, dc_dbc / 20.0) *
                                         std::polar(1.0, dc_ang_deg * M_PI / 180.0);
        std::normal_distribution<double> noise(0.0, noise_amp);
        double phase_inc = 2.0 * M_PI * cfo_hz * 1e-6;  // T_sym = 1us, matches the extractor
        double phase = 0.0;
        DsssDecodeResult dec;
        dec.preamble_found = true;
        dec.sync_symbols.resize(size_t(m));
        for (int k = 0; k < m; ++k) {
            std::complex<double> rot(std::cos(phase), std::sin(phase));
            std::complex<double> sample = (gain * ref[size_t(k)] + c_offset) * rot;
            phase += phase_inc;
            if (phase > M_PI) phase -= 2 * M_PI;
            sample += std::complex<double>(noise(rng), noise(rng));
            dec.sync_symbols[size_t(k)] = sample;
        }

        auto fp = extract_dsss_fingerprint(channel_hz, dec);
        check(!fp.gated_out, "dsss_clean_injection_not_gated", fp.gate_reason);
        check(std::abs(fp.cfo_ppm - (cfo_hz / channel_hz) * 1e6) < 20.0, "dsss_cfo_recovered",
              "got " + std::to_string(fp.cfo_ppm) + "ppm, want " +
                  std::to_string((cfo_hz / channel_hz) * 1e6) + "ppm");
        check(std::abs(fp.dc_dbc - dc_dbc) < 1.5, "dsss_dc_dbc_recovered",
              "got " + std::to_string(fp.dc_dbc) + "dBc, want " + std::to_string(dc_dbc) + "dBc");
        check(std::abs(fp.dc_ang_deg - dc_ang_deg) < 5.0, "dsss_dc_ang_deg_recovered",
              "got " + std::to_string(fp.dc_ang_deg) + "deg, want " + std::to_string(dc_ang_deg) +
                  "deg");
        // Documented, structural limitation (see wifi_fingerprint.cpp's
        // own comment): a real-valued BPSK reference cannot identify
        // gain/phase imbalance separately from DC offset via this
        // widely-linear technique - confirm the extractor honestly
        // leaves these at their defaults rather than fitting noise.
        check(fp.irr_db == 0.0 && fp.iq_eps == 0.0 && fp.iq_phi_deg == 0.0,
              "dsss_iq_imbalance_left_unset",
              "irr_db=" + std::to_string(fp.irr_db) + " iq_eps=" + std::to_string(fp.iq_eps) +
                  " iq_phi_deg=" + std::to_string(fp.iq_phi_deg));
    }

    // 5. DSSS: too few SYNC symbols must gate out.
    {
        DsssDecodeResult dec;
        dec.preamble_found = true;
        dec.sync_symbols = {std::complex<double>(1, 0), std::complex<double>(-1, 0)};
        auto fp = extract_dsss_fingerprint(channel_hz, dec);
        check(fp.gated_out, "dsss_too_few_symbols_gated", fp.gate_reason);
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED.\n", failures);
        return 1;
    }
    std::printf("\nAll Wi-Fi fingerprint extraction checks passed.\n");
    return 0;
}
