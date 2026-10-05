#include "worker_protocol.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace rfmon::cyclo {
static_assert(sizeof(float) == 4 && sizeof(double) == 8 &&
              std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559,
              "worker wire format requires IEEE float32/float64");
namespace {
void require(bool condition) { if (!condition) throw std::runtime_error("invalid cyclostationary worker message"); }
struct Writer {
    WireBytes bytes;
    void u64(std::uint64_t v, unsigned n = 8) { for (unsigned i = 0; i < n; ++i) bytes.push_back((v >> (i * 8)) & 255); }
    void real(double v) { std::uint64_t bits; std::memcpy(&bits, &v, 8); u64(bits); }
    void sample(float v) { std::uint32_t bits; std::memcpy(&bits, &v, 4); u64(bits, 4); }
    void str(const std::string& s) { require(s.size() <= 128); u64(s.size(), 4); bytes.insert(bytes.end(), s.begin(), s.end()); }
};
struct Reader {
    const WireBytes& bytes; std::size_t at = 0;
    std::uint64_t u64(unsigned n = 8) {
        require(n <= bytes.size() - at); std::uint64_t v = 0;
        for (unsigned i = 0; i < n; ++i) v |= std::uint64_t(bytes[at++]) << (8 * i);
        return v;
    }
    double real(double low = -1e100, double high = 1e100) {
        auto bits = u64(); double v; std::memcpy(&v, &bits, 8);
        require(std::isfinite(v) && v >= low && v <= high); return v;
    }
    float sample() { auto bits = std::uint32_t(u64(4)); float v; std::memcpy(&v, &bits, 4); return v; }
    std::size_t size(std::size_t max, unsigned width = 8) { auto v = u64(width); require(v <= max); return std::size_t(v); }
    std::string str() {
        auto n = size(128, 4); require(n <= bytes.size() - at);
        std::string s(bytes.begin() + at, bytes.begin() + at + n); at += n;
        for (unsigned char c : s) require(c >= 32 && c < 127);
        return s;
    }
    void end() { require(at == bytes.size()); }
};
bool status(const std::string& s, std::initializer_list<const char*> values) {
    return std::any_of(values.begin(), values.end(), [&](const char* v) { return s == v; });
}
void write_spectral(Writer& w, const SpectralFeatures& r) {
    for (auto v : {r.fft_size, r.hop_samples, r.frames, r.samples_used}) w.u64(v);
    for (auto v : {r.bin_hz, r.mean_i, r.mean_q, r.mean_power, r.variance_power, r.dc_fraction,
                   r.max_component_abs, r.spectral_flatness, r.strongest_bin_fraction,
                   r.occupied_low_hz, r.occupied_high_hz}) w.real(v);
    w.str(r.quality); require(r.warnings.size() <= 16); w.u64(r.warnings.size(), 4);
    for (const auto& v : r.warnings) w.str(v);
    require(r.power_fraction.size() <= 512); w.u64(r.power_fraction.size(), 4);
    for (auto v : r.power_fraction) w.real(v);
    require(r.peaks.size() <= 12); w.u64(r.peaks.size(), 4);
    for (const auto& p : r.peaks)
        for (auto v : {p.alpha_hz, p.frequency_offset_hz, p.coherence_squared, p.normalized_cross_magnitude}) w.real(v);
}
SpectralFeatures read_spectral(Reader& q, double rate, std::size_t count) {
    SpectralFeatures r;
    r.fft_size = q.size(512); r.hop_samples = q.size(257); r.frames = q.size(256); r.samples_used = q.size(count);
    require(r.fft_size == 512 && r.hop_samples == 257 && r.frames > 0 &&
            r.frames == std::min<std::size_t>(256, 1 + (count - 512) / 257) &&
            r.samples_used == (r.frames - 1) * 257 + 512);
    r.bin_hz = q.real(0, 1e9); require(r.bin_hz == rate / 512);
    r.mean_i = q.real(); r.mean_q = q.real(); r.mean_power = q.real(0); r.variance_power = q.real(0);
    r.dc_fraction = q.real(0, 1); r.max_component_abs = q.real(0); r.spectral_flatness = q.real(0, 1);
    r.strongest_bin_fraction = q.real(0, 1); r.occupied_low_hz = q.real(-rate / 2, rate / 2);
    r.occupied_high_hz = q.real(r.occupied_low_hz, rate / 2);
    r.quality = q.str(); require(status(r.quality, {"measured", "no_variation", "insufficient_averages"}));
    auto warnings = q.size(16, 4); for (std::size_t i = 0; i < warnings; ++i) r.warnings.push_back(q.str());
    auto powers = q.size(512, 4); require(powers == 0 || powers == 512);
    double sum = 0;
    for (std::size_t i = 0; i < powers; ++i) { const auto p = q.real(0, 1); r.power_fraction.push_back(p); sum += p; }
    require(!powers || std::abs(sum - 1) < 1e-8);
    auto peaks = q.size(12, 4); require(!peaks || powers == 512);
    for (std::size_t i = 0; i < peaks; ++i) {
        CyclicPeak p; p.alpha_hz = q.real(r.bin_hz, 32 * r.bin_hz);
        p.frequency_offset_hz = q.real(-rate / 2, rate / 2); p.coherence_squared = q.real(0, 1);
        p.normalized_cross_magnitude = q.real(0); r.peaks.push_back(p);
    }
    return r;
}
void write_ofdm(Writer& w, const OfdmMeasurement& r) {
    w.str(r.hypothesis.label); w.u64(r.hypothesis.useful_samples); w.u64(r.hypothesis.prefix_samples);
    w.str(r.status);
    for (auto v : {r.samples_used, r.train_symbols, r.holdout_symbols, r.prefix_phase_samples}) w.u64(v);
    for (auto v : {r.symbol_rate_hz, r.train_prefix_coherence_squared, r.holdout_prefix_coherence_squared,
                   r.holdout_outside_coherence_squared, r.holdout_contrast, r.holdout_symbol_cyclic_coherence_squared}) w.real(v);
}
OfdmMeasurement read_ofdm(Reader& q, double rate, std::size_t count, const OfdmHypothesis& expected) {
    OfdmMeasurement r;
    r.hypothesis.label = q.str(); r.hypothesis.useful_samples = q.size(8192); r.hypothesis.prefix_samples = q.size(8192);
    require(r.hypothesis.label == expected.label && r.hypothesis.useful_samples == expected.useful_samples &&
            r.hypothesis.prefix_samples == expected.prefix_samples);
    r.status = q.str(); require(status(r.status, {"measured", "no_variation", "insufficient_symbols"}));
    r.samples_used = q.size(count); r.train_symbols = q.size(count); r.holdout_symbols = q.size(count);
    const auto period = expected.useful_samples + expected.prefix_samples;
    r.prefix_phase_samples = q.size(period - 1); r.symbol_rate_hz = q.real(0, 1e9);
    require(r.symbol_rate_hz == rate / period);
    r.train_prefix_coherence_squared = q.real(0, 1); r.holdout_prefix_coherence_squared = q.real(0, 1);
    r.holdout_outside_coherence_squared = q.real(0, 1); r.holdout_contrast = q.real(-1, 1);
    r.holdout_symbol_cyclic_coherence_squared = q.real(0, 1);
    if (r.status == "measured") require(r.train_symbols >= 8 && r.holdout_symbols >= 8 &&
        std::abs(r.holdout_contrast - (r.holdout_prefix_coherence_squared - r.holdout_outside_coherence_squared)) < 1e-12);
    return r;
}
void write_chirp(Writer& w, const ChirpMeasurement& r) {
    w.str(r.label); w.str(r.status); w.real(r.slope_hz_per_second); w.real(r.nominal_sweep_hz);
    for (auto n : {r.lag_samples, r.gate_samples, r.positions_examined, r.positions_eligible, r.peak_offset}) w.u64(n);
    for (auto v : {r.peak_coherence_squared, r.first_half_coherence_squared,
                   r.second_half_coherence_squared, r.half_energy_balance}) w.real(v);
    w.str(r.frequency_status); w.u64(r.frequency_pairs); w.u64(r.phase_unwraps);
    w.real(r.frequency_slope_hz_per_second); w.real(r.frequency_rmse_hz);
    w.real(r.frequency_mean_hz);
}
ChirpMeasurement read_chirp(Reader& q, double rate, std::size_t samples, const ChirpMeasurement& expected) {
    ChirpMeasurement r; r.label = q.str(); r.status = q.str();
    r.slope_hz_per_second = q.real(); r.nominal_sweep_hz = q.real(0, 1e9);
    r.lag_samples = q.size(32000); r.gate_samples = q.size(32000);
    require(r.label == expected.label && r.slope_hz_per_second == expected.slope_hz_per_second &&
            r.nominal_sweep_hz == expected.nominal_sweep_hz && r.lag_samples == expected.lag_samples &&
            r.gate_samples == expected.gate_samples);
    r.positions_examined = q.size(samples); r.positions_eligible = q.size(r.positions_examined);
    r.peak_offset = q.size(samples);
    r.peak_coherence_squared = q.real(0, 1); r.first_half_coherence_squared = q.real(0, 1);
    r.second_half_coherence_squared = q.real(0, 1); r.half_energy_balance = q.real(0, 1);
    r.frequency_status = q.str();
    const auto phase_pairs = r.lag_samples + r.gate_samples ? r.lag_samples + r.gate_samples - 1 : 0;
    r.frequency_pairs = q.size(phase_pairs); r.phase_unwraps = q.size(r.frequency_pairs ? r.frequency_pairs - 1 : 0);
    r.frequency_slope_hz_per_second = q.real(-1e20, 1e20); r.frequency_rmse_hz = q.real(0, 1e14);
    r.frequency_mean_hz = q.real(-1e14, 1e14);
    const bool unsupported = rate <= r.nominal_sweep_hz || r.lag_samples < 8;
    const bool short_input = samples < r.lag_samples + r.gate_samples;
    if (unsupported || short_input) {
        require(r.status == (unsupported ? "unsupported_sample_rate" : "insufficient_samples") &&
                !r.positions_examined && !r.positions_eligible);
    } else {
        require(r.positions_examined == samples - r.lag_samples - r.gate_samples + 1 &&
                status(r.status, {"measured", "no_variation", "no_eligible_energy"}));
        require((r.status == "measured") == (r.positions_eligible > 0));
    }
    if (r.status == "measured") {
        require(r.peak_offset < r.positions_examined && r.half_energy_balance > 0 &&
                status(r.frequency_status, {"measured", "partial_phase_support"}) &&
                (r.frequency_status == "measured") == (r.frequency_pairs == phase_pairs));
    } else require(!r.peak_offset && r.peak_coherence_squared == 0 && r.first_half_coherence_squared == 0 &&
                 r.second_half_coherence_squared == 0 && r.half_energy_balance == 0 && r.frequency_status == "unavailable" &&
                 !r.frequency_pairs && !r.phase_unwraps);
    if (r.frequency_status != "measured") require(r.frequency_slope_hz_per_second == 0 && r.frequency_rmse_hz == 0 && r.frequency_mean_hz == 0);
    return r;
}
void write_roi(Writer& w, const RoiMeasurements& r) {
    for (auto v : {r.samples_examined, r.blocks, r.regions_seen, r.contrast_samples}) w.u64(v);
    for (auto v : {r.dc_fraction, r.background_to_mean, r.high_threshold_to_mean}) w.real(v);
    w.str(r.status); require(r.regions.size() <= roi_region_limit); w.u64(r.regions.size(), 4);
    for (const auto& region : r.regions) {
        for (auto v : {region.offset, region.samples, region.spectral_frames, region.spectral_samples, region.bands_seen}) w.u64(v);
        for (auto v : {region.energy_fraction, region.occupied_low_hz, region.occupied_high_hz}) w.real(v);
        w.u64(region.contrast_selected, 4); w.u64(region.touches_window_edge, 4); w.str(region.spectral_status);
        require(region.bands.size() <= roi_band_limit); w.u64(region.bands.size(), 4);
        for (const auto& b : region.bands) {
            for (auto v : {b.low_hz, b.high_hz, b.power_fraction}) w.real(v);
            w.u64(b.edge_bin, 4); w.u64(b.contains_dc, 4);
        }
    }
}
RoiMeasurements read_roi(Reader& q, double rate, std::size_t samples) {
    RoiMeasurements r; r.samples_examined = q.size(samples); require(r.samples_examined == samples);
    r.blocks = q.size(512); require(r.blocks == (samples + roi_block_samples - 1) / roi_block_samples);
    r.regions_seen = q.size(r.blocks); r.contrast_samples = q.size(samples);
    r.dc_fraction = q.real(0, 1); r.background_to_mean = q.real(0, 1e6); r.high_threshold_to_mean = q.real(0, 4e6);
    r.status = q.str(); require(status(r.status, {"contrast_regions", "uniform_energy", "no_variation"}));
    const auto count = q.size(roi_region_limit, 4);
    if (r.status == "no_variation") require(!count && !r.regions_seen && !r.contrast_samples &&
        r.background_to_mean == 0 && r.high_threshold_to_mean == 0);
    else {
        require(std::abs(r.high_threshold_to_mean - 4 * std::max(r.background_to_mean, 1e-12)) <=
                1e-10 * std::max(1.0, r.high_threshold_to_mean));
        if (r.status == "contrast_regions") require(r.regions_seen && r.contrast_samples && count == std::min(r.regions_seen, roi_region_limit));
        else require(count == 1 && !r.regions_seen && !r.contrast_samples);
    }
    std::size_t last = 0, selected = 0; double energy = 0;
    for (std::size_t i = 0; i < count; ++i) {
        EnergyRegion region; region.offset = q.size(samples); region.samples = q.size(samples);
        require(region.samples > 0 && region.offset >= last && region.samples <= samples - region.offset &&
                region.offset % roi_block_samples == 0);
        last = region.offset + region.samples;
        require(last == samples || last % roi_block_samples == 0);
        region.spectral_frames = q.size(roi_frame_limit); region.spectral_samples = q.size(region.samples);
        require(region.spectral_samples == region.spectral_frames * roi_fft_size);
        region.bands_seen = q.size(roi_fft_size); region.energy_fraction = q.real(0, 1);
        energy += region.energy_fraction;
        region.occupied_low_hz = q.real(-rate / 2, rate / 2); region.occupied_high_hz = q.real(region.occupied_low_hz, rate / 2);
        region.contrast_selected = q.size(1, 4); region.touches_window_edge = q.size(1, 4);
        require(region.contrast_selected == (r.status == "contrast_regions") &&
                region.touches_window_edge == (region.offset == 0 || last == samples));
        if (r.status == "uniform_energy") require(region.offset == 0 && region.samples == samples && region.energy_fraction == 1);
        else selected += region.samples;
        region.spectral_status = q.str();
        require(status(region.spectral_status, {"measured", "few_frames", "no_variation", "insufficient_samples"}));
        const bool has_psd = region.spectral_status == "measured" || region.spectral_status == "few_frames";
        if (has_psd) require(region.spectral_frames > 0 &&
            region.spectral_frames == std::min(roi_frame_limit, region.samples / roi_fft_size) &&
            (region.spectral_frames >= 8) == (region.spectral_status == "measured"));
        else require(!region.spectral_frames && !region.bands_seen && region.occupied_low_hz == 0 && region.occupied_high_hz == 0 &&
            (region.samples < roi_fft_size) == (region.spectral_status == "insufficient_samples"));
        const auto bands = q.size(roi_band_limit, 4); require(bands == std::min(region.bands_seen, roi_band_limit));
        double high = -rate / 2, fraction = 0;
        for (std::size_t b = 0; b < bands; ++b) {
            FrequencyInterval band; band.low_hz = q.real(high, rate / 2); band.high_hz = q.real(band.low_hz, rate / 2);
            require(band.high_hz > band.low_hz); high = band.high_hz;
            band.power_fraction = q.real(0, 1); fraction += band.power_fraction;
            band.edge_bin = q.size(1, 4); band.contains_dc = q.size(1, 4);
            require(band.contains_dc == (band.low_hz <= 0 && band.high_hz >= 0));
            const bool edge = band.low_hz == -rate / 2 || std::abs(band.high_hz - (rate / 2 - rate / roi_fft_size / 2)) < rate * 1e-12;
            require(band.edge_bin == edge); region.bands.push_back(band);
        }
        require(fraction <= 1 + 1e-8); r.regions.push_back(std::move(region));
    }
    require(energy <= 1 + 1e-8 && selected <= r.contrast_samples);
    if (r.status == "contrast_regions" && r.regions_seen <= roi_region_limit) require(selected == r.contrast_samples);
    return r;
}
} // namespace

