#include "analysis_samples.hpp"
#include "iq_file.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace rfmon::cyclo {
namespace { constexpr double pi = 3.14159265358979323846; }

PreparedSamples select_analysis_band(const std::vector<std::complex<float>>& iq,
                                     const BandSelection& c) {
    if (!std::isfinite(c.sample_rate_hz) || c.sample_rate_hz <= 0 || c.sample_rate_hz > 1e9 ||
        !std::isfinite(c.source_usable_bandwidth_hz) || c.source_usable_bandwidth_hz <= 0 ||
        c.source_usable_bandwidth_hz > c.sample_rate_hz || !std::isfinite(c.offset_hz) ||
        !std::isfinite(c.passband_width_hz) || c.passband_width_hz <= 0 ||
        !c.decimation || c.decimation > 16 || iq.size() > max_window_samples)
        throw std::invalid_argument("invalid band selection or input size");
    PreparedSamples r;
    for (auto z : iq) {
        if (!std::isfinite(z.real()) || !std::isfinite(z.imag()))
            throw std::invalid_argument("non-finite IQ before band selection");
        if (c.remove_source_mean) r.removed_source_mean += std::complex<double>(z);
    }
    if (c.remove_source_mean && !iq.empty()) r.removed_source_mean /= double(iq.size());
    r.sample_rate_hz = c.sample_rate_hz / c.decimation;
    r.input_step = c.decimation;
    if (c.passband_width_hz > 0.8 * r.sample_rate_hz)
        throw std::invalid_argument("ROI passband needs at least 20% output-rate guard space");
    const double pass_edge = c.passband_width_hz / 2;
    r.stopband_edge_hz = std::min(r.sample_rate_hz / 2,
                                  c.source_usable_bandwidth_hz / 2 - std::abs(c.offset_hz));
    const double transition = r.stopband_edge_hz - pass_edge;
    if (!(transition > 0))
        throw std::invalid_argument("ROI and filter transition must fit the declared source passband");
    const double requested_taps = std::ceil(3.3 * c.sample_rate_hz / transition);
    if (!std::isfinite(requested_taps) || requested_taps > 1023)
        throw std::invalid_argument("transition needs more than 1023 FIR taps");
    r.filter_taps = std::max<std::size_t>(33, std::size_t(requested_taps));
    if (r.filter_taps % 2 == 0) ++r.filter_taps;
    r.first_input_center = r.filter_taps / 2;
    if (iq.size() < r.filter_taps)
        throw std::invalid_argument("insufficient samples for the requested band-selection filter");
    const auto output_count = 1 + (iq.size() - r.filter_taps) / c.decimation;
    if (output_count * r.filter_taps > 64000000)
        throw std::invalid_argument("band selection exceeds 64 million FIR operations");
    const double cutoff = (pass_edge + r.stopband_edge_hz) / (2 * c.sample_rate_hz);
    std::vector<double> coefficients(r.filter_taps);
    for (std::size_t j = 0; j < r.filter_taps; ++j) {
        const double t = double(j) - r.first_input_center;
        const double sinc = t == 0 ? 2 * cutoff : std::sin(2 * pi * cutoff * t) / (pi * t);
        coefficients[j] = sinc * (0.54 - 0.46 * std::cos(2 * pi * j / (r.filter_taps - 1)));
    }
    const double dc_gain = std::accumulate(coefficients.begin(), coefficients.end(), 0.0);
    for (auto& value : coefficients) value /= dc_gain;
    std::vector<std::complex<double>> mixed(iq.size());
    const double omega = -2 * pi * c.offset_hz / c.sample_rate_hz;
    const auto advance = std::polar(1.0, omega);
    std::complex<double> oscillator(1, 0);
    for (std::size_t j = 0; j < iq.size(); ++j) {
        if (!std::isfinite(iq[j].real()) || !std::isfinite(iq[j].imag()))
            throw std::invalid_argument("non-finite IQ before band selection");
        if (j % 4096 == 0) oscillator = std::polar(1.0, omega * j);
        mixed[j] = (std::complex<double>(iq[j]) - r.removed_source_mean) * oscillator;
        oscillator *= advance;
    }
    r.samples.reserve(output_count);
    for (std::size_t m = 0; m < output_count; ++m) {
        std::complex<double> value{};
        const auto first = m * c.decimation;
        for (std::size_t j = 0; j < r.filter_taps; ++j)
            value += mixed[first + j] * coefficients[j];
        auto output = std::complex<float>(value);
        if (!std::isfinite(output.real()) || !std::isfinite(output.imag()))
            throw std::invalid_argument("band-selection output exceeds float32 range");
        r.samples.push_back(output);
    }
    return r;
}
} // namespace rfmon::cyclo
