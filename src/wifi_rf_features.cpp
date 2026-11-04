#include "wifi_rf_features.hpp"

#include <algorithm>
#include <map>

namespace rfmon::wifi {
namespace {

using C = std::complex<float>;
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kAnchorBelowSymbols = 20;
constexpr size_t kEarlySymbols = 16;

// Weighted least-squares line v = a + b t; returns b, its 1-sigma
// uncertainty and the rms residual. Needs three points for an uncertainty.
struct Line { double slope = 0, sigma = NAN, rms = NAN; bool ok = false; };
Line fit_line(const std::vector<double>& t, const std::vector<double>& v, std::vector<double> w = {}) {
    Line l;
    const size_t n = t.size();
    if (n < 2) return l;
    if (w.size() != n) w.assign(n, 1.0);
    double sw = 0, mt = 0, mv = 0;
    for (size_t i = 0; i < n; ++i) { sw += w[i]; mt += w[i] * t[i]; mv += w[i] * v[i]; }
    mt /= sw; mv /= sw;
    double stt = 0, stv = 0;
    for (size_t i = 0; i < n; ++i) { stt += w[i] * (t[i] - mt) * (t[i] - mt); stv += w[i] * (t[i] - mt) * (v[i] - mv); }
    if (stt <= 0) return l;
    l.slope = stv / stt;
    l.ok = true;
    if (n >= 3) {
        double ss = 0;
        for (size_t i = 0; i < n; ++i) { const double r = v[i] - (mv + l.slope * (t[i] - mt)); ss += w[i] * r * r; }
        l.rms = std::sqrt(ss / sw * double(n) / double(n - 2));
        l.sigma = l.rms / std::sqrt(stt);
    }
    return l;
}
void unwrap(std::vector<double>& p) {
    for (size_t i = 1; i < p.size(); ++i) p[i] += 2 * kPi * std::round((p[i - 1] - p[i]) / (2 * kPi));
}

}  // namespace

FrameRfFeatures ofdm_features(const std::vector<std::vector<C>>& y, const std::vector<std::vector<C>>& x,
                              const std::vector<C>& h, const std::vector<int>& tones,
                              const std::vector<double>& window, int fft_size, double fs, double preamble_cfo_hz,
                              double anchor, double anchor_weight) {
    FrameRfFeatures f;
    const size_t n_sym = std::min({y.size(), x.size(), window.size()});
    if (n_sym < 2 || tones.size() != h.size()) return f;
    std::map<int, size_t> index;
    for (size_t i = 0; i < tones.size(); ++i) index[tones[i]] = i;
    // Per symbol: common phase and phase slope across tones (timing), by
    // weighted least squares on the known-point phase errors.
    std::vector<double> t(n_sym), theta(n_sym), delay(n_sym);
    for (size_t n = 0; n < n_sym; ++n) {
        C sum{};
        for (size_t i = 0; i < tones.size(); ++i)
            if (std::norm(x[n][i]) > 0 && std::norm(h[i]) > 0) sum += y[n][i] * std::conj(h[i] * x[n][i]);
        const double rough = std::arg(sum);
        double sw = 0, sk = 0, sr = 0;
        std::vector<double> k, r, w;
        for (size_t i = 0; i < tones.size(); ++i) {
            if (!(std::norm(x[n][i]) > 0 && std::norm(h[i]) > 0)) continue;
            const C z = y[n][i] * std::conj(h[i] * x[n][i]);
            k.push_back(tones[i]); r.push_back(std::remainder(std::arg(z) - rough, 2 * kPi)); w.push_back(std::abs(z));
            sw += w.back(); sk += w.back() * k.back(); sr += w.back() * r.back();
        }
        if (sw <= 0) return f;
        const double mk = sk / sw, mr = sr / sw;
        double skk = 0, skr = 0;
        for (size_t i = 0; i < k.size(); ++i) { skk += w[i] * (k[i] - mk) * (k[i] - mk); skr += w[i] * (k[i] - mk) * (r[i] - mr); }
        const double s = skk > 0 ? skr / skk : 0;                 // rad per tone
        theta[n] = rough + mr - s * mk;                            // phase at tone 0
        delay[n] = -s * fft_size / (2 * kPi);                      // window offset in samples
        t[n] = (window[n] + fft_size / 2.0) / fs;                  // symbol centre, seconds
    }
    unwrap(theta);
    // The training field is the phase reference: zero phase at its centre.
    // Used for short frames only: live, it cut their 90th-percentile CFO
    // deviation from 0.27 to 0.11 ppm, but on frames of 20+ symbols it
    // added the training field's own error (0.096 -> 0.127 ppm on data).
    std::vector<double> ta = t, pa = theta, wa(n_sym, 1.0);
    if (anchor_weight > 0 && n_sym < kAnchorBelowSymbols) { ta.insert(ta.begin(), anchor / fs); pa.insert(pa.begin(), 0.0); wa.insert(wa.begin(), anchor_weight); }
    const Line ph = fit_line(ta, pa, wa), tm = fit_line(t, delay);
    if (!ph.ok) return f;
    {   // Early CFO: anchor plus the first symbols, the same stretch in every frame.
        const size_t m = std::min<size_t>(n_sym, kEarlySymbols);
        std::vector<double> te(t.begin(), t.begin() + long(m)), pe(theta.begin(), theta.begin() + long(m)), we(m, 1.0);
        if (anchor_weight > 0) { te.insert(te.begin(), anchor / fs); pe.insert(pe.begin(), 0.0); we.insert(we.begin(), anchor_weight); }
        const Line e = fit_line(te, pe, we);
        if (e.ok) {
            f.cfo_early_hz = preamble_cfo_hz + e.slope / (2 * kPi);
            f.cfo_early_sigma_hz = e.sigma / (2 * kPi);
            if (n_sym >= 40) f.settling_hz = (preamble_cfo_hz + ph.slope / (2 * kPi)) - f.cfo_early_hz;
        }
    }
    f.method = "OFDM data-aided";
    f.symbols = int(n_sym);
    f.cfo_hz = preamble_cfo_hz + ph.slope / (2 * kPi);
    f.cfo_sigma_hz = ph.sigma / (2 * kPi);
    f.phase_jitter_deg = ph.rms * 180 / kPi;
    // Sampling-clock offset from the drift of the timing offset, signed so
    // it equals the CFO in ppm when one crystal drives both (measured live:
    // within 0.03-0.6 ppm of the CFO for every transmitter checked).
    if (tm.ok) { f.sfo_ppm = -tm.slope / fs * 1e6; f.sfo_sigma_ppm = tm.sigma / fs * 1e6; }
    // Residuals after the per-symbol corrections: SNR, transmitter I/Q
    // image (error regressed on the mirror-tone point) and carrier leakage.
    double sig = 0, err = 0, xp = 0, xn = 0;
    C nu_num{}; double nu_den = 0;
    C dc{};
    size_t dc_count = 0;
    const auto dc_it = index.find(0);
    for (size_t n = 0; n < n_sym; ++n) {
        const double s = -2 * kPi * delay[n] / fft_size;
        for (size_t i = 0; i < tones.size(); ++i) {
            const C rot = std::polar(1.f, float(-theta[n] - s * tones[i]));
            const C yc = y[n][i] * rot;
            if (std::norm(x[n][i]) > 0) {
                const C ref = h[i] * x[n][i], e = yc - ref;
                sig += std::norm(ref); err += std::norm(e); xp += std::norm(x[n][i]); ++xn;
                const auto m = index.find(-tones[i]);
                if (m != index.end() && std::norm(x[n][m->second]) > 0) {
                    const C g = h[i] * std::conj(x[n][m->second]);
                    nu_num += e * std::conj(g); nu_den += std::norm(g);
                }
            } else if (dc_it != index.end() && i == dc_it->second) {
                dc += yc; ++dc_count;
            }
        }
    }
    if (err > 0) f.snr_db = 10 * std::log10(sig / err);
    if (nu_den > 0) f.iq_image_db = 20 * std::log10(std::max(1e-9, double(std::abs(nu_num)) / nu_den));
    if (dc_count && xn > 0) {
        // Channel on the DC tone from its neighbours.
        C hdc{}; int nb = 0;
        for (int k : {-1, 1}) { const auto it = index.find(k); if (it != index.end()) { hdc += h[it->second]; ++nb; } }
        if (nb && std::norm(hdc) > 0) {
            hdc /= float(nb);
            const double amp = std::abs(dc / float(dc_count)) / std::abs(hdc);
            f.lo_leak_dbc = 20 * std::log10(std::max(1e-9, amp)) - 10 * std::log10(xp / xn);
        }
    }
    return f;
}

FrameRfFeatures dsss_features(const std::vector<std::complex<double>>& despread, const std::vector<int8_t>& polarity,
                              double symbol_rate_hz) {
    FrameRfFeatures f;
    const size_t n = std::min(despread.size(), polarity.size());
    if (n < 3) return f;
    std::vector<double> t(n), p(n);
    for (size_t k = 0; k < n; ++k) { t[k] = double(k) / symbol_rate_hz; p[k] = std::arg(despread[k] * double(polarity[k])); }
    unwrap(p);
    const Line ph = fit_line(t, p);
    if (!ph.ok) return f;
    f.method = "DSSS data-aided";
    f.symbols = int(n);
    f.cfo_hz = ph.slope / (2 * kPi);
    f.cfo_sigma_hz = ph.sigma / (2 * kPi);
    f.phase_jitter_deg = ph.rms * 180 / kPi;
    // SNR from the derotated symbols' spread about their mean.
    std::complex<double> mean{};
    double mt = 0, mp = 0;
    for (size_t k = 0; k < n; ++k) { mt += t[k]; mp += p[k]; }
    mt /= double(n); mp /= double(n);
    std::vector<std::complex<double>> z(n);
    for (size_t k = 0; k < n; ++k) {
        z[k] = despread[k] * double(polarity[k]) * std::polar(1.0, -(mp + ph.slope * (t[k] - mt)));
        mean += z[k];
    }
    mean /= double(n);
    double var = 0;
    for (const auto& v : z) var += std::norm(v - mean);
    var /= double(n);
    if (var > 0) f.snr_db = 10 * std::log10(std::norm(mean) / var);
    return f;
}

}  // namespace rfmon::wifi
