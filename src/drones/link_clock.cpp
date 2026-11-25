#include "link_clock.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace rfmon::link_clock {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr std::size_t decim = 10;
constexpr int max_lag = 12;          // decimated samples: about +-12 ppm over 0.5 s per iteration
constexpr int template_iterations = 3;
constexpr int harmonic_count = 20;

struct Params {
    double base_rate, n_fft, prefix, nominal_rate, frame_s;
};
Params params(Link link) {
    // Lightbridge: 4096 points at 44.8 MSps, prefixes averaging 515.8 samples, 68 symbols per 7 ms.
    // 15 kHz OcuSync: 2048 points at 30.72 MSps, LTE-style slots of 7 symbols per 0.5 ms (14 kHz).
    // 30 kHz OcuSync: 1024 points at 30.72 MSps, prefixes in the same proportion, 28 kHz.
    return link == Link::lightbridge ? Params{44.8e6, 4096, 516, 68 / 7e-3, 7e-3}
         : link == Link::ocusync_30k ? Params{30.72e6, 1024, 72, 28000, 0}
                                     : Params{30.72e6, 2048, 144, 14000, 0};
}

// Cyclic-prefix metric |sum over window of x(k) conj(x(k+lag))|, mean over each `decim` samples,
// with its mean removed.
std::vector<double> block_metric(const std::complex<float>* x, std::size_t n, std::size_t lag, std::size_t window) {
    if (n <= lag + window) return {};
    const std::size_t products = n - lag;
    std::vector<std::complex<double>> c(products + 1);
    for (std::size_t k = 0; k < products; ++k)
        c[k + 1] = c[k] + std::complex<double>(x[k]) * std::conj(std::complex<double>(x[k + lag]));
    const std::size_t m = products + 1 - window;
    std::vector<double> out(m / decim);
    double mean = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        double sum = 0;
        for (std::size_t j = 0; j < decim; ++j) {
            const std::size_t k = i * decim + j;
            sum += std::abs(c[k + window] - c[k]);
        }
        out[i] = sum / decim;
        mean += out[i];
    }
    if (out.empty()) return out;
    mean /= out.size();
    for (auto& v : out) v -= mean;
    return out;
}

double wrap_pi(double v) { return v - 2 * pi * std::round(v / (2 * pi)); }

// Weighted least squares y = a t + b with weights q; returns a, b, and var(a) scaled by the
// residual variance (dof n - 2).
struct Line { double a{}, b{}, var_a{}, rms{}; bool ok{}; };
Line fit_line(const std::vector<double>& t, const std::vector<double>& y, const std::vector<double>& q) {
    double s = 0, st = 0, stt = 0, sy = 0, sty = 0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        s += q[i]; st += q[i] * t[i]; stt += q[i] * t[i] * t[i];
        sy += q[i] * y[i]; sty += q[i] * t[i] * y[i];
    }
    const double det = s * stt - st * st;
    if (!(det > 0)) return {};
    Line line;
    line.a = (s * sty - st * sy) / det;
    line.b = (stt * sy - st * sty) / det;
    double r2 = 0, r2_plain = 0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const double r = y[i] - line.a * t[i] - line.b;
        r2 += q[i] * r * r;
        r2_plain += r * r;
    }
    const double s2 = r2 / std::max<double>(1, double(t.size()) - 2);
    line.var_a = s2 * s / det;
    line.rms = std::sqrt(r2_plain / t.size());
    line.ok = true;
    return line;
}

