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

// Linear-interpolating resample to an arbitrary target rate. Used only
// to put the Barker correlator on an exact whole-samples-per-chip grid
// (see BARKER_SAMPLES_PER_CHIP) - not a general-purpose resampler, and
// deliberately not anti-alias filtered, since its one caller is
// matched-filtering a spread signal whose bandwidth it is not reducing.
std::vector<std::complex<float>> resample_linear(const std::vector<std::complex<float>>& in,
                                                  double in_rate, double out_rate) {
    if (in.empty() || in_rate <= 0.0 || out_rate <= 0.0) return in;
    double ratio = in_rate / out_rate;
    size_t n_out = size_t(double(in.size()) / ratio);
    if (n_out == 0) return {};
    std::vector<std::complex<float>> out(n_out);
    for (size_t i = 0; i < n_out; ++i) {
        double pos = double(i) * ratio;
        size_t i0 = size_t(pos);
        if (i0 + 1 >= in.size()) {
            out[i] = in.back();
            continue;
        }
        float frac = float(pos - double(i0));
        out[i] = in[i0] * (1.0f - frac) + in[i0 + 1] * frac;
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

// Receiver LO leakage sits at exactly the tuned centre - i.e. DC of the
// raw capture, before any mixing. config.hpp documents it as a 40-50dB
// spike that set_rx_dc_offset()/set_rx_iq_balance() do not remove.
// Subtracting the complex mean kills it at the one point in the chain
// where its position is known exactly; this MUST run before
// mix_to_baseband(), after which the spike is no longer at DC and a
// mean is no longer the right tool.
//
// Why this matters more than it looks: an unmodulated carrier is
// perfectly self-similar at every lag, so it drives the Schmidl-Cox
// metric below to a sustained ~0.7 - above the plateau threshold - for
// the entire buffer. Before this existed, the classifier reported OFDM
// on receiver self-noise with HIGHER confidence than on real OFDM
// (measured: 3.17 vs 2.56), which inverted the whole decision.
std::vector<std::complex<float>> remove_dc(const std::vector<std::complex<float>>& iq) {
    if (iq.empty()) return iq;
    std::complex<double> mean(0.0, 0.0);
    for (const auto& s : iq) mean += std::complex<double>(s.real(), s.imag());
    mean /= double(iq.size());
    std::complex<float> m(float(mean.real()), float(mean.imag()));
    std::vector<std::complex<float>> out(iq.size());
    for (size_t i = 0; i < iq.size(); ++i) out[i] = iq[i] - m;
    return out;
}

// Fraction of the spectrum SPANNED by bins within `within_db` of the
// peak - outermost-above-threshold minus innermost, not a count. This
// is the discriminator a plateau threshold fundamentally cannot
// provide: a bare tone and a genuine L-STF produce the SAME
// autocorrelation plateau, and only their spectra tell them apart.
//
// Span, deliberately, not the fraction of bins occupied: an L-STF is
// spectrally SPARSE by construction - 12 active subcarriers of 52,
// spaced 4 apart, which is precisely what creates its 0.8us
// periodicity - so it lights up a comb, not a filled band. Counting
// occupied bins rejects real OFDM (measured: 0.06 of bins at 20 Msps,
// under any useful threshold). What actually separates a preamble from
// a carrier is that the comb is spread across the whole channel while a
// carrier sits in one place, so the outermost extent is the right
// measure - the same "tolerant of interior dips" reasoning that
// estimate_occupied_bandwidth_hz() below is built on.
//
// Hann-windowed deliberately - a rectangular window smears a pure tone
// across enough bins (sinc sidelobes) to blur the very distinction
// being measured here.
double occupied_bin_fraction(const std::complex<float>* x, size_t n, double within_db) {
    constexpr int kFft = 256;
    if (n < size_t(kFft)) return 0.0;

    size_t fft_sz = size_t(kFft);
    std::vector<kiss_fft_cpx> in(fft_sz), out(fft_sz);
    for (int i = 0; i < kFft; ++i) {
        double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * double(i) / double(kFft - 1)));
        in[size_t(i)].r = float(double(x[size_t(i)].real()) * w);
        in[size_t(i)].i = float(double(x[size_t(i)].imag()) * w);
    }
    kiss_fft_cfg cfg = kiss_fft_alloc(kFft, 0, nullptr, nullptr);
    kiss_fft(cfg, in.data(), out.data());
    kiss_fft_free(cfg);

    // Reorder to monotonic frequency so "outermost" means outermost in
    // frequency, not in raw FFT bin order.
    std::vector<float> mag2(fft_sz);
    for (int i = 0; i < kFft; ++i) {
        int shifted = (i + kFft / 2) % kFft;
        float re = out[size_t(i)].r, im = out[size_t(i)].i;
        mag2[size_t(shifted)] = re * re + im * im;
    }
    float peak = *std::max_element(mag2.begin(), mag2.end());
    if (peak <= 0.0f) return 0.0;

    float thresh = peak * float(std::pow(10.0, within_db / 10.0));
    int lo = 0, hi = kFft - 1;
    while (lo < kFft && mag2[size_t(lo)] < thresh) ++lo;
    while (hi >= 0 && mag2[size_t(hi)] < thresh) --hi;
    if (lo > hi) return 0.0;
    return double(hi - lo + 1) / double(kFft);
}

