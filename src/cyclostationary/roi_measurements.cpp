#include "roi_measurements.hpp"
#include "sample_tiles.hpp"
#include <kissfft/kiss_fft.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
constexpr double pi = 3.14159265358979323846;
struct FftFree { void operator()(kiss_fft_state* p) const { kiss_fft_free(p); } };
double quantile20(std::vector<double> values) {
    const auto index = (values.size() - 1) / 5;
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index];
}
void spectrum(EnergyRegion& r, const std::vector<std::complex<float>>& iq, double scale, double rate) {
    if (r.samples < roi_fft_size) return;
    std::complex<double> mean{};
    double raw = 0;
    for (std::size_t j = 0; j < r.samples; ++j) {
        const auto z = std::complex<double>(iq[r.offset + j]) / scale;
        mean += z; raw += std::norm(z);
    }
    mean /= double(r.samples);
    double variance = 0;
    for (std::size_t j = 0; j < r.samples; ++j)
        variance += std::norm(std::complex<double>(iq[r.offset + j]) / scale - mean);
    if (!variance || variance <= raw * 1e-12) { r.spectral_status = "no_variation"; return; }
    std::unique_ptr<kiss_fft_state, FftFree> fft(kiss_fft_alloc(roi_fft_size, 0, nullptr, nullptr));
    if (!fft) throw std::runtime_error("ROI FFT allocation failed");
    std::array<kiss_fft_cpx, roi_fft_size> input{}, output{};
    std::array<double, roi_fft_size> powers{}, hann{};
    const double rms = std::sqrt(variance / r.samples);
    for (std::size_t j = 0; j < roi_fft_size; ++j) hann[j] = 0.5 - 0.5 * std::cos(2 * pi * j / roi_fft_size);
    // Non-overlapping frame count; evenly distribute frames through the ROI.
    // These ordinary PSD frames are never phase-concatenated for cyclic work.
    r.spectral_frames = std::min(roi_frame_limit, r.samples / roi_fft_size);
    r.spectral_samples = r.spectral_frames * roi_fft_size;
    for (std::size_t frame = 0; frame < r.spectral_frames; ++frame) {
        const auto span = r.samples - roi_fft_size;
        const auto offset = r.spectral_frames > 1 ? frame * span / (r.spectral_frames - 1) : span / 2;
        for (std::size_t j = 0; j < roi_fft_size; ++j) {
            const auto z = (std::complex<double>(iq[r.offset + offset + j]) / scale - mean) *
                           (hann[j] / rms);
            input[j] = {float(z.real()), float(z.imag())};
        }
        kiss_fft(fft.get(), input.data(), output.data());
        for (std::size_t k = 0; k < roi_fft_size; ++k) {
            const auto& z = output[(k + roi_fft_size / 2) % roi_fft_size];
            powers[k] += double(z.r) * z.r + double(z.i) * z.i;
        }
    }
    const double total = std::accumulate(powers.begin(), powers.end(), 0.0);
    if (!(total > 0) || !std::isfinite(total)) { r.spectral_status = "no_variation"; return; }
    r.spectral_status = r.spectral_frames < 8 ? "few_frames" : "measured";
    const double bin = rate / roi_fft_size;
    const auto edge = [&](std::size_t k, bool high) {
        return std::clamp((double(k) - roi_fft_size / 2 + (high ? 0.5 : -0.5)) * bin, -rate / 2, rate / 2);
    };
    double accumulated = 0; bool low = false;
    for (std::size_t k = 0; k < roi_fft_size; ++k) {
        accumulated += powers[k] / total;
        if (!low && accumulated >= 0.005) { low = true; r.occupied_low_hz = edge(k, false); }
        if (accumulated >= 0.995) { r.occupied_high_hz = edge(k, true); break; }
    }
    const double baseline = std::max(quantile20(std::vector<double>(powers.begin(), powers.end())), total * 1e-12);
    for (std::size_t k = 0; k < roi_fft_size;) {
        if (powers[k] < baseline * 4) { ++k; continue; }
        const auto start = k; double power = 0; bool strong = false;
        while (k < roi_fft_size && powers[k] >= baseline * 4) {
            power += powers[k]; strong = strong || powers[k] >= baseline * 8; ++k;
        }
        if (!strong) continue;
        ++r.bands_seen;
        FrequencyInterval b{edge(start, false), edge(k - 1, true), std::clamp(power / total, 0.0, 1.0),
                            start == 0 || k == roi_fft_size, start <= roi_fft_size / 2 && k > roi_fft_size / 2};
        r.bands.push_back(b);
        std::sort(r.bands.begin(), r.bands.end(), [](const auto& a, const auto& b) {
            return a.power_fraction != b.power_fraction ? a.power_fraction > b.power_fraction : a.low_hz < b.low_hz;
        });
        if (r.bands.size() > roi_band_limit) r.bands.pop_back();
    }
    std::sort(r.bands.begin(), r.bands.end(), [](const auto& a, const auto& b) { return a.low_hz < b.low_hz; });
}
} // namespace