std::optional<Estimate> harmonics(Link link, const Params& p, const std::vector<std::vector<double>>& metrics,
                                  const std::vector<double>& t0, double fs) {
    const std::size_t nb = metrics.size();
    double mean_t = 0;
    for (double t : t0) mean_t += t;
    mean_t /= nb;
    std::vector<double> tc(nb);
    for (std::size_t b = 0; b < nb; ++b) tc[b] = t0[b] - mean_t;
    double sum_w = 0, sum_wr = 0, min_sigma = INFINITY;
    std::vector<std::pair<double, double>> ests;  // rate offset (Hz), sigma (Hz)
    for (int h = 1; h <= harmonic_count; ++h) {
        if (h * p.nominal_rate >= fs / decim / 2 * .8) break;
        const double w = -2 * pi * h * p.nominal_rate;
        std::vector<std::complex<double>> z(nb);
        double zmax = 0;
        for (std::size_t b = 0; b < nb; ++b) {
            // Each metric sample stands for the middle of its `decim` input samples.
            std::complex<double> phasor = std::polar(1.0, w * (t0[b] + (decim / 2.) / fs));
            const std::complex<double> step = std::polar(1.0, w * decim / fs);
            std::complex<double> sum{};
            for (double v : metrics[b]) { sum += v * phasor; phasor *= step; }
            z[b] = sum;
            zmax = std::max(zmax, std::abs(sum));
        }
        if (!(zmax > 0)) continue;
        std::vector<double> q(nb), ph(nb);
        for (std::size_t b = 0; b < nb; ++b) q[b] = std::norm(z[b]) / (zmax * zmax);
        const auto unwrapped = [&](double slope) {
            for (std::size_t b = 0; b < nb; ++b) {
                const double a = std::arg(z[b] * std::polar(1.0, -slope * tc[b]));
                ph[b] = b ? ph[b - 1] + wrap_pi(a - ph[b - 1]) : a;
            }
        };
        // Unwrap, fit, remove the fit and unwrap again: guards against a bad first unwrap.
        double slope = 0;
        Line line;
        for (int i = 0; i < 3; ++i) {
            unwrapped(slope);
            line = fit_line(tc, ph, q);
            if (!line.ok) break;
            slope += line.a;
        }
        if (!line.ok) continue;
        unwrapped(slope);
        line = fit_line(tc, ph, q);
        if (!line.ok) continue;
        std::complex<double> zs{};
        double zabs = 0;
        for (std::size_t b = 0; b < nb; ++b) {
            zs += z[b] * std::polar(1.0, -(slope + line.a) * tc[b]);
            zabs += std::abs(z[b]);
        }
        const double coherence = std::abs(zs) / (zabs + 1e-30);
        // A harmonic whose phase wanders (low coherence or big residual) is not used.
        if (!(coherence > .5) || !(line.rms < .8)) continue;
        const double rate = (slope + line.a) / (2 * pi * h);
        const double sigma = std::sqrt(line.var_a) / (2 * pi * h);
        if (!(sigma > 0) || !std::isfinite(rate)) continue;
        ests.emplace_back(rate, sigma);
        sum_w += 1 / (sigma * sigma);
        sum_wr += rate / (sigma * sigma);
        min_sigma = std::min(min_sigma, sigma);
    }
    if (ests.empty()) return std::nullopt;
    const double rate = sum_wr / sum_w;
    double spread = 0;
    for (const auto& [r, s] : ests) spread += (r - rate) * (r - rate) / (s * s);
    spread = std::sqrt(spread / sum_w);
    // Harmonic estimates share the same data, so they are not independent: quote the larger of
    // the combined error and the spread between harmonics.
    const double sigma = std::max({1 / std::sqrt(sum_w), spread, min_sigma / 2});
    return Estimate{link, rate / p.nominal_rate * 1e6, sigma_scale * sigma / p.nominal_rate * 1e6, nb};
}

std::optional<Estimate> frame_template(const Params& p, const std::vector<std::vector<double>>& metrics,
                                       const std::size_t* starts, double fs) {
    const double fsd = fs / decim, period0 = p.frame_s * fsd;
    const auto bins = static_cast<std::size_t>(std::lround(period0));
    const std::size_t nb = metrics.size();
    if (bins < 4 * max_lag) return std::nullopt;
    // Each metric sample sits at a fractional template position: bin where[b][n] plus frac[b][n].
    // Samples are spread over the two neighbouring bins (and read back the same way), so a
    // block's timing is not rounded to whole bins, which biased the slope by up to 10%.
    std::vector<std::vector<std::size_t>> where(nb);
    std::vector<std::vector<float>> frac(nb);
    std::vector<double> sums(bins), counts(bins), own_sum(bins), own_count(bins), shape(bins);
    std::vector<double> padded(bins + 2 * max_lag + 2);
    double e = 0;
    std::optional<Estimate> result;
    for (int iteration = 0; iteration < template_iterations; ++iteration) {
        const double period = period0 / (1 + e);
        std::fill(sums.begin(), sums.end(), 0.);
        std::fill(counts.begin(), counts.end(), 0.);
        for (std::size_t b = 0; b < nb; ++b) {
            where[b].resize(metrics[b].size());
            frac[b].resize(metrics[b].size());
            const double first = double(starts[b]) / decim;
            for (std::size_t n = 0; n < metrics[b].size(); ++n) {
                const double phase = std::fmod(first + n, period) / period * bins;
                const double whole = std::floor(phase);
                const auto k = static_cast<std::size_t>(whole) % bins, k1 = (k + 1) % bins;
                const double f = phase - whole;
                where[b][n] = k;
                frac[b][n] = static_cast<float>(f);
                sums[k] += (1 - f) * metrics[b][n];
                counts[k] += 1 - f;
                sums[k1] += f * metrics[b][n];
                counts[k1] += f;
            }
        }
        std::vector<double> lag(nb), weight(nb), time(nb);
        double max_weight = 0;
        for (std::size_t b = 0; b < nb; ++b) {
            // Leave-one-out template: the other blocks folded at the frame period.
            std::fill(own_sum.begin(), own_sum.end(), 0.);
            std::fill(own_count.begin(), own_count.end(), 0.);
            for (std::size_t n = 0; n < metrics[b].size(); ++n) {
                const auto k = where[b][n], k1 = (k + 1) % bins;
                const double f = frac[b][n];
                own_sum[k] += (1 - f) * metrics[b][n];
                own_count[k] += 1 - f;
                own_sum[k1] += f * metrics[b][n];
                own_count[k1] += f;
            }
            double mean = 0;
            for (std::size_t k = 0; k < bins; ++k) {
                const double c = counts[k] - own_count[k];
                shape[k] = c > 1e-9 ? (sums[k] - own_sum[k]) / c : 0.;
                mean += shape[k];
            }
            mean /= bins;
            // The template padded by max_lag + 1 bins either side, so shifted reads need no wrap.
            for (std::size_t i = 0; i < padded.size(); ++i)
                padded[i] = shape[(i + bins - max_lag) % bins] - mean;
            std::array<double, 2 * max_lag + 1> r{};
            for (int k = -max_lag; k <= max_lag; ++k) {
                double acc = 0;
                const double* base = padded.data() + max_lag + k;
                for (std::size_t n = 0; n < metrics[b].size(); ++n) {
                    const double* at = base + where[b][n];
                    const double f = frac[b][n];
                    acc += metrics[b][n] * (at[0] + f * (at[1] - at[0]));
                }
                r[k + max_lag] = acc;
            }
            const auto peak = static_cast<int>(std::max_element(r.begin(), r.end()) - r.begin());
            double d = 0;
            if (peak > 0 && peak < 2 * max_lag) {
                const double den = r[peak - 1] - 2 * r[peak] + r[peak + 1];
                if (den < 0) d = .5 * (r[peak - 1] - r[peak + 1]) / den;
            }
            lag[b] = peak - max_lag + d;
            weight[b] = std::max(r[peak], 0.);
            time[b] = (double(starts[b]) + metrics[b].size() * decim / 2.) / fs;
            max_weight = std::max(max_weight, weight[b]);
        }
        if (!(max_weight > 0)) return std::nullopt;
        double sw = 0, swt = 0;
        for (std::size_t b = 0; b < nb; ++b) {
            weight[b] /= max_weight;
            sw += weight[b];
            swt += weight[b] * time[b];
        }
        for (auto& t : time) t -= swt / sw;
        const Line line = fit_line(time, lag, weight);
        if (!line.ok) return std::nullopt;
        // A block that matches at lag k sits k decimated samples early: lags growing with time
        // mean the drone's clock runs fast.
        e += line.a / fsd;
        result = Estimate{Link::lightbridge, e * 1e6, sigma_scale * std::sqrt(line.var_a) / fsd * 1e6, nb};
    }
    if (result && !(std::isfinite(result->ppm) && std::isfinite(result->sigma_ppm))) return std::nullopt;
    return result;
}
}