// --- Barker-11 / DSSS-CCK correlator ---------------------------------

constexpr double BARKER_CHIP_RATE_HZ = 11e6;
constexpr float BARKER_CHIPS[11] = {+1, -1, +1, +1, -1, +1, +1, +1, -1, -1, -1};
constexpr double BARKER_PEAK_RATIO_THRESHOLD = 0.5;
constexpr int MIN_BARKER_CONSISTENT_PEAKS = 16;  // consecutive 1us bit periods

// The correlator runs on a resampled copy at EXACTLY this many samples
// per chip, rather than on whatever rate the radio happened to give us.
//
// This matters more than it sounds. The template below lays chips out
// as chip = (i * 11) / template_len, which is only exact when
// template_len is a whole multiple of 11 - i.e. when the rate is a
// whole multiple of the 11 Mchip/s Barker rate. At the X310's real
// 20 Msps that gave 1.818 samples/chip, so chip boundaries were
// quantized to sample boundaries with up to half a sample of timing
// error each, and the "matched" filter was not matched at all.
//
// The original tests could not see this: they generated their DSSS
// waveform with this same zero-order-hold formula, so template and
// signal shared the identical distortion and correlated perfectly.
// Against a properly pulse-shaped waveform, detection measured 1/40
// even at 30dB SNR. Resampling to 2.0 samples/chip makes the layout
// exact (22 samples = 11 chips x 2) and decouples the correlator from
// the capture rate entirely - it now behaves the same on the X310's
// 20 Msps and the B210's 56 Msps.
constexpr double BARKER_SAMPLES_PER_CHIP = 2.0;

// Evidence for a Barker peak train. `confidence` is the MEAN normalized
// correlation over the longest qualifying run - inherently bounded to
// [0,1] because it is a normalized correlation, which is what makes it
// directly comparable against the OFDM branch's mean plateau metric.
//
// This deliberately replaced an earlier best_run/MIN_..._PEAKS ratio
// that was unbounded above: a longer burst scored arbitrarily higher,
// so "highest confidence wins" across sub-captures was really selecting
// the longest signal rather than the most likely modulation, and the
// two branches' numbers were not on a comparable scale at all.
struct PeakTrainEvidence {
    double confidence = 0.0;  // mean normalized correlation over the run, [0,1]
    int length = 0;           // run length in symbols
};

