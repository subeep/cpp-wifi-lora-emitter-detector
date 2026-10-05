#include "spectral_correlation.hpp"
#include "iq_file.hpp"

#include <kissfft/kiss_fft.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
constexpr double pi = 3.14159265358979323846;
bool supported_fft(std::size_t n) {
    if (n < 32 || n > 4096 || n % 2) return false;
    for (auto p : {2u, 3u, 5u}) while (n % p == 0) n /= p;
    return n == 1;
}
int raw_bin(int signed_bin, int n) { return signed_bin < 0 ? signed_bin + n : signed_bin; }
struct FftFree { void operator()(kiss_fft_state* p) const { kiss_fft_free(p); } };
} // namespace

SpectralFeatures measure_spectral_correlation(
    const std::vector<std::complex<float>>& iq, const SpectralConfig& c) {
    if (!std::isfinite(c.sample_rate_hz) || c.sample_rate_hz <= 0 || c.sample_rate_hz > 1e9)
        throw std::invalid_argument("sample rate must be finite in (0, 1e9]");
    if (!supported_fft(c.fft_size))
        throw std::invalid_argument("FFT size must be even, 32..4096, with factors 2/3/5 only");
    if (!c.max_frames || c.max_frames > 1024 || !c.max_alpha_bins ||
        c.max_alpha_bins > std::min<std::size_t>(128, c.fft_size / 2) ||
        !c.peak_count || c.peak_count > 32 ||
        !std::isfinite(c.pair_power_floor_relative) ||
        c.pair_power_floor_relative <= 0 || c.pair_power_floor_relative >= 1)
        throw std::invalid_argument("invalid estimator resource limits or pair-power floor");
    if (iq.size() > max_window_samples) throw std::invalid_argument("IQ window exceeds 8 MiB");
    const int n = int(c.fft_size);
    std::size_t hop = c.hop_samples;
    if (!hop) {
        hop = c.fft_size / 2 + 1;
        while (std::gcd(hop, c.fft_size) != 1) ++hop;
    }
    if (!hop || hop > c.fft_size) throw std::invalid_argument("hop must be in 1..FFT size");
    // STFT frame-time sampling aliases cyclic frequencies separated by Fs/H.
    // Disallow ambiguous delta bins in this coarse alpha-grid prototype.
    const auto alias_period_bins = c.fft_size / std::gcd(hop, c.fft_size);
    if (c.max_alpha_bins >= alias_period_bins)
        throw std::invalid_argument("hop aliases requested alpha bins; use coprime hop");
    SpectralFeatures r;
    r.fft_size = c.fft_size; r.hop_samples = hop;
    r.bin_hz = c.sample_rate_hz / double(n);
    if (iq.size() < c.fft_size) return r;
    r.frames = std::min(c.max_frames, 1 + (iq.size() - c.fft_size) / hop);
    if (r.frames * c.fft_size * c.max_alpha_bins > 64000000)
        throw std::invalid_argument("estimator exceeds 64 million pair operations; reduce frames/alpha bins");
    r.samples_used = (r.frames - 1) * hop + c.fft_size;
    if (r.samples_used < iq.size()) r.warnings.push_back("window_tail_not_analyzed");
    if (r.frames < 16) r.warnings.push_back("few_overlapping_averages");
    r.warnings.push_back("overlapping_frames_are_not_independent_trials");
    std::complex<double> mean{};
    for (std::size_t i = 0; i < r.samples_used; ++i) {
        if (!std::isfinite(iq[i].real()) || !std::isfinite(iq[i].imag()))
            throw std::invalid_argument("non-finite IQ");
        auto z = std::complex<double>(iq[i]);
        mean += z;
        r.mean_power += std::norm(z);
        r.max_component_abs = std::max(r.max_component_abs,
                                      std::max(std::abs(z.real()), std::abs(z.imag())));
    }
    mean /= double(r.samples_used);
    r.mean_i = mean.real(); r.mean_q = mean.imag();
    r.mean_power /= double(r.samples_used);
    for (std::size_t i = 0; i < r.samples_used; ++i)
        r.variance_power += std::norm(std::complex<double>(iq[i]) - mean);
    r.variance_power /= double(r.samples_used);
    r.dc_fraction = r.mean_power > 0 ? std::clamp(std::norm(mean) / r.mean_power, 0.0, 1.0) : 0;
    if (r.variance_power <= r.mean_power * 1e-12 || r.variance_power == 0) {
        r.quality = "no_variation";
        return r;
    }
    r.quality = r.frames < 16 ? "insufficient_averages" : "measured";
    if (r.dc_fraction > 0.5) r.warnings.push_back("large_dc_component_removed");
    const double rms = std::sqrt(r.variance_power);
    std::vector<double> window(n), power(n);
    double window_energy = 0;
    for (int j = 0; j < n; ++j) {
        window[j] = 0.5 - 0.5 * std::cos(2 * pi * j / n); // periodic Hann
        window_energy += window[j] * window[j];
    }
    const double fft_scale = 1.0 / std::sqrt(window_energy);
    std::vector<kiss_fft_cpx> input(n), output(n);
    std::vector<std::complex<double>> cross((c.max_alpha_bins + 1) * n);
    std::unique_ptr<kiss_fft_state, FftFree> fft(kiss_fft_alloc(n, 0, nullptr, nullptr));
    if (!fft) throw std::runtime_error("FFT allocation failed");
    for (std::size_t m = 0; m < r.frames; ++m) {
        const auto start = m * hop;
        for (int j = 0; j < n; ++j) {
            auto z = (std::complex<double>(iq[start + j]) - mean) / rms * window[j];
            input[j].r = float(z.real()); input[j].i = float(z.imag());
        }
        kiss_fft(fft.get(), input.data(), output.data());
        for (int k = 0; k < n; ++k) {
            output[k].r *= float(fft_scale); output[k].i *= float(fft_scale);
            power[k] += double(output[k].r) * output[k].r + double(output[k].i) * output[k].i;
        }
        for (int delta = 1; delta <= int(c.max_alpha_bins); ++delta) {
            // Local FFT phases include alpha*frame_start. Remove this phase
            // before averaging; omitting it erases genuine cyclic structure.
            const double phase = -2 * pi * double((std::size_t(delta) * start) % c.fft_size) / n;
            const std::complex<double> rotation(std::cos(phase), std::sin(phase));
            for (int low = -n / 2; low + delta < n / 2; ++low) {
                const int lo = raw_bin(low, n), hi = raw_bin(low + delta, n);
                const auto a = std::complex<double>(output[hi].r, output[hi].i);
                const auto b = std::complex<double>(output[lo].r, output[lo].i);
                cross[std::size_t(delta) * n + std::size_t(low + n / 2)] += a * std::conj(b) * rotation;
            }
        }
    }
    for (auto& p : power) p /= double(r.frames);
    const double total = std::accumulate(power.begin(), power.end(), 0.0);
    if (!(total > 0) || !std::isfinite(total)) throw std::runtime_error("invalid FFT power");
    const double max_power = *std::max_element(power.begin(), power.end());
    const double floor = max_power * c.pair_power_floor_relative;
    r.strongest_bin_fraction = max_power / total;
    if (r.strongest_bin_fraction > 0.5) r.warnings.push_back("concentrated_spectrum_tone_or_narrowband");
    double cumulative = 0;
    int low_edge = -n / 2, high_edge = n / 2 - 1;
    bool found_low = false;
    for (int k = -n / 2; k < n / 2; ++k) {
        const double p = power[raw_bin(k, n)];
        cumulative += p / total;
        if (!found_low && cumulative >= 0.005) { low_edge = k; found_low = true; }
        if (cumulative >= 0.995) { high_edge = k; break; }
    }
    // Finish power/flatness over every FFT bin, including beyond the 99% edge.
    double log_sum = 0;
    for (int k = -n / 2; k < n / 2; ++k) {
        const double p = power[raw_bin(k, n)];
        r.power_fraction.push_back(p / total);
        log_sum += std::log(std::max(p, total * 1e-30));
    }
    r.spectral_flatness = std::clamp(std::exp(log_sum / n) / (total / n), 0.0, 1.0);
    r.occupied_low_hz = (low_edge - 0.5) * r.bin_hz;
    r.occupied_high_hz = (high_edge + 0.5) * r.bin_hz;
    r.occupied_low_hz = std::max(r.occupied_low_hz, -c.sample_rate_hz / 2);
    r.occupied_high_hz = std::min(r.occupied_high_hz, c.sample_rate_hz / 2);
    std::vector<CyclicPeak> candidates;
    for (int delta = 1; delta <= int(c.max_alpha_bins); ++delta) {
        for (int low = -n / 2; low + delta < n / 2; ++low) {
            const auto lo = raw_bin(low, n), hi = raw_bin(low + delta, n);
            if (power[lo] < floor || power[hi] < floor) continue;
            auto value = cross[std::size_t(delta) * n + std::size_t(low + n / 2)] / double(r.frames);
            candidates.push_back({delta * r.bin_hz, (low + delta / 2.0) * r.bin_hz,
                                 std::clamp(std::norm(value) / (power[lo] * power[hi]), 0.0, 1.0),
                                 std::abs(value)});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.coherence_squared != b.coherence_squared) return a.coherence_squared > b.coherence_squared;
        if (a.alpha_hz != b.alpha_hz) return a.alpha_hz < b.alpha_hz;
        return a.frequency_offset_hz < b.frequency_offset_hz;
    });
    // Suppress adjacent frequency cells from one ridge for a compact feature set.
    for (const auto& peak : candidates) {
        bool adjacent = false;
        for (const auto& kept : r.peaks)
            if (peak.alpha_hz == kept.alpha_hz &&
                std::abs(peak.frequency_offset_hz - kept.frequency_offset_hz) <= 2 * r.bin_hz)
                adjacent = true;
        if (!adjacent) r.peaks.push_back(peak);
        if (r.peaks.size() == c.peak_count) break;
    }
    return r;
}
} // namespace rfmon::cyclo
