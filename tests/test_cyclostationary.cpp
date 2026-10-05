#include "cyclostationary/iq_file.hpp"
#include "cyclostationary/analysis_samples.hpp"
#include "cyclostationary/spectral_correlation.hpp"
#include "cyclostationary/ofdm_structure.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

using namespace rfmon::cyclo;
namespace fs = std::filesystem;
namespace {
constexpr double pi = 3.14159265358979323846;
int failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
template<class F> void rejects(F f, const std::string& message) {
    try { f(); check(false, message); } catch (const std::exception&) {}
}
std::vector<std::complex<float>> noise(std::size_t count, unsigned seed) {
    std::mt19937 engine(seed);
    std::normal_distribution<float> normal;
    std::vector<std::complex<float>> x(count);
    for (auto& z : x) z = {normal(engine), normal(engine)};
    return x;
}

// Independent oracle: direct DFT using absolute sample coordinates, without
// the implementation's local-FFT phase correction. Normalization cancels in
// coherence. This catches wrong conjugation, FFT order and frame-time phase.
double direct_coherence(const std::vector<std::complex<float>>& x,
                        const SpectralFeatures& r, int low, int delta) {
    std::complex<double> mean{};
    for (std::size_t j = 0; j < r.samples_used; ++j) mean += std::complex<double>(x[j]);
    mean /= double(r.samples_used);
    std::complex<double> cross{};
    double a_power = 0, b_power = 0;
    for (std::size_t frame = 0; frame < r.frames; ++frame) {
        std::complex<double> a{}, b{};
        const auto start = frame * r.hop_samples;
        for (std::size_t j = 0; j < r.fft_size; ++j) {
            const double w = (1 - std::cos(2 * pi * j / r.fft_size)) / 2;
            const auto z = (std::complex<double>(x[start + j]) - mean) * w;
            const double t = double(start + j) / r.fft_size;
            a += z * std::polar(1.0, -2 * pi * (low + delta) * t);
            b += z * std::polar(1.0, -2 * pi * low * t);
        }
        cross += a * std::conj(b);
        a_power += std::norm(a); b_power += std::norm(b);
    }
    return std::norm(cross) / (a_power * b_power);
}

void math_tests() {
    SpectralConfig c;
    c.sample_rate_hz = 64000; c.fft_size = 64; c.max_alpha_bins = 16;
    auto x = noise(40000, 713);
    auto white = measure_spectral_correlation(x, c);
    check(white.quality == "measured", "white-noise measurements available");
    check(white.spectral_flatness > 0.9, "white-noise flatness");
    check(white.strongest_bin_fraction < 0.03, "white-noise spectral concentration");
    check(!white.peaks.empty() && white.peaks.front().coherence_squared < 0.08,
          "stationary noise does not give a strong cyclic coherence peak");
    check(std::abs(std::accumulate(white.power_fraction.begin(), white.power_fraction.end(), 0.0) - 1) < 1e-12,
          "ordinary spectrum fractions sum to one");

    // Two tones at -4 and +7 bins imply exactly alpha=11*Fs/N and
    // a cross-frequency midpoint of 1.5*Fs/N. No vendor meaning is assumed.
    std::vector<std::complex<float>> tones(40000);
    for (std::size_t i = 0; i < tones.size(); ++i)
        tones[i] = std::complex<float>(std::polar(1.0, -2 * pi * 4 * i / 64) +
                                      std::polar(0.7, 2 * pi * 7 * i / 64));
    auto measured = measure_spectral_correlation(tones, c);
    bool recovered = false;
    for (const auto& peak : measured.peaks)
        recovered |= std::abs(peak.alpha_hz - 11000) < 1e-9 &&
                     std::abs(peak.frequency_offset_hz - 1500) <= 2000 &&
                     peak.coherence_squared > 0.99;
    check(recovered, "known cyclic frequency and frequency midpoint recovered");

    auto am = noise(9000, 911);
    for (std::size_t i = 0; i < am.size(); ++i)
        am[i] *= float(1 + 0.85 * std::cos(2 * pi * 4 * i / 64));
    c.max_frames = 200;
    auto cyclic = measure_spectral_correlation(am, c);
    check(!cyclic.peaks.empty() && cyclic.peaks.front().coherence_squared > 0.15,
          "periodically modulated noise has stronger cyclic structure");
    for (const auto& peak : cyclic.peaks) {
        const int delta = int(std::llround(peak.alpha_hz / cyclic.bin_hz));
        const int low = int(std::llround(peak.frequency_offset_hz / cyclic.bin_hz - delta / 2.0));
        const double expected = direct_coherence(am, cyclic, low, delta);
        check(std::abs(expected - peak.coherence_squared) < 2e-6,
              "FFT estimator agrees with absolute-time direct DFT oracle");
    }
    auto amplified = am;
    for (auto& z : amplified) z *= 1e25f;
    auto scaled = measure_spectral_correlation(amplified, c);
    check(std::abs(scaled.peaks.front().coherence_squared - cyclic.peaks.front().coherence_squared) < 2e-6,
          "large finite amplitude does not overflow and coherence is scale invariant");

    for (std::size_t i = 0; i < tones.size(); ++i)
        tones[i] = std::complex<float>(std::polar(1.0, 2 * pi * 5 * i / 64));
    c.max_frames = 1024;
    auto single = measure_spectral_correlation(tones, c);
    check(single.strongest_bin_fraction > 0.6, "single tone flagged by concentration");
    check(single.peaks.empty() || single.peaks.front().coherence_squared < 0.02,
          "single tone leakage does not masquerade as strong nonzero-alpha structure");

    std::vector<std::complex<float>> dc(1024, {2, -3});
    auto constant = measure_spectral_correlation(dc, c);
    check(constant.quality == "no_variation" && constant.peaks.empty(), "DC-only input abstains");
    check(measure_spectral_correlation(std::vector<std::complex<float>>(16), c).quality == "insufficient_samples",
          "short window reports insufficient samples");
    auto bad = noise(1024, 88); bad[17] = {NAN, 0};
    rejects([&] { measure_spectral_correlation(bad, c); }, "non-finite IQ rejected");
    auto invalid = c; invalid.hop_samples = 32;
    rejects([&] { measure_spectral_correlation(x, invalid); }, "hop/alpha aliases rejected");
    invalid = c; invalid.fft_size = 4093;
    rejects([&] { measure_spectral_correlation(x, invalid); }, "expensive unsupported FFT factorization rejected");
    invalid = c; invalid.fft_size = 4096; invalid.hop_samples = 1; invalid.max_alpha_bins = 128;
    rejects([&] { measure_spectral_correlation(std::vector<std::complex<float>>(6000), invalid); },
            "pair-operation budget enforced");
    invalid = c; invalid.sample_rate_hz = INFINITY;
    rejects([&] { measure_spectral_correlation(x, invalid); }, "infinite sample rate rejected");
    c.fft_size = 480; c.max_alpha_bins = 32;
    check(measure_spectral_correlation(x, c).fft_size == 480, "mixed-radix grid supported");
}

void input_tests(const fs::path& dir) {
    // Fixed wire representations, independent of host byte order.
    const unsigned char f32[] = {0,0,128,63, 0,0,0,191, 0,0,0,64, 0,0,0,192};
    auto float_path = dir / "valid.dat";
    { std::ofstream f(float_path, std::ios::binary); f.write(reinterpret_cast<const char*>(f32), sizeof(f32)); }
    auto f = read_iq_window(float_path, IqFormat::cf32_le, 0, 10);
    check(f.file_samples == 2 && f.samples.size() == 2, "bounded read clips to actual file end");
    check(f.samples[0] == std::complex<float>(1, -0.5), "little-endian float32 decoded");
    check(read_iq_window(float_path, IqFormat::cf32_le, 1, 1).samples[0] == std::complex<float>(2, -2),
          "offset is in complex samples");
    rejects([&] { read_iq_window(float_path, IqFormat::cf32_le, 2, 1); }, "offset at EOF rejected");
    rejects([&] { read_iq_window(float_path, IqFormat::cf32_le, 0, max_window_samples + 1); }, "allocation cap enforced");
    rejects([&] { read_iq_window(dir, IqFormat::cf32_le, 0, 1); }, "directory rejected as IQ input");
    const unsigned char i16[] = {0,128, 255,127, 0,64, 0,192};
    auto int_path = dir / "valid.bin";
    { std::ofstream o(int_path, std::ios::binary); o.write(reinterpret_cast<const char*>(i16), sizeof(i16)); }
    auto s = read_iq_window(int_path, IqFormat::ci16_le, 0, 2);
    check(s.samples[0].real() == -1 && s.samples[0].imag() == 32767.0f/32768,
          "signed int16 boundary decoding");
    check(s.samples[1] == std::complex<float>(0.5f, -0.5f), "int16 IQ order correct");
    { std::ofstream o(dir / "odd.dat", std::ios::binary); o << 'x'; }
    rejects([&] { read_iq_window(dir / "odd.dat", IqFormat::cf32_le, 0, 2); }, "partial complex sample rejected");
    const unsigned char nan[] = {0,0,192,127, 0,0,0,0};
    { std::ofstream o(dir / "nan.dat", std::ios::binary); o.write(reinterpret_cast<const char*>(nan), sizeof(nan)); }
    rejects([&] { read_iq_window(dir / "nan.dat", IqFormat::cf32_le, 0, 1); }, "non-finite file sample rejected");
}

void band_tests() {
    BandSelection c;
    c.sample_rate_hz = 1e6; c.source_usable_bandwidth_hz = 1e6;
    c.offset_hz = 125000; c.passband_width_hz = 80000; c.decimation = 4;
    std::vector<std::complex<float>> x(16000);
    for (std::size_t j = 0; j < x.size(); ++j)
        x[j] = std::complex<float>(std::polar(1.0, 2 * pi * 145000 * j / 1e6));
    const auto original = x;
    auto pass = select_analysis_band(x, c);
    check(pass.sample_rate_hz == 250000 && pass.input_step == 4, "decimation preserves physical sample rate");
    check(pass.filter_taps % 2 == 1 && pass.first_input_center == pass.filter_taps / 2,
          "centered FIR coordinates explicit");
    check(x == original, "band selection leaves input IQ unchanged");
    check(pass.samples.size() == 1 + (x.size() - pass.filter_taps) / 4, "only fully supported FIR outputs emitted");
    double max_error = 0;
    for (std::size_t m = 0; m < pass.samples.size(); ++m) {
        const auto coordinate = pass.first_input_center + m * pass.input_step;
        const auto expected = std::polar(1.0, 2 * pi * 20000 * coordinate / 1e6);
        max_error = std::max(max_error, std::abs(std::complex<double>(pass.samples[m]) - expected));
    }
    check(max_error < 0.02, "passband tone frequency, amplitude and phase survive downmix/decimation");
    // Explicit pre-mix removal equals subtracting the mean by hand first.
    auto with_dc = original;
    for (auto& z : with_dc) z += std::complex<float>(2, -3);
    std::complex<double> mean{};
    for (auto z : with_dc) mean += std::complex<double>(z);
    mean /= double(with_dc.size());
    auto manual = with_dc;
    for (auto& z : manual) z = std::complex<float>(std::complex<double>(z) - mean);
    auto dc_selection = c; dc_selection.remove_source_mean = true;
    auto automatic = select_analysis_band(with_dc, dc_selection);
    const auto oracle = select_analysis_band(manual, c);
    check(std::abs(automatic.removed_source_mean - mean) < 1e-12, "pre-mix mean recorded exactly");
    max_error = 0;
    for (std::size_t i = 0; i < oracle.samples.size(); ++i)
        max_error = std::max(max_error, double(std::abs(automatic.samples[i] - oracle.samples[i])));
    check(max_error < 1e-6, "explicit pre-mix DC removal matches independent source subtraction");
    check(select_analysis_band(with_dc, c).removed_source_mean == std::complex<double>{}, "default preprocessing unchanged");
    // Without an anti-alias filter this 200 kHz translated tone aliases to
    // -50 kHz at 250 ksps, with unit amplitude. Require strong attenuation.
    for (std::size_t j = 0; j < x.size(); ++j)
        x[j] = std::complex<float>(std::polar(1.0, 2 * pi * 325000 * j / 1e6));
    auto blocked = select_analysis_band(x, c);
    double power = 0;
    for (auto z : blocked.samples) power += std::norm(z);
    check(std::sqrt(power / blocked.samples.size()) < 0.005, "out-of-band alias suppressed by more than 46 dB");
    auto invalid = c; invalid.passband_width_hz = 240000;
    rejects([&] { select_analysis_band(x, invalid); }, "filter guard space enforced");
    invalid = c; invalid.offset_hz = 480000;
    rejects([&] { select_analysis_band(x, invalid); }, "requested ROI cannot cross source passband edge");
    invalid = c; invalid.decimation = 0;
    rejects([&] { select_analysis_band(x, invalid); }, "zero decimation rejected");
    rejects([&] { select_analysis_band(std::vector<std::complex<float>>(4), c); },
            "short input cannot be padded into a filtered window");
    x[5] = {INFINITY, 0};
    rejects([&] { select_analysis_band(x, c); }, "non-finite input rejected before filtering");
}
void ofdm_tests() {
    // Independent transmitter: direct inverse DFT of random QPSK carriers,
    // then copy the tail into the prefix. No folding/correlation code reused.
    auto random = noise(100 * 64, 7001);
    auto x = noise(37, 7002);
    for (std::size_t symbol = 0; symbol < 100; ++symbol) {
        std::vector<std::complex<float>> time(64);
        for (std::size_t n = 0; n < 64; ++n) {
            std::complex<double> z{};
            for (int k = -26; k <= 26; ++k) if (k) {
                const auto q = random[symbol * 64 + std::size_t(k + 26)];
                z += std::complex<double>(q.real() > 0 ? 1 : -1, q.imag() > 0 ? 1 : -1) *
                     std::polar(1.0 / std::sqrt(52.0), 2 * pi * k * n / 64);
            }
            time[n] = std::complex<float>(z);
        }
        x.insert(x.end(), time.end() - 16, time.end());
        x.insert(x.end(), time.begin(), time.end());
    }
    auto corrupt = noise(x.size(), 7003);
    for (std::size_t n = 0; n < x.size(); ++n)
        x[n] = (x[n] + 0.06f * corrupt[n]) * std::complex<float>(std::polar(1.0, 0.07 * n));
    const auto original = x;
    const std::vector<OfdmHypothesis> hypotheses = {{"reference", 64, 16}, {"wrong", 96, 24}};
    const auto measured = measure_ofdm_structure(x, 20e6, hypotheses);
    const auto& m = measured[0];
    check(m.status == "measured" && m.prefix_phase_samples == 37, "CP phase recovered from training only");
    check(m.holdout_prefix_coherence_squared > 0.96 && m.holdout_contrast > 0.9,
          "held-out OFDM prefix persists with noise and carrier offset");
    check(m.holdout_symbol_cyclic_coherence_squared > 0.02,
          "fine symbol alpha outside integer FFT grid measured");
    check(measured[1].holdout_contrast < 0.1, "incorrect OFDM timing does not repeat on holdout");
    check(x == original, "OFDM analysis does not mutate capture samples");
    auto huge = x; for (auto& z : huge) z *= 1e25f;
    check(std::abs(measure_ofdm_structure(huge, 20e6, hypotheses)[0].holdout_contrast - m.holdout_contrast) < 1e-6,
          "CP measurement scale invariant without overflow");
    auto white = noise(8000, 7004);
    check(std::abs(measure_ofdm_structure(white, 20e6, hypotheses)[0].holdout_contrast) < 0.02,
          "phase fitting on noise does not create held-out CP contrast");
    for (std::size_t n = 0; n < white.size(); ++n) white[n] = std::complex<float>(std::polar(1.0, 0.2 * n));
    const auto tone = measure_ofdm_structure(white, 20e6, hypotheses)[0];
    check(tone.holdout_prefix_coherence_squared > 0.99 && std::abs(tone.holdout_contrast) < 0.001 &&
          tone.holdout_symbol_cyclic_coherence_squared < 1e-6,
          "pure tone correlates everywhere but has no localized prefix or symbol alpha");
    auto short_window = measure_ofdm_structure(std::vector<std::complex<float>>(100), 20e6, hypotheses);
    check(short_window[0].status == "insufficient_symbols", "short OFDM input abstains");
    check(measure_ofdm_structure(std::vector<std::complex<float>>(8000, {2, 3}), 20e6, hypotheses)[0].status == "no_variation",
          "constant OFDM input abstains");
    const auto bank = wlan_ofdm_hypotheses(20e6);
    check(bank.size() == 5 && bank[0].useful_samples == 64 && bank[4].prefix_samples == 64,
          "WLAN timing hypotheses mapped to exact source samples");
    check(wlan_ofdm_hypotheses(20000001).empty(), "fractional timing skipped rather than rounded");
    rejects([&] { measure_ofdm_structure(x, 20e6, {{"bad", 64, 64}}); }, "invalid prefix rejected");
    rejects([&] { measure_ofdm_structure(x, 20e6, std::vector<OfdmHypothesis>(33)); }, "hypothesis budget bounded");
    x[0] = {NAN, 0};
    rejects([&] { measure_ofdm_structure(x, 20e6, hypotheses); }, "non-finite OFDM input rejected");
}
} // namespace

int main() {
    const auto dir = fs::temp_directory_path() / ("rfmon-cyclo-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        if (!fs::create_directory(dir)) throw std::runtime_error("test directory already exists");
        math_tests(); input_tests(dir); band_tests(); ofdm_tests();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: exception: " << e.what() << '\n'; ++failures;
    }
    fs::remove_all(dir);
    std::cout << "cyclostationary math/input failures=" << failures << '\n';
    return failures ? 1 : 0;
}