// Longest run, anywhere in `baseband`, of consecutive periodic peaks
// spaced one Barker-symbol (11 chips = 1us) apart that each clear
// BARKER_PEAK_RATIO_THRESHOLD - the "clean, periodic peak train" a
// real 802.11b DSSS/CCK preamble produces (matched-filter magnitude is
// insensitive to the DBPSK data bit's sign, so this fires regardless
// of the actual preamble bit values).
//
// Note this branch needs no carrier-rejection guard of its own: the
// Barker chips sum to +1 out of 11, so an unmodulated tone correlates
// at only ~1/11 = 0.09, far under BARKER_PEAK_RATIO_THRESHOLD. The
// matched filter rejects carriers structurally, which is exactly what
// the OFDM autocorrelator cannot do.
PeakTrainEvidence barker_evidence(const std::vector<std::complex<float>>& baseband,
                                   double sample_rate_hz) {
    PeakTrainEvidence best;
    int template_len = int(std::lround(sample_rate_hz / 1e6));  // 1 Barker symbol = 1us
    if (template_len < 3 || baseband.size() < size_t(template_len) * (MIN_BARKER_CONSISTENT_PEAKS + 1)) {
        return best;
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
    // of consecutive one-symbol-spaced positions clearing threshold,
    // and carry that run's mean correlation along as the confidence.
    for (int phase = 0; phase < template_len; ++phase) {
        int run = 0;
        double run_sum = 0.0;
        for (size_t n = size_t(phase); n < n_positions; n += size_t(template_len)) {
            if (ratio[n] >= BARKER_PEAK_RATIO_THRESHOLD) {
                ++run;
                run_sum += double(ratio[n]);
                if (run >= MIN_BARKER_CONSISTENT_PEAKS && run > best.length) {
                    best.length = run;
                    best.confidence = std::min(1.0, run_sum / double(run));
                }
            } else {
                run = 0;
                run_sum = 0.0;
            }
        }
    }
    return best;
}

// --- Schmidl-Cox / OFDM correlator -----------------------------------

constexpr double SC_SHORT_SYMBOL_S = 0.8e-6;  // one 802.11 L-STF short symbol
// The plateau metric settles at roughly (rho/(rho+1))^2 for SNR rho, so
// this threshold IS a hard sensitivity floor: 0.6 puts it at 5.4dB, and
// the stress matrix showed OFDM detection collapsing from 40/40 at 15dB
// to 0/40 at 5dB, exactly there. 0.35 moves the floor to ~1.6dB.
//
// Lowering it is safe now, and was not before, because rejecting
// carriers no longer depends on this number at all - that job belongs to
// MAX_SC_PLATEAU_SYMBOLS (bounded vs unbounded plateau) and
// MIN_OFDM_BIN_FRACTION (spread vs single-bin spectrum). Verified
// against the false-positive battery in tests/test_wifi_stress.cpp
// rather than assumed.
constexpr double SC_PLATEAU_THRESHOLD = 0.35;
constexpr int MIN_SC_PLATEAU_SYMBOLS = 4;  // consecutive short-symbol repeats

// THE carrier-rejection guard. A real L-STF is exactly 10 short
// symbols, so its Schmidl-Cox plateau runs about (10-2)=8 short-symbol
// periods and then STOPS - the L-LTF that follows has a different
// (3.2us) structure, which breaks the 0.8us repetition. An unmodulated
// carrier is self-similar at every lag forever, so its plateau is
// bounded only by the capture length: at 20 Msps a 20ms buffer yields a
// run of ~25,000 short-symbol periods.
//
// That difference - bounded vs unbounded - is the only thing in the
// time domain that separates the two, since the plateau VALUE is ~0.7-1
// for both. Allowing 20 symbols leaves generous headroom over the
// physical 8 for timing jitter, channel spread and back-to-back
// preambles, while still rejecting a carrier by three orders of
// magnitude.
constexpr int MAX_SC_PLATEAU_SYMBOLS = 20;

// Spectral-width gate for the OFDM branch (see occupied_bin_fraction()).
// A 20MHz 802.11 burst lights up most of a 256-bin FFT of its own
// channel; an unmodulated carrier lights up one or two bins, i.e. a
// fraction under ~0.01. 0.20 sits far from both, so it does not need to
// be tuned precisely to separate them.
constexpr double MIN_OFDM_BIN_FRACTION = 0.20;
constexpr double OCCUPIED_BIN_WITHIN_DB = -10.0;

// Evidence for an L-STF plateau. `confidence` is the MEAN plateau
// metric over the run - naturally bounded to [0,1], matching
// PeakTrainEvidence so the two branches are finally on one scale.
struct PlateauEvidence {
    double confidence = 0.0;
    size_t start = 0;  // where the plateau began, for the spectral check
    int length = 0;    // plateau length in samples
};

// Longest run, anywhere in `baseband`, of consecutive samples where
// M(d) = |P(d)|^2 / R(d)^2 clears SC_PLATEAU_THRESHOLD - the sustained
// near-1.0 plateau a repeated OFDM short training symbol produces.
// P/R use the standard incremental sliding-window recurrence so this
// stays O(N) rather than re-summing an L-sample window per position.
// Runs longer than MAX_SC_PLATEAU_SYMBOLS are discarded outright, not
// truncated - an over-long plateau is positive evidence of a carrier,
// not weak evidence of a preamble.
PlateauEvidence schmidl_cox_evidence(const std::vector<std::complex<float>>& baseband,
                                      double sample_rate_hz) {
    PlateauEvidence best;
    int L = int(std::lround(SC_SHORT_SYMBOL_S * sample_rate_hz));
    if (L < 3 || baseband.size() < size_t(2 * L) + size_t(MIN_SC_PLATEAU_SYMBOLS * L)) {
        return best;
    }

    size_t n_d = baseband.size() - size_t(2 * L);
    std::complex<float> P(0.0f, 0.0f);
    float R = 0.0f;
    for (int k = 0; k < L; ++k) {
        P += std::conj(baseband[size_t(k)]) * baseband[size_t(k + L)];
        float m = std::abs(baseband[size_t(k + L)]);
        R += m * m;
    }

    const int min_len = MIN_SC_PLATEAU_SYMBOLS * L;
    const int max_len = MAX_SC_PLATEAU_SYMBOLS * L;
    int run = 0;
    size_t run_start = 0;
    double run_sum = 0.0;

    auto close_run = [&]() {
        if (run >= min_len && run <= max_len) {
            double mean_m = run_sum / double(run);
            if (mean_m > best.confidence) {
                best.confidence = std::min(1.0, mean_m);
                best.start = run_start;
                best.length = run;
            }
        }
        run = 0;
        run_sum = 0.0;
    };

    for (size_t d = 0; d < n_d; ++d) {
        float p_mag2 = P.real() * P.real() + P.imag() * P.imag();
        double m_metric = double(p_mag2) / (double(R) * double(R) + 1e-12);
        if (m_metric >= SC_PLATEAU_THRESHOLD) {
            if (run == 0) run_start = d;
            ++run;
            run_sum += std::min(1.0, m_metric);
        } else {
            close_run();
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
    close_run();
    return best;
}

}  // namespace

std::vector<BurstWindow> detect_bursts(const std::complex<float>* x, size_t n,
                                        double sample_rate_hz, double threshold_db,
                                        size_t max_bursts) {
    std::vector<BurstWindow> out;
    if (n == 0 || sample_rate_hz <= 0.0 || max_bursts == 0) return out;

    // ~1us blocks. Block-averaged rather than a true sliding window:
    // this only has to find burst EDGES to within a microsecond, and a
    // sliding window over 20M samples would cost more than the
    // correlators this is meant to be cheaper than.
    size_t block = size_t(std::max(8.0, std::round(sample_rate_hz * 1e-6)));
    size_t n_blocks = n / block;
    if (n_blocks < 16) return out;

    std::vector<float> env(n_blocks);
    for (size_t i = 0; i < n_blocks; ++i) {
        double sum = 0.0;
        const std::complex<float>* p = x + i * block;
        for (size_t k = 0; k < block; ++k) {
            sum += double(p[k].real()) * p[k].real() + double(p[k].imag()) * p[k].imag();
        }
        env[i] = float(sum / double(block));
    }

    // 25th percentile as the noise floor - a mean or median would be
    // dragged upward by the traffic itself on a busy channel, which is
    // exactly when this needs to stay honest.
    std::vector<float> sorted(env);
    size_t q = sorted.size() / 4;
    std::nth_element(sorted.begin(), sorted.begin() + long(q), sorted.end());
    double noise = double(sorted[q]);
    if (!(noise > 0.0)) noise = 1e-20;
    double thresh = noise * std::pow(10.0, threshold_db / 10.0);

    // The shortest thing worth calling a frame. Also the floor below
    // which classify_modulation() cannot work anyway: Barker needs
    // template_len*(MIN_BARKER_CONSISTENT_PEAKS+1) samples and the
    // spectral gate needs 256, so anything briefer would only ever
    // return Unknown.
    const size_t min_blocks = 20;  // ~20us
    const size_t merge_gap_blocks = 2;  // bridge brief mid-frame dips

    size_t i = 0;
    while (i < n_blocks && out.size() < max_bursts) {
        if (double(env[i]) < thresh) {
            ++i;
            continue;
        }
        size_t start = i;
        size_t last_hot = i;
        while (i < n_blocks && (double(env[i]) >= thresh || i - last_hot <= merge_gap_blocks)) {
            if (double(env[i]) >= thresh) last_hot = i;
            ++i;
        }
        size_t len_blocks = last_hot - start + 1;
        if (len_blocks >= min_blocks) {
            double peak = 0.0;
            for (size_t b = start; b <= last_hot; ++b) peak = std::max(peak, double(env[b]));
            BurstWindow w;
            w.start = start * block;
            w.length = len_blocks * block;
            if (w.start + w.length > n) w.length = n - w.start;
            w.peak_db = 10.0 * std::log10(peak + 1e-20);
            out.push_back(w);
        }
    }
    return out;
}

std::vector<BeaconSource> find_beacon_sources(const std::vector<double>& burst_starts_s,
                                               const std::vector<double>& burst_power_db,
                                               double tolerance_s, int min_repeats) {
    std::vector<BeaconSource> sources;
    size_t n = std::min(burst_starts_s.size(), burst_power_db.size());
    if (n < size_t(min_repeats)) return sources;

    // Work on a time-ordered index so trains can be chained forward.
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return burst_starts_s[a] < burst_starts_s[b]; });

    std::vector<bool> used(n, false);
    for (size_t oi = 0; oi < n; ++oi) {
        size_t seed = order[oi];
        if (used[seed]) continue;

        // Chain forward from this burst, stepping one beacon interval at
        // a time. A missed beacon (collision with another BSS, or a
        // capture edge) must not end the train, so allow the next hit to
        // land an integer number of intervals ahead rather than exactly
        // one - otherwise a single lost frame splits one real source
        // into two phantom ones.
        std::vector<size_t> train{seed};
        double last_t = burst_starts_s[seed];
        double sum_power = burst_power_db[seed];
        for (size_t oj = oi + 1; oj < n; ++oj) {
            size_t cand = order[oj];
            if (used[cand]) continue;
            double dt = burst_starts_s[cand] - last_t;
            if (dt <= 0.0) continue;
            double intervals = dt / BEACON_INTERVAL_S;
            double nearest = std::round(intervals);
            if (nearest < 1.0) continue;
            // Only bridge a couple of missed beacons; beyond that the
            // match is more likely coincidence than the same train.
            if (nearest > 3.0) continue;
            if (std::abs(dt - nearest * BEACON_INTERVAL_S) > tolerance_s * nearest) continue;
            train.push_back(cand);
            sum_power += burst_power_db[cand];
            last_t = burst_starts_s[cand];
        }

        if (int(train.size()) < min_repeats) continue;

        // Chaining alone is far too weak a test. With busy traffic,
        // random bursts frequently land close enough to an interval
        // multiple to chain by luck - 60 aperiodic bursts in one second
        // produced SEVEN phantom trains before this check existed,
        // which on a real channel would invent emitters outright.
        //
        // A genuine beacon train is EVENLY spaced, so test that
        // directly: least-squares fit t = t0 + k*T over the train's
        // interval indices and require every member to sit close to the
        // fitted line. Coincidental chains wander and fail this even
        // when each individual step was within tolerance.
        double t0_ref = burst_starts_s[train.front()];
        std::vector<double> ks;
        ks.reserve(train.size());
        for (size_t idx : train) {
            ks.push_back(std::round((burst_starts_s[idx] - t0_ref) / BEACON_INTERVAL_S));
        }
        double m = double(train.size());
        double sum_k = 0.0, sum_t = 0.0, sum_kk = 0.0, sum_kt = 0.0;
        for (size_t j = 0; j < train.size(); ++j) {
            double k = ks[j], t = burst_starts_s[train[j]];
            sum_k += k;
            sum_t += t;
            sum_kk += k * k;
            sum_kt += k * t;
        }
        double denom = m * sum_kk - sum_k * sum_k;
        if (std::abs(denom) < 1e-12) continue;
        double period = (m * sum_kt - sum_k * sum_t) / denom;
        double intercept = (sum_t - period * sum_k) / m;

        // The fitted period must actually be a beacon interval, not an
        // arbitrary spacing that happens to fit a line.
        if (period < BEACON_INTERVAL_S * 0.98 || period > BEACON_INTERVAL_S * 1.02) continue;

        double sq_resid = 0.0;
        for (size_t j = 0; j < train.size(); ++j) {
            double pred = intercept + period * ks[j];
            double r = burst_starts_s[train[j]] - pred;
            sq_resid += r * r;
        }
        if (std::sqrt(sq_resid / m) > tolerance_s) continue;

        // Completeness: a real beacon train fills most of the slots it
        // spans, because the AP beacons on every interval. A chain
        // assembled from coincidences is sparse - it only matched where
        // random traffic happened to land, and reached its length by
        // bridging over gaps.
        //
        // Allowing up to 3 intervals per step (needed so one collided
        // beacon does not split a source in two) triples the chances of
        // a lucky match, which is what still let one phantom train
        // through 60 aperiodic bursts after the straight-line test. This
        // closes that: occupancy below 80% is not a beacon train (a real
        // train that lost one beacon of nine is still 0.89).
        double slots = (ks.back() - ks.front()) + 1.0;
        if (slots > 0.0 && m / slots < 0.8) continue;

        for (size_t idx : train) used[idx] = true;
        BeaconSource src;
        src.first_offset_s = t0_ref;
        src.burst_count = int(train.size());
        src.mean_power_db = sum_power / double(train.size());
        src.period_s = period;
        sources.push_back(src);
    }
    return sources;
}

