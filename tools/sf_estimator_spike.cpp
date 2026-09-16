// Standalone validation spike for a proposed SF/BW estimator - see
// discussion in the "Emitter to Packet" write-up. Does NOT touch or
// replace any production code (lora_phy.*, scanner.cpp): it links
// against the existing decode/spectral functions read-only, as a
// ground-truth oracle, and implements ONE new idea in this file only.
//
// The idea: instead of blind-searching all 24 (BW, SF) hypotheses,
// measure two things directly from the raw, un-decimated capture and
// solve for SF algebraically:
//
//   1. Symbol period T_sym, via autocorrelation of the raw IQ at the
//      lag corresponding to each candidate (SF, BW)'s symbol duration.
//      LoRa's preamble is dozens of *identical* back-to-back upchirps,
//      so the true T_sym is the lag with the strongest self-similarity
//      - this needs no assumption about SF or BW to measure.
//
//   2. Occupied bandwidth, via the SAME spectral segmentation the
//      production code already runs on this same capture for the
//      Active-emitters table (spectrogram_max_db + find_segments).
//
//   SF = round( log2( T_sym_samples * BW_hz / Fs_capture ) )
//
// Ground truth for comparison: the existing lora::detect_burst() run
// across the full 24-hypothesis grid, unmodified, on the SAME buffer.
//
// Usage: ./sf_estimator_spike [n_captures]  (default 8, ~2.1s each)
//
// VALIDATION RESULT (live X310 + TarangMini, driven through 5 known
// SF/BW settings via tools/tarangmini_sf_bw_sweep.py's serial config
// API as ground truth): 31% overall accuracy against real bursts
// (11/36). The BW estimate (spectral) is solid - confirmed correct
// every time a real burst was present. The autocorrelation-based T_sym
// estimate has a real, reproducible bug: it sometimes locks onto HALF
// the true symbol period instead of the true one - 100% consistently
// wrong (same wrong half-period lag every time) across all 8 real
// SF12/BW125 bursts tested, a signature of a genuine secondary
// correlation ridge in a linear chirp's self-ambiguity function, not
// random noise. Where it DID lock onto the true period, it correctly
// broke the SF/BW aliasing ambiguity (6/7 correct on the SF9/BW250
// case, whose alias SF7/BW125 shares an identical chirp rate) -
// proving the core two-independent-measurements idea is sound. NOT
// wired into production; needs a harmonic-consistency check (a true
// period should also correlate strongly at 2x/3x lag, unlike a
// spurious half-period artifact) and re-validation before it should
// replace the brute-force search in scanner.cpp.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <vector>

#include "classifier.hpp"
#include "config.hpp"
#include "detector.hpp"
#include "lora_phy.hpp"
#include "lora_phy_std.hpp"
#include "sdr_capture.hpp"
#include "spectrum.hpp"

using namespace rfmon;