WireBytes encode_header(MessageType type, std::size_t bytes) {
    require(bytes <= request_byte_limit); Writer w;
    for (auto v : {wire_magic, wire_version, std::uint32_t(type), std::uint32_t(bytes)}) w.u64(v, 4);
    return w.bytes;
}
WireHeader decode_header(const WireBytes& bytes, std::size_t limit) {
    require(bytes.size() == 16); Reader q{bytes};
    require(q.u64(4) == wire_magic && q.u64(4) == wire_version);
    auto type = q.size(4, 4); require(type >= 1); auto length = q.size(limit, 4); q.end();
    return {MessageType(type), std::uint32_t(length)};
}
WireBytes encode_request(std::uint64_t id, double rate, const TilePlan& plan, const std::vector<std::complex<float>>& iq) {
    require(plan.count > 0 && plan.count <= tile_limit && plan.samples == iq.size()); Writer w;
    w.bytes.reserve(24 + plan.count * 12 + iq.size() * 8);
    w.u64(id); w.real(rate); w.u64(plan.count, 4);
    std::size_t start = 0;
    for (std::size_t t = 0; t < plan.count; ++t) {
        const auto& p = plan.tiles[t]; require(p.samples >= 2048 && p.samples <= tile_sample_limit && p.samples <= iq.size() - start &&
            p.source_offset <= std::numeric_limits<std::size_t>::max() - p.samples);
        w.u64(p.source_offset); w.u64(p.samples, 4);
        for (std::size_t n = 0; n < p.samples; ++n) { w.sample(iq[start + n].real()); w.sample(iq[start + n].imag()); }
        start += p.samples;
    }
    require(start == iq.size() && w.bytes.size() <= request_byte_limit); return w.bytes;
}
WorkerRequest decode_request(const WireBytes& bytes) {
    require(bytes.size() <= request_byte_limit); Reader q{bytes}; WorkerRequest r;
    r.job_id = q.u64(); r.rate_hz = q.real(std::numeric_limits<double>::min(), 1e9);
    const auto count = q.size(tile_limit, 4); require(count > 0); std::size_t total = 0;
    for (std::size_t t = 0; t < count; ++t) {
        WorkerTile tile; tile.source.source_offset = q.size(std::numeric_limits<std::size_t>::max());
        tile.source.samples = q.size(tile_sample_limit, 4); require(tile.source.samples >= 2048);
        require(tile.source.source_offset <= std::numeric_limits<std::size_t>::max() - tile.source.samples);
        total += tile.source.samples; require(total <= capture_sample_budget);
        tile.iq.reserve(tile.source.samples);
        for (std::size_t n = 0; n < tile.source.samples; ++n) { auto i = q.sample(), v = q.sample(); tile.iq.emplace_back(i, v); }
        r.tiles.push_back(std::move(tile));
    }
    q.end(); return r;
}
WireBytes encode_result(std::uint64_t id, const std::vector<TileMeasurement>& tiles) {
    require(!tiles.empty() && tiles.size() <= tile_limit); Writer w; w.u64(id); w.u64(tiles.size(), 4);
    for (const auto& tile : tiles) {
        w.u64(tile.source.source_offset); w.u64(tile.source.samples, 4); write_spectral(w, tile.spectral);
        require(tile.ofdm.size() <= 5); w.u64(tile.ofdm.size(), 4);
        for (const auto& m : tile.ofdm) write_ofdm(w, m);
        write_roi(w, tile.roi);
        require(tile.chirps.size() == 3); w.u64(tile.chirps.size(), 4);
        for (const auto& m : tile.chirps) write_chirp(w, m);
    }
    require(w.bytes.size() <= reply_byte_limit); return w.bytes;
}
std::vector<TileMeasurement> decode_result(const WireBytes& bytes, std::uint64_t id, double rate, const TilePlan& plan) {
    require(bytes.size() <= reply_byte_limit); Reader q{bytes}; require(q.u64() == id && q.size(tile_limit, 4) == plan.count);
    std::vector<TileMeasurement> out;
    const auto bank = wlan_ofdm_hypotheses(rate);
    for (std::size_t t = 0; t < plan.count; ++t) {
        TileMeasurement tile; tile.source.source_offset = q.size(std::numeric_limits<std::size_t>::max());
        tile.source.samples = q.size(tile_sample_limit, 4);
        require(tile.source.samples == plan.tiles[t].samples && tile.source.source_offset == plan.tiles[t].source_offset);
        tile.spectral = read_spectral(q, rate, tile.source.samples);
        require(q.size(5, 4) == bank.size());
        for (const auto& h : bank) tile.ofdm.push_back(read_ofdm(q, rate, tile.source.samples, h));
        tile.roi = read_roi(q, rate, tile.source.samples);
        const auto chirp_bank = measure_chirp_structure({}, rate);
        require(q.size(3, 4) == chirp_bank.size());
        for (const auto& h : chirp_bank) tile.chirps.push_back(read_chirp(q, rate, tile.source.samples, h));
        tile.evidence = assess_link_evidence(tile.spectral, tile.ofdm, tile.chirps, {rate, 0, -1});
        out.push_back(std::move(tile));
    }
    q.end(); return out;
}
WireBytes encode_error(std::uint64_t id) { Writer w; w.u64(id); return w.bytes; }
void validate_error(const WireBytes& bytes, std::uint64_t id) { Reader q{bytes}; require(q.u64() == id); q.end(); }
} // namespace rfmon::cyclo