ModClassification classify_modulation(const std::vector<std::complex<float>>& iq,
                                       double sample_rate_hz, double capture_center_hz,
                                       double segment_center_hz, bool try_dsss) {
    double offset_hz = segment_center_hz - capture_center_hz;

    // Kill receiver LO leakage first, while it is still exactly at DC -
    // see remove_dc()'s comment for why this one line is load-bearing.
    std::vector<std::complex<float>> baseband =
        mix_to_baseband(remove_dc(iq), sample_rate_hz, offset_hz);

    // Schmidl-Cox's sliding recurrence is O(N) regardless of L, so
    // there's no cost reason to decimate before running it - and not
    // decimating means L is computed from the real sample rate, closer
    // to the true 0.8us short-symbol duration and less sensitive to
    // rounding than a smaller, decimated L would be.
    PlateauEvidence sc = schmidl_cox_evidence(baseband, sample_rate_hz);

    // A bounded plateau is necessary but NOT sufficient: any narrowband
    // carrier inside the captured span produces one too (an in-channel
    // CW interferer, an adjacent emitter, residual LO after the DC
    // removal above). Real OFDM additionally fills its channel, so
    // require spectral width over the burst before believing it.
    double sc_conf = 0.0;
    if (sc.length > 0) {
        size_t avail = baseband.size() - sc.start;
        double bin_fraction =
            occupied_bin_fraction(baseband.data() + sc.start, avail, OCCUPIED_BIN_WITHIN_DB);
        if (bin_fraction >= MIN_OFDM_BIN_FRACTION) sc_conf = sc.confidence;
    }

    // Barker runs on a copy resampled to an exact whole-samples-per-chip
    // grid rather than on the raw capture rate - see
    // BARKER_SAMPLES_PER_CHIP for why that is a correctness fix and not
    // just a tidy-up. It also bounds cost on fast radios: the B210's
    // 56 Msps resamples DOWN to 22, so the O(N * template_len) inner
    // loop shrinks rather than growing.
    double barker_conf = 0.0;
    if (try_dsss) {
        const double barker_rate = BARKER_CHIP_RATE_HZ * BARKER_SAMPLES_PER_CHIP;
        if (std::abs(sample_rate_hz - barker_rate) < 1.0) {
            barker_conf = barker_evidence(baseband, barker_rate).confidence;
        } else {
            auto for_barker = resample_linear(baseband, sample_rate_hz, barker_rate);
            barker_conf = barker_evidence(for_barker, barker_rate).confidence;
        }
    }

    // Both confidences are now mean normalized correlations in [0,1],
    // so this comparison is finally meaningful - previously the two
    // branches produced unbounded, differently-scaled run-length ratios
    // and "whichever is larger" was close to arbitrary. A branch
    // reporting 0 did not qualify (its own evidence gates rejected it).
    ModClassification result;
    if (barker_conf > 0.0 && barker_conf >= sc_conf) {
        result.mod = ModClass::DSSS;
        result.confidence = barker_conf;
    } else if (sc_conf > 0.0) {
        result.mod = ModClass::OFDM;
        result.confidence = sc_conf;
    }
    return result;
}