namespace {

// ---------------------------------------------------------------------
// New estimator #1: symbol period via preamble autocorrelation.
// Runs at the RAW capture rate, before any BW-specific decimation, so
// it is not tied to a (SF, BW) hypothesis at all.
// ---------------------------------------------------------------------
struct PeriodEstimate {
    long lag_samples = 0;
    double score = 0.0;  // normalized |correlation|, 0..1
};

PeriodEstimate estimate_symbol_period(const std::vector<std::complex<float>>& iq,
                                       double fs_capture) {
    // Candidate lags: every (SF, BW) in the production search grid,
    // expressed in RAW samples. Two different (SF, BW) pairs never
    // give the same lag (an alias pair's symbol duration doubles each
    // step, even though their chirp *rate* stays equal) - see the
    // write-up for why this is what breaks the aliasing ambiguity.
    std::vector<long> candidate_lags;
    for (double bw : LORA_LISTEN_BW_LIST_HZ) {
        for (int sf : LORA_LISTEN_SF_LIST) {
            double t_sym = double(1 << sf) / bw;
            long lag = std::lround(t_sym * fs_capture);
            if (lag > 0) candidate_lags.push_back(lag);
        }
    }
    std::sort(candidate_lags.begin(), candidate_lags.end());
    candidate_lags.erase(std::unique(candidate_lags.begin(), candidate_lags.end()),
                          candidate_lags.end());

    size_t n = iq.size();
    // Search the first third of the capture - production's own
    // find_preamble() also searches from the start, and a real
    // preamble is dozens of symbols, far more than needed here.
    size_t search_span = n / 3;

    PeriodEstimate best;
    for (long lag : candidate_lags) {
        if (size_t(lag) * 3 >= n) continue;  // need room for several periods
        std::complex<double> num(0.0, 0.0);
        double denom_a = 0.0, denom_b = 0.0;
        size_t count = 0;
        for (size_t i = 0; i + size_t(lag) < search_span; i += 8) {
            std::complex<double> a(iq[i].real(), iq[i].imag());
            std::complex<double> b(iq[i + size_t(lag)].real(), iq[i + size_t(lag)].imag());
            num += a * std::conj(b);
            denom_a += std::norm(a);
            denom_b += std::norm(b);
            ++count;
        }
        if (count < 20) continue;
        double denom = std::sqrt(denom_a * denom_b);
        double score = denom > 1e-12 ? std::abs(num) / denom : 0.0;
        if (score > best.score) {
            best.score = score;
            best.lag_samples = lag;
        }
    }
    return best;
}

// ---------------------------------------------------------------------
// New estimator #2: occupied bandwidth via the EXISTING spectral
// pipeline (spectrum.cpp + detector.cpp), reused read-only - this is
// the same code path run_lora_listen_step() already calls to feed the
// Active-emitters table on this same capture, just surfaced here
// instead of only being used for that table.
// ---------------------------------------------------------------------
double estimate_bandwidth_hz(const std::vector<std::complex<float>>& iq, double fs_capture,
                              double tuned_center_hz) {
    Spectrum spec = spectrogram_max_db(iq, fs_capture);
    double guard_hz = std::max(fs_capture * DC_GUARD_FRACTION, DC_GUARD_MIN_HZ);
    std::vector<bool> dc_mask = mask_dc_guard(spec.freqs_offset_hz, guard_hz);
    std::vector<bool> edge_mask = mask_edge_guard(spec.freqs_offset_hz, fs_capture, EDGE_GUARD_FRACTION);
    std::vector<Segment> segments =
        find_segments(spec.freqs_offset_hz, spec.psd_db, tuned_center_hz, edge_mask, dc_mask,
                      NOISE_FLOOR_PERCENTILE, /*threshold_db=*/12.0, MIN_SEGMENT_BINS,
                      MERGE_GAP_BINS, /*hysteresis_low_db=*/12.0);
    if (segments.empty()) return 0.0;
    // Widest segment near the tuned center - the burst, not noise.
    const Segment* widest = &segments.front();
    for (const auto& s : segments)
        if (s.bandwidth_hz > widest->bandwidth_hz) widest = &s;
    return widest->bandwidth_hz;
}

// Snap a raw bandwidth estimate to the nearest of the 3 real LoRa BW
// options - the spectral estimate is continuous-valued, but TarangMini
// only ever transmits at one of these three.
double snap_bw(double bw_hz) {
    double best = LORA_LISTEN_BW_LIST_HZ.front();
    double best_d = 1e18;
    for (double b : LORA_LISTEN_BW_LIST_HZ) {
        double d = std::abs(bw_hz - b);
        if (d < best_d) { best_d = d; best = b; }
    }
    return best;
}

int solve_sf(long lag_samples, double bw_hz, double fs_capture) {
    double n_effective = double(lag_samples) * bw_hz / fs_capture;  // should land near 2^SF
    if (n_effective <= 0) return -1;
    return int(std::lround(std::log2(n_effective)));
}

// ---------------------------------------------------------------------
// Ground truth: run the EXISTING, unmodified production search
// (lora::detect_burst across all 24 hypotheses) on the same buffer.
// Preamble-only, not header/CRC - the header checksum is just 5 bits
// and false-accepts ~1/32 of the time, so preamble lock (6 consistent
// symbols, ratio > 0.5) is the more trustworthy ground truth here.
// ---------------------------------------------------------------------
struct Hit {
    double bw_hz;
    int sf;
    int preamble_len;
    int cfo_bins;
};

std::vector<Hit> run_production_search(const std::vector<std::complex<float>>& iq_raw,
                                        double fs_capture) {
    std::vector<Hit> hits;
    for (double bw : LORA_LISTEN_BW_LIST_HZ) {
        int decim = std::max(1, int(std::lround(fs_capture / bw)));
        std::vector<std::complex<float>> iq;
        iq.reserve(iq_raw.size() / size_t(decim) + 1);
        for (size_t i = 0; i + size_t(decim) <= iq_raw.size(); i += size_t(decim)) {
            std::complex<float> sum(0.0f, 0.0f);
            for (int j = 0; j < decim; ++j) sum += iq_raw[i + size_t(j)];
            iq.push_back(sum / float(decim));
        }
        for (int sf : LORA_LISTEN_SF_LIST) {
            auto burst = lora::detect_burst(iq, sf);
            if (burst.has_value() && burst->preamble_len >= 6) {
                hits.push_back({bw, sf, burst->preamble_len, burst->cfo_bins});
            }
        }
    }
    return hits;
}

}  // namespace

