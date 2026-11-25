#pragma once

// Carrier frequency offsets as a per-transmitter measurement (display and logging only).
//
// A drone builds its 2.4 GHz and 5.8 GHz carriers from one reference crystal, so its error in Hz
// grows with the carrier and the same error in parts per million (ppm) shows on both bands.
// The receiver's own crystal error is shared by everything it hears, so offsets from one X310 can
// be compared with each other, not with another receiver's. Nothing here reaches a level,
// verdict or track; the Drones page uses it only to mark rows that look like one aircraft.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace rfmon::drone_claims {

// One DroneID packet's offset, measured against the documented channel it was decoded on.
struct OffsetSample {
    double ppm{};
    double offset_hz{};
    double channel_hz{};
    std::chrono::steady_clock::time_point observed{};
};

// Offset (Hz) of a carrier from its nominal frequency, as parts per million of that frequency.
inline std::optional<double> offset_ppm(double offset_hz, double nominal_hz) {
    if (!std::isfinite(offset_hz) || !std::isfinite(nominal_hz) || nominal_hz <= 0) return std::nullopt;
    return offset_hz / nominal_hz * 1e6;
}

struct OffsetBand {
    std::size_t count{};
    double mean_ppm{}, spread_ppm{};  // spread: sample standard deviation (0 for one packet)
};

struct OffsetSummary {
    OffsetBand all, low, high;        // low: 2.4 GHz, high: 5 GHz
    double latest_ppm{}, latest_hz{}, latest_channel_hz{};
    // Mean 5 GHz minus mean 2.4 GHz, when both bands have packets. A drone that builds both
    // carriers from one crystal shows about zero here; that is what the measurement checks.
    std::optional<double> band_difference_ppm;
};

inline OffsetBand offset_band(const std::vector<OffsetSample>& samples, int band) {
    OffsetBand out;
    double sum = 0;
    for (const auto& s : samples)
        if (band < 0 || (band == 1) == (s.channel_hz >= 3e9)) { sum += s.ppm; ++out.count; }
    if (!out.count) return out;
    out.mean_ppm = sum / out.count;
    if (out.count > 1) {
        double squares = 0;
        for (const auto& s : samples)
            if (band < 0 || (band == 1) == (s.channel_hz >= 3e9))
                squares += (s.ppm - out.mean_ppm) * (s.ppm - out.mean_ppm);
        out.spread_ppm = std::sqrt(squares / (out.count - 1));
    }
    return out;
}

// One capture's link-grid position (symbol_timing::GridOffset) with where and when it was taken.
struct LinkGridSample {
    double hz{}, spacing_hz{}, centre_hz{}, coherence{};
    std::chrono::steady_clock::time_point observed{};
};

// Comparing crystals. A drone's offset drifts as it warms (about +0.04 ppm per minute seen live on
// 19 Nov), so values are compared only within crystal_window_s of each other.
inline constexpr double crystal_window_s = 90;
// A 15 kHz link grid sits a whole multiple of this from (nominal carrier x ppm): measured on four
// DJI drones (Mini 2 twice, Mavic, Matrice 300). One link capture therefore gives the crystal only
// modulo 5 kHz, and an unrelated link lands anywhere within +-2.5 kHz of a drone's prediction.
inline constexpr double link_raster_hz = 5000;

inline double seconds_apart(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::abs(std::chrono::duration<double>(a - b).count());
}

// Mean ppm of the samples within window_s of t.
inline std::optional<double> ppm_near(const std::vector<OffsetSample>& samples,
                                      std::chrono::steady_clock::time_point t, double window_s = crystal_window_s) {
    double sum = 0;
    std::size_t n = 0;
    for (const auto& s : samples)
        if (seconds_apart(s.observed, t) <= window_s) { sum += s.ppm; ++n; }
    if (!n) return std::nullopt;
    return sum / n;
}

inline double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const auto n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// Two sets of DroneID offsets: for each sample of b with samples of a within the window, a's ppm
// there minus b's; the median of those differences.
struct DroneIdComparison {
    std::size_t pairs{};
    double difference_ppm{};
};
inline std::optional<DroneIdComparison> compare_droneid(const std::vector<OffsetSample>& a,
                                                        const std::vector<OffsetSample>& b,
                                                        double window_s = crystal_window_s) {
    std::vector<double> d;
    for (const auto& s : b)
        if (const auto near = ppm_near(a, s.observed, window_s)) d.push_back(*near - s.ppm);
    if (d.empty()) return std::nullopt;
    return DroneIdComparison{d.size(), median(d)};
}