double estimate_occupied_bandwidth_hz(const std::complex<float>* x, size_t n,
                                       double sample_rate_hz, double threshold_db) {
    if (n == 0 || sample_rate_hz <= 0.0) return 0.0;

    // Scale the FFT to the burst rather than fixing it at 256. A short
    // frame (a 23us ACK is ~460 samples at 20 Msps) yields only one or
    // two 256-point segments, and a one-segment periodogram is far too
    // noisy for a peak-relative span - that is what produced 0.2-0.7MHz
    // readings on bursts that were really 20MHz wide.
    //
    // Shrinking the FFT is the right lever rather than padding the
    // window with surrounding idle air, which would just dilute the
    // burst with noise and bias the span the other way. Resolution
    // still lands well inside what this has to distinguish: at 64 bins
    // over 20 Msps each bin is 312.5kHz, so a ~1MHz Bluetooth burst is
    // ~3 bins and a 16MHz Wi-Fi burst is ~51 - never confusable.
    constexpr int kMaxSegments = 256;
    constexpr int kMinSegments = 4;
    int kFftSize = 256;
    while (kFftSize > 64 && n < size_t(kFftSize) * size_t(kMinSegments)) kFftSize /= 2;
    if (n < size_t(kFftSize)) return 0.0;

    // Remove DC before measuring anything. Receiver LO leakage sits at
    // exactly the tuned centre, and config.hpp documents it at 40-50dB
    // - so it would BE the peak that this whole function measures
    // everything relative to, collapsing the -6dB span to one or two
    // bins. That is not hypothetical: it is why the live GUI label read
    // "~0MHz" on channels that genuinely had 20MHz APs on them.
    std::complex<double> mean(0.0, 0.0);
    for (size_t i = 0; i < n; ++i) mean += std::complex<double>(x[i].real(), x[i].imag());
    mean /= double(n);
    std::complex<float> dc(float(mean.real()), float(mean.imag()));

    // Welch-average across the WHOLE buffer. Previously this FFT'd only
    // the first 256 samples - 12.8us out of a full second at 20 Msps -
    // so the result was whatever happened to be in the opening blink of
    // the capture, with single-periodogram variance on top. Segments
    // are spread evenly rather than taken consecutively so a short
    // burst anywhere in the buffer still contributes.
    size_t n_segments = std::min(size_t(kMaxSegments), n / size_t(kFftSize));
    if (n_segments == 0) return 0.0;
    size_t span = n - size_t(kFftSize);
    size_t stride = (n_segments > 1) ? (span / (n_segments - 1)) : 1;
    if (stride == 0) stride = 1;

    // Hann window: a rectangular window's sidelobes smear a strong
    // narrowband component across the band, which inflates the measured
    // span - the opposite failure from the one above, but just as wrong.
    size_t fft_sz = size_t(kFftSize);
    std::vector<double> window(fft_sz);
    double window_power = 0.0;
    for (int i = 0; i < kFftSize; ++i) {
        double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * double(i) / double(kFftSize - 1)));
        window[size_t(i)] = w;
        window_power += w * w;
    }

    std::vector<double> psd(fft_sz, 0.0);
    std::vector<kiss_fft_cpx> in(fft_sz), out(fft_sz);
    kiss_fft_cfg cfg = kiss_fft_alloc(kFftSize, 0, nullptr, nullptr);
    size_t used = 0;
    for (size_t s = 0; s < n_segments; ++s) {
        size_t start = s * stride;
        if (start + size_t(kFftSize) > n) break;
        for (int i = 0; i < kFftSize; ++i) {
            std::complex<float> v = x[start + size_t(i)] - dc;
            in[size_t(i)].r = float(double(v.real()) * window[size_t(i)]);
            in[size_t(i)].i = float(double(v.imag()) * window[size_t(i)]);
        }
        kiss_fft(cfg, in.data(), out.data());
        for (int i = 0; i < kFftSize; ++i) {
            // Bin order is [0..+Nyquist), [-Nyquist..0) - reorder to
            // monotonic frequency for the span scan below.
            int shifted = (i + kFftSize / 2) % kFftSize;
            double re = out[size_t(i)].r, im = out[size_t(i)].i;
            psd[size_t(shifted)] += re * re + im * im;
        }
        ++used;
    }
    kiss_fft_free(cfg);
    if (used == 0) return 0.0;
    for (auto& p : psd) p /= (double(used) * window_power);

    double peak = *std::max_element(psd.begin(), psd.end());
    if (peak <= 0.0) return 0.0;
    double thresh_lin = peak * std::pow(10.0, threshold_db / 10.0);

    int lo = 0, hi = kFftSize - 1;
    while (lo < kFftSize && psd[size_t(lo)] < thresh_lin) ++lo;
    while (hi >= 0 && psd[size_t(hi)] < thresh_lin) --hi;
    if (lo > hi) return 0.0;

    int occupied_bins = hi - lo + 1;
    double bin_hz = sample_rate_hz / kFftSize;
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