RoiMeasurements measure_rois(const std::vector<std::complex<float>>& iq, double rate) {
    if (!std::isfinite(rate) || rate <= 0 || rate > 1e9 || iq.size() > tile_sample_limit)
        throw std::invalid_argument("invalid ROI rate or window limit");
    RoiMeasurements r; r.samples_examined = iq.size();
    double scale = 0;
    for (const auto& z : iq) {
        if (!std::isfinite(z.real()) || !std::isfinite(z.imag())) throw std::invalid_argument("non-finite ROI IQ");
        scale = std::max(scale, std::max(std::abs(double(z.real())), std::abs(double(z.imag()))));
    }
    if (iq.size() < roi_block_samples) return r;
    r.blocks = (iq.size() + roi_block_samples - 1) / roi_block_samples;
    if (!scale) { r.status = "no_variation"; return r; }
    std::complex<double> mean{}; double raw = 0;
    for (const auto& sample : iq) { const auto z = std::complex<double>(sample) / scale; mean += z; raw += std::norm(z); }
    mean /= double(iq.size());
    r.dc_fraction = raw ? std::clamp(std::norm(mean) * iq.size() / raw, 0.0, 1.0) : 0;
    std::vector<double> powers(r.blocks); double total = 0;
    for (std::size_t b = 0; b < r.blocks; ++b) {
        const auto start = b * roi_block_samples, end = std::min(iq.size(), start + roi_block_samples);
        for (auto j = start; j < end; ++j) powers[b] += std::norm(std::complex<double>(iq[j]) / scale - mean);
        total += powers[b]; powers[b] /= double(end - start);
    }
    if (!total || total <= raw * 1e-12) { r.status = "no_variation"; return r; }
    const double average = total / iq.size(), background = quantile20(powers);
    r.background_to_mean = background / average;
    const double baseline = std::max(background, average * 1e-12);
    r.high_threshold_to_mean = 4 * baseline / average;
    for (std::size_t b = 0; b < r.blocks;) {
        if (powers[b] < baseline * 2) { ++b; continue; }
        const auto first = b; bool strong = false; double energy = 0;
        while (b < r.blocks && powers[b] >= baseline * 2) {
            const auto count = std::min(roi_block_samples, iq.size() - b * roi_block_samples);
            energy += powers[b] * count; strong = strong || powers[b] >= baseline * 4; ++b;
        }
        if (!strong) continue;
        EnergyRegion region; region.offset = first * roi_block_samples;
        region.samples = std::min(iq.size(), b * roi_block_samples) - region.offset;
        region.energy_fraction = std::clamp(energy / total, 0.0, 1.0); region.contrast_selected = true;
        region.touches_window_edge = region.offset == 0 || region.offset + region.samples == iq.size();
        ++r.regions_seen; r.contrast_samples += region.samples; r.regions.push_back(region);
        std::sort(r.regions.begin(), r.regions.end(), [](const auto& a, const auto& b) {
            return a.energy_fraction != b.energy_fraction ? a.energy_fraction > b.energy_fraction : a.offset < b.offset;
        });
        if (r.regions.size() > roi_region_limit) r.regions.pop_back();
    }
    r.status = r.regions_seen ? "contrast_regions" : "uniform_energy";
    if (r.regions.empty()) {
        EnergyRegion context; context.samples = iq.size(); context.energy_fraction = 1; context.touches_window_edge = true;
        r.regions.push_back(context); // preserve continuous/unsegmented emissions
    }
    std::sort(r.regions.begin(), r.regions.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    for (auto& region : r.regions) spectrum(region, iq, scale, rate);
    return r;
}
} // namespace rfmon::cyclo