// How far a link grid sits from the nearest (k x link_raster_hz + centre x ppm), in Hz, signed.
inline double link_residual_hz(const LinkGridSample& g, double ppm) {
    double r = std::fmod(g.hz - g.centre_hz * ppm * 1e-6, link_raster_hz);
    if (r < -link_raster_hz / 2) r += link_raster_hz;
    if (r >= link_raster_hz / 2) r -= link_raster_hz;
    return r;
}

// A link's 15 kHz grids against DroneID offsets: each grid with DroneID packets within the window
// is compared with their ppm there. Median of |residual| over those captures.
struct LinkFit {
    std::size_t captures{};
    double median_residual_hz{};
    double ppm{};  // mean DroneID ppm used, over the compared captures
};
inline std::optional<LinkFit> fit_link(const std::vector<LinkGridSample>& grids, const std::vector<OffsetSample>& droneid,
                                       double window_s = crystal_window_s) {
    std::vector<double> residuals;
    double ppm_sum = 0;
    for (const auto& g : grids) {
        if (std::abs(g.spacing_hz - 15e3) > 1) continue;  // the 5 kHz relation is only known for 15 kHz links
        const auto ppm = ppm_near(droneid, g.observed, window_s);
        if (!ppm) continue;
        residuals.push_back(std::abs(link_residual_hz(g, *ppm)));
        ppm_sum += *ppm;
    }
    if (residuals.empty()) return std::nullopt;
    return LinkFit{residuals.size(), median(residuals), ppm_sum / residuals.size()};
}

// One capture's link clock (symbol_timing::Result::link_clock): the drone's crystal error in
// ppm from how fast its link's symbols arrive, relative to this receiver like DroneID ppm.
struct LinkClockSample {
    double ppm{}, sigma_ppm{};
    bool lightbridge{};
    double centre_hz{};
    std::chrono::steady_clock::time_point observed{};
};

// Inverse-variance mean of link-clock readings. Its sigma is at least the readings' own scatter
// over sqrt(n): live, Lightbridge readings scattered about 2.5x their reported sigma, and on 23 Nov
// 2026 six 5.2 GHz Phantom readings (mean -2.63, reported +-0.52, scatter-based about +-1.0) read
// as "probably not" the Phantom's 2.4 GHz row (-4.53), making a third drone.
struct LinkClockMean {
    std::size_t captures{};
    double ppm{}, sigma_ppm{};
};
inline std::optional<LinkClockMean> link_clock_mean(const std::vector<const LinkClockSample*>& samples) {
    double w = 0, wp = 0;
    for (const auto* s : samples)
        if (s->sigma_ppm > 0) { w += 1 / (s->sigma_ppm * s->sigma_ppm); wp += s->ppm / (s->sigma_ppm * s->sigma_ppm); }
    if (!(w > 0)) return std::nullopt;
    const double mean = wp / w;
    double sigma = 1 / std::sqrt(w);
    if (samples.size() >= 2) {
        double ss = 0;
        for (const auto* s : samples) ss += (s->ppm - mean) * (s->ppm - mean);
        sigma = std::max(sigma, std::sqrt(ss / double(samples.size() - 1) / double(samples.size())));
    }
    return LinkClockMean{samples.size(), mean, sigma};
}
inline std::optional<LinkClockMean> link_clock_mean(const std::vector<LinkClockSample>& samples) {
    std::vector<const LinkClockSample*> all;
    for (const auto& s : samples) all.push_back(&s);
    return link_clock_mean(all);
}

// A row's link-clock readings without outliers: with at least five, a reading further from their
// median than 5 robust spreads (1.4826 x median absolute deviation), 4 of its own sigmas and
// 0.3 ppm is dropped. Live 21 Nov 2026: a Mini 2 capture with its link in 11 of 24 blocks read
// -11.93 +- 0.11 ppm among readings of -0.40 to -0.62; the Phantom's Lightbridge readings
// (robust spread about 2 ppm) all stay. Not from fewer: with three, the median and spread are so
// loose that of three scattered Wi-Fi readings the odd one was dropped and the other two agreed.
inline constexpr std::size_t clock_outlier_min_readings = 5;
inline std::vector<LinkClockSample> without_clock_outliers(std::vector<LinkClockSample> samples) {
    if (samples.size() < clock_outlier_min_readings) return samples;
    const auto median_of = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        const std::size_t n = v.size();
        return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    };
    std::vector<double> ppm, dev;
    for (const auto& s : samples) ppm.push_back(s.ppm);
    const double m = median_of(ppm);
    for (double p : ppm) dev.push_back(std::abs(p - m));
    const double spread = 1.4826 * median_of(dev);
    std::vector<LinkClockSample> kept;
    for (const auto& s : samples)
        if (std::abs(s.ppm - m) <= std::max({5 * spread, 4 * s.sigma_ppm, 0.3})) kept.push_back(s);
    return kept;
}

