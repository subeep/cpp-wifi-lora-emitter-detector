// Whole-frame RF features of one FCS-verified Wi-Fi frame (RF fingerprint
// plan, phase A: docs/WIFI_RF_FINGERPRINT_PHASE_A_2026-11-04.md).
//
// A frame that passed its FCS has known transmitted symbols, so every
// feature is fitted data-aided over the whole frame instead of the 8 us
// preamble the older extractor (wifi_fingerprint.hpp) uses. Live captures
// showed the preamble-only CFO at 0.3-0.6 ppm per frame and the whole-frame
// one at 0.015-0.05 ppm on the same transmitters.
//
// Pure numerics: the decoders supply symbols, channel and known points.
#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace rfmon::wifi {

struct FrameRfFeatures {
    std::string method;                    // "OFDM data-aided" / "DSSS data-aided"
    int symbols = 0;                       // symbols the fit used
    double cfo_hz = 0, cfo_sigma_hz = 0;   // carrier offset vs the nominal channel at the receiver, 1 sigma
    // OFDM: the carrier offset over the same stretch of every frame - the
    // training field plus the first 16 data symbols - so short and long
    // frames compare like with like. cfo_hz minus this is the frequency
    // settling during the frame (frames of 40+ symbols).
    double cfo_early_hz = NAN, cfo_early_sigma_hz = NAN, settling_hz = NAN;
    double sfo_ppm = NAN, sfo_sigma_ppm = NAN;  // sampling-clock offset (OFDM); equals cfo in ppm when one crystal drives both
    double phase_jitter_deg = NAN;         // rms of each symbol's common phase about the fitted line
    double lo_leak_dbc = NAN;              // transmitter carrier leakage on the DC tone (OFDM)
    double iq_image_db = NAN;              // transmitter I/Q image relative to the signal (OFDM)
    double snr_db = NAN;                   // data-aided, after every fitted correction
};

// OFDM. For each data symbol n: y[n][i] the raw FFT bin of tones[i] (after
// the preamble CFO correction, before any phase correction), x[n][i] the
// known transmitted point (0 for an unused tone such as DC), window[n] the
// FFT window start in samples at fs. h[i] is the channel estimate on
// tones[i] from the training field. `preamble_cfo_hz` is the correction
// already applied; the result's cfo_hz includes it. The channel estimate
// defines zero phase at the training field's centre, `anchor` samples at
// fs, worth `anchor_weight` symbols (2 for the averaged L-LTF). The early
// CFO always uses it; the whole-frame CFO only for frames under 20 symbols.
FrameRfFeatures ofdm_features(const std::vector<std::vector<std::complex<float>>>& y,
                              const std::vector<std::vector<std::complex<float>>>& x,
                              const std::vector<std::complex<float>>& h, const std::vector<int>& tones,
                              const std::vector<double>& window, int fft_size, double fs, double preamble_cfo_hz,
                              double anchor, double anchor_weight);

// DSSS: despread symbols (one per bit time) and each symbol's known
// polarity (+1 / -1) from the verified bits. No CFO correction was applied
// before despreading, so the phase slope is the whole offset.
FrameRfFeatures dsss_features(const std::vector<std::complex<double>>& despread, const std::vector<int8_t>& polarity,
                              double symbol_rate_hz);

}  // namespace rfmon::wifi
