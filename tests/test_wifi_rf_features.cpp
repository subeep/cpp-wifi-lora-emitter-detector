// Whole-frame RF feature fit (wifi_rf_features.hpp) on synthetic symbols
// with known impairments: carrier offset, sampling-clock offset, transmitter
// carrier leakage and I/Q image, plus the DSSS phase fit.
#include "wifi_rf_features.hpp"
#include <cmath>
#include <iostream>
#include <random>

using namespace rfmon::wifi;
using C = std::complex<float>;

int main() {
    int failures = 0, checks = 0;
    auto check = [&](bool ok, const std::string& name) { ++checks; if (!ok) ++failures; std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; };
    const double pi = 3.14159265358979323846, fs = 20e6;
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0, 1);

    // OFDM: tones -26..26 (DC unused), QPSK, frequency-selective channel.
    struct Case { const char* name; double cfo_hz, sfo_ppm, leak_dbc, image_db; int symbols; float snr_db; };
    for (const Case& c : {Case{"long frame", 180.0, 12.0, -30.0, -32.0, 200, 30}, Case{"short frame", -420.0, 0.0, -100.0, -100.0, 6, 25}}) {
        std::vector<int> tones;
        for (int k = -26; k <= 26; ++k) tones.push_back(k);
        std::vector<C> h;
        for (int k : tones) h.push_back(std::polar(1.0f + 0.3f * std::cos(0.2f * k), 0.15f * k));
        std::vector<std::vector<C>> y, x;
        std::vector<double> window;
        const C leak = std::polar(float(std::pow(10, c.leak_dbc / 20)), 0.7f), nu = std::polar(float(std::pow(10, c.image_db / 20)), -1.1f);
        const float sigma = float(std::pow(10, -c.snr_db / 20) / std::sqrt(2));
        for (int n = 0; n < c.symbols; ++n) {
            // Phase and timing are referenced to the training field's centre (sample 160).
            const double start = 320 + 80.0 * n, t = (start + 32 - 160) / fs;
            window.push_back(start);
            std::vector<C> xs(tones.size()), ys(tones.size());
            for (size_t i = 0; i < tones.size(); ++i)
                if (tones[i]) xs[i] = C(rng() & 1 ? 1.f : -1.f, rng() & 1 ? 1.f : -1.f) / std::sqrt(2.f);
            // A fast transmitter clock raises the carrier and makes the window slide later.
            const double drift = c.sfo_ppm * 1e-6 * (start + 32 - 160);
            for (size_t i = 0; i < tones.size(); ++i) {
                const int k = tones[i];
                size_t mirror = 0;
                for (size_t j = 0; j < tones.size(); ++j) if (tones[j] == -k) mirror = j;
                C tx = xs[i] + nu * std::conj(xs[mirror]);  // transmitter I/Q image
                if (!k) tx = leak;                            // transmitter carrier leakage
                const C ch = (k ? h[i] : 0.5f * (h[i - 1] + h[i + 1]));
                ys[i] = ch * tx * std::polar(1.f, float(2 * pi * c.cfo_hz * t)) * std::polar(1.f, float(2 * pi * k * drift / 64)) +
                        sigma * C(noise(rng), noise(rng));
            }
            y.push_back(ys);
            x.push_back(xs);
        }
        const auto f = ofdm_features(y, x, h, tones, window, 64, fs, 1000.0, 160, 2);
        const std::string n = c.name;
        check(f.method == "OFDM data-aided" && f.symbols == c.symbols, n + ": fitted");
        check(std::abs(f.cfo_hz - (1000.0 + c.cfo_hz)) < (c.symbols > 20 ? 5.0 : 60.0), n + ": CFO includes the preamble correction (" + std::to_string(f.cfo_hz) + " Hz)");
        check(std::abs(f.cfo_early_hz - (1000.0 + c.cfo_hz)) < 60.0, n + ": early CFO (" + std::to_string(f.cfo_early_hz) + " Hz)");
        if (c.symbols > 20) {
            check(std::abs(f.sfo_ppm - c.sfo_ppm) < 3.0, n + ": sampling-clock offset sign and size (" + std::to_string(f.sfo_ppm) + " ppm)");
            check(std::abs(f.lo_leak_dbc - c.leak_dbc) < 1.5, n + ": carrier leakage (" + std::to_string(f.lo_leak_dbc) + " dBc)");
            check(std::abs(f.iq_image_db - c.image_db) < 1.5, n + ": I/Q image (" + std::to_string(f.iq_image_db) + " dB)");
            check(f.snr_db > c.snr_db - 4, n + ": SNR (" + std::to_string(f.snr_db) + " dB)");
        }
    }
    {
        std::vector<std::vector<C>> one(1, std::vector<C>(3, C(1, 0)));
        check(ofdm_features(one, one, {C(1, 0), C(1, 0), C(1, 0)}, {-1, 0, 1}, {0.0}, 64, fs, 0, 0, 0).method.empty(), "one symbol: no fit");
    }

    // DSSS: DBPSK-like polarity, 1 us symbols, 2.5 kHz offset, 400 symbols.
    {
        std::vector<std::complex<double>> s;
        std::vector<int8_t> pol;
        for (int k = 0; k < 400; ++k) {
            const int8_t p = (rng() & 1) ? 1 : -1;
            pol.push_back(p);
            s.push_back(double(p) * std::polar(1.0, 2 * pi * 2500.0 * k * 1e-6 + 0.3) + 0.05 * std::complex<double>(noise(rng), noise(rng)));
        }
        const auto f = dsss_features(s, pol, 1e6);
        check(f.method == "DSSS data-aided" && std::abs(f.cfo_hz - 2500.0) < 10.0 && f.snr_db > 20, "DSSS: carrier offset from the whole frame (" + std::to_string(f.cfo_hz) + " Hz)");
    }
    std::cout << checks - failures << "/" << checks << " checks passed\n";
    return failures ? 1 : 0;
}