// How far a row's link-clock readings scatter about their inverse-variance mean, in units of
// their own errors: chi-square per degree of freedom (about 1 when all come from one clock).
// nullopt with fewer than two readings.
inline std::optional<double> link_clock_scatter(const std::vector<LinkClockSample>& samples) {
    const auto mean = link_clock_mean(samples);
    if (!mean || samples.size() < 2) return std::nullopt;
    double chi2 = 0;
    for (const auto& s : samples)
        if (s.sigma_ppm > 0) chi2 += (s.ppm - mean->ppm) * (s.ppm - mean->ppm) / (s.sigma_ppm * s.sigma_ppm);
    return chi2 / double(samples.size() - 1);
}

// Two rows' link clocks: each row's readings taken within window_s of one of the other's.
struct LinkClockComparison {
    LinkClockMean a, b;
    double difference_ppm{}, sigma_ppm{};  // a - b, and its error
};
inline std::optional<LinkClockComparison> compare_link_clocks(const std::vector<LinkClockSample>& a,
                                                              const std::vector<LinkClockSample>& b,
                                                              double window_s = crystal_window_s) {
    const auto near = [&](const std::vector<LinkClockSample>& from, const std::vector<LinkClockSample>& other) {
        std::vector<const LinkClockSample*> out;
        for (const auto& s : from)
            for (const auto& o : other)
                if (seconds_apart(s.observed, o.observed) <= window_s) { out.push_back(&s); break; }
        return out;
    };
    const auto ma = link_clock_mean(near(a, b)), mb = link_clock_mean(near(b, a));
    if (!ma || !mb) return std::nullopt;
    return LinkClockComparison{*ma, *mb, ma->ppm - mb->ppm, std::hypot(ma->sigma_ppm, mb->sigma_ppm)};
}

// A link's clock against DroneID packets: each reading with packets within the window is
// compared with their ppm there; inverse-variance mean of (link - DroneID). DroneID's own error
// (about 0.05 ppm) is small beside a link reading's and is not added.
struct LinkClockFit {
    std::size_t captures{};
    double difference_ppm{}, sigma_ppm{};
    double ppm{};  // mean DroneID ppm used
};
inline std::optional<LinkClockFit> fit_link_clock(const std::vector<LinkClockSample>& clocks,
                                                  const std::vector<OffsetSample>& droneid,
                                                  double window_s = crystal_window_s) {
    double w = 0, wd = 0, ppm_sum = 0;
    std::size_t n = 0;
    for (const auto& c : clocks) {
        const auto ppm = ppm_near(droneid, c.observed, window_s);
        if (!ppm || !(c.sigma_ppm > 0)) continue;
        const double wi = 1 / (c.sigma_ppm * c.sigma_ppm);
        w += wi; wd += wi * (c.ppm - *ppm); ppm_sum += *ppm; ++n;
    }
    if (!n) return std::nullopt;
    return LinkClockFit{n, wd / w, 1 / std::sqrt(w), ppm_sum / n};
}

inline std::optional<OffsetSummary> summarize_offsets(const std::vector<OffsetSample>& samples) {
    if (samples.empty()) return std::nullopt;
    OffsetSummary out;
    out.all = offset_band(samples, -1);
    out.low = offset_band(samples, 0);
    out.high = offset_band(samples, 1);
    const OffsetSample* latest = &samples.front();
    for (const auto& s : samples) if (s.observed >= latest->observed) latest = &s;
    out.latest_ppm = latest->ppm;
    out.latest_hz = latest->offset_hz;
    out.latest_channel_hz = latest->channel_hz;
    if (out.low.count && out.high.count) out.band_difference_ppm = out.high.mean_ppm - out.low.mean_ppm;
    return out;
}
}