int main(int argc, char** argv) {
    int n_captures = (argc > 1) ? std::atoi(argv[1]) : 8;

    DeviceProfile profile = device_profile(SdrDeviceType::X310);
    std::printf("Connecting to X310 (%s, antenna %s)...\n", profile.device_args.c_str(),
                profile.antenna.c_str());
    // X310 RFNoC bring-up is documented elsewhere in this project as
    // intermittently flaky right after a previous process released the
    // link - same retry shape as Scanner::connect_sdr() (scanner.cpp).
    std::unique_ptr<UsrpCapture> sdr;
    for (int attempt = 0; attempt < 6; ++attempt) {
        try {
            sdr = std::make_unique<UsrpCapture>(profile.antenna, profile.default_gain_db, 0,
                                                 profile.device_args);
            break;
        } catch (const std::exception& e) {
            std::printf("  connect attempt %d/6 failed: %s\n", attempt + 1, e.what());
            sdr.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    }
    if (!sdr) {
        std::fprintf(stderr, "could not connect to the X310 after 6 attempts\n");
        return 1;
    }
    std::printf("Connected. Capturing %d x %.1fs at 866.9 MHz, %.0f kHz.\n\n", n_captures,
                LORA_LISTEN_DURATION_S, profile.lora_listen_capture_rate_hz / 1e3);

    int agree = 0, disagree = 0, no_ground_truth = 0, no_estimate = 0;

    for (int c = 0; c < n_captures; ++c) {
        auto [iq, actual_rate, overflow] =
            sdr->capture(866.9e6, profile.lora_listen_capture_rate_hz, LORA_LISTEN_DURATION_S);
        std::time_t t_end = std::time(nullptr);
        char tbuf[16];
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", std::localtime(&t_end));
        std::printf("=== capture %d/%d [ends %s] ===  (%zu samples @ %.0f Hz%s)\n", c + 1, n_captures,
                    tbuf, iq.size(), actual_rate, overflow ? ", OVERFLOW" : "");
        if (iq.empty()) {
            std::printf("  empty capture, skip\n\n");
            continue;
        }

        PeriodEstimate period = estimate_symbol_period(iq, actual_rate);
        double bw_raw = estimate_bandwidth_hz(iq, actual_rate, 866.9e6);
        double bw_snapped = snap_bw(bw_raw);
        int sf_est = (period.score > 0.0) ? solve_sf(period.lag_samples, bw_snapped, actual_rate) : -1;

        std::printf("  autocorr: lag=%ld samples, score=%.3f -> T_sym=%.1f us\n", period.lag_samples,
                    period.score, period.lag_samples / actual_rate * 1e6);
        std::printf("  spectral BW: raw=%.1f kHz -> snapped=%.0f kHz\n", bw_raw / 1e3, bw_snapped / 1e3);
        if (sf_est >= 5 && sf_est <= 12) {
            std::printf("  ESTIMATE: BW=%.0fk SF%d  (single calculation, no search)\n",
                        bw_snapped / 1e3, sf_est);
        } else {
            std::printf("  ESTIMATE: none (score too low or SF out of [5,12] range)\n");
        }

        auto hits = run_production_search(iq, actual_rate);
        if (hits.empty()) {
            std::printf("  [production ground truth] no preamble lock at any of 24 hypotheses\n");
            if (sf_est >= 5 && sf_est <= 12) no_ground_truth++;
        } else {
            std::printf("  [production ground truth] %zu hypothes(es) locked:\n", hits.size());
            bool match = false;
            for (const auto& h : hits) {
                bool is_match = (sf_est == h.sf && std::abs(bw_snapped - h.bw_hz) < 1.0);
                if (is_match) match = true;
                std::printf("      BW=%.0fk SF%-2d preamble_len=%-3d cfo_bins=%-4d%s\n", h.bw_hz / 1e3,
                            h.sf, h.preamble_len, h.cfo_bins, is_match ? "   <-- matches estimate" : "");
            }
            if (sf_est < 5 || sf_est > 12) {
                no_estimate++;
                std::printf("  RESULT: no estimate produced, but production locked something\n");
            } else if (match) {
                agree++;
                std::printf("  RESULT: AGREE\n");
            } else {
                disagree++;
                std::printf("  RESULT: DISAGREE\n");
            }
        }
        std::printf("\n");
    }

    std::printf("=== summary over %d captures ===\n", n_captures);
    std::printf("  agree:            %d\n", agree);
    std::printf("  disagree:         %d\n", disagree);
    std::printf("  estimate but no production lock: %d\n", no_ground_truth);
    std::printf("  production locked, no estimate:  %d\n", no_estimate);
    return 0;
}