std::optional<Estimate> measure(Link link, const std::complex<float>* blocks, std::size_t block_len,
                                const std::size_t* starts, std::size_t count, double rate) {
    if (!blocks) return std::nullopt;
    std::vector<const std::complex<float>*> each(count);
    for (std::size_t b = 0; b < count; ++b) each[b] = blocks + b * block_len;
    return measure(link, each.data(), block_len, starts, count, rate);
}

std::optional<Estimate> measure(Link link, const std::complex<float>* const* blocks, std::size_t block_len,
                                const std::size_t* starts, std::size_t count, double rate) {
    if (!(rate >= 20e6 && rate <= 25e6) || count < 4 || !blocks || !starts) return std::nullopt;
    const auto p = params(link);
    const auto lag = static_cast<std::size_t>(std::lround(p.n_fft * rate / p.base_rate));
    const auto window = std::max<std::size_t>(8, static_cast<std::size_t>(.9 * p.prefix * rate / p.base_rate));
    std::vector<std::vector<double>> metrics(count);
    std::vector<double> t0(count);
    for (std::size_t b = 0; b < count; ++b) {
        if (!blocks[b]) return std::nullopt;
        metrics[b] = block_metric(blocks[b], block_len, lag, window);
        if (metrics[b].size() < 2 * max_lag + 1) return std::nullopt;
        t0[b] = double(starts[b]) / rate;
    }
    auto e = link == Link::lightbridge ? frame_template(p, metrics, starts, rate) : harmonics(link, p, metrics, t0, rate);
    if (e) {
        // Symbol grid position: phase of the metric at the nominal symbol rate, block times
        // from the capture's first sample (each metric sample at the middle of its samples).
        const double period = symbol_period_s(link);
        const double w = -2 * pi / period;
        std::complex<double> z{};
        for (std::size_t b = 0; b < count; ++b) {
            std::complex<double> phasor = std::polar(1.0, w * (t0[b] + (decim / 2.) / rate));
            const std::complex<double> step = std::polar(1.0, w * decim / rate);
            for (double v : metrics[b]) { z += v * phasor; phasor *= step; }
        }
        if (std::abs(z) > 0 && std::isfinite(z.real()) && std::isfinite(z.imag())) {
            double phase = -std::arg(z) / (2 * pi) * period;
            if (phase < 0) phase += period;
            if (phase >= period) phase = 0;
            e->has_symbol_phase = true;
            e->symbol_phase_s = phase;
        }
    }
    if (e) {
        const double span = std::max(.05, (double(starts[count - 1] - starts[0]) + block_len) / rate);
        const double floor = (link == Link::lightbridge ? lightbridge_min_sigma_ppm : ocusync_min_sigma_ppm) *
                             min_sigma_span_s / span;
        e->sigma_ppm = std::max(e->sigma_ppm, floor);
    }
    return e;
}
}
