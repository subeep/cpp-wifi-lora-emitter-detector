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
void write_spectral(Writer& w, const SpectralFeatures& r, bool compact_power=false) {
    for (auto v : {r.fft_size, r.hop_samples, r.frames, r.samples_used}) w.u64(v);
    for (auto v : {r.bin_hz, r.mean_i, r.mean_q, r.mean_power, r.variance_power, r.dc_fraction,
                   r.max_component_abs, r.spectral_flatness, r.strongest_bin_fraction,
                   r.occupied_low_hz, r.occupied_high_hz}) w.real(v);
    w.str(r.quality); require(r.warnings.size() <= 16); w.u64(r.warnings.size(), 4);
    for (const auto& v : r.warnings) w.str(v);
    require(r.power_fraction.size() <= 512); w.u64(r.power_fraction.size(), 4);
    for (auto v : r.power_fraction) { if(compact_power)w.sample(float(v));else w.real(v); }
    require(r.peaks.size() <= 12); w.u64(r.peaks.size(), 4);
    for (const auto& p : r.peaks)
        for (auto v : {p.alpha_hz, p.frequency_offset_hz, p.coherence_squared, p.normalized_cross_magnitude}) w.real(v);
}
SpectralFeatures read_spectral(Reader& q, double rate, std::size_t count, bool compact_power=false) {
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
    for (std::size_t i = 0; i < powers; ++i) { const auto p = compact_power?double(q.sample()):q.real(0, 1);
        require(std::isfinite(p) && p>=0 && p<=1);r.power_fraction.push_back(p); sum += p; }
    require(!powers || std::abs(sum - 1) < (compact_power?1e-6:1e-8));
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
void write_waveform(Writer& w, const WaveformFeatures& r) {
    w.str(r.cyclic_status); w.str(r.morphology_status);
    for(auto n:{r.samples_examined,r.partition_samples,r.holdout_offset,r.fft_calls,r.refinement_evaluations,r.phase_pairs}) w.u64(n);
    w.real(r.alpha_bin_hz); require(r.peaks.size()<=cyclic_peak_limit); w.u64(r.peaks.size(),4);
    for(const auto& p:r.peaks) {
        w.u64(p.conjugate,4); w.u64(p.lag_samples);
        for(auto v:{p.coarse_alpha_hz,p.alpha_hz,p.discovery_coherence_squared,p.holdout_coherence_squared}) w.real(v);
        w.u64(p.persistent_pattern,4);
    }
    for(auto v:{r.amplitude_cv,r.envelope_low,r.envelope_high,r.envelope_high_fraction,r.envelope_fit_residual,r.envelope_contrast,
                r.frequency_low_hz,r.frequency_high_hz,r.frequency_first_fraction,r.frequency_second_fraction,r.frequency_concentration}) w.real(v);
    w.u64(r.envelope_transitions); w.u64(r.frequency_transitions);
    w.u64(r.two_level_envelope_pattern,4); w.u64(r.two_frequency_pattern,4);
}
WaveformFeatures read_waveform(Reader& q,double rate,std::size_t samples) {
    WaveformFeatures r; r.cyclic_status=q.str(); r.morphology_status=q.str();
    require(status(r.cyclic_status,{"measured","no_variation","insufficient_samples"}) &&
            status(r.morphology_status,{"measured","no_energy","insufficient_samples"}));
    r.samples_examined=q.size(samples); require(r.samples_examined==samples);
    r.partition_samples=q.size(cyclic_partition_limit); r.holdout_offset=q.size(samples);
    r.fft_calls=q.size(8); r.refinement_evaluations=q.size(40); r.phase_pairs=q.size(samples-1);
    r.alpha_bin_hz=q.real(0,rate);
    if(samples<2064) require(r.cyclic_status=="insufficient_samples" && !r.partition_samples && !r.holdout_offset && r.alpha_bin_hz==0);
    else {
        std::size_t n=1024; while(n<cyclic_partition_limit && 2*(n*2)+16<=samples) n*=2;
        require(r.cyclic_status!="insufficient_samples" && r.partition_samples==n && r.holdout_offset==samples-n && r.alpha_bin_hz==rate/n);
    }
    const auto count=q.size(cyclic_peak_limit,4); std::size_t types[2]={0,0};
    if(r.cyclic_status!="measured") require(!count && !r.fft_calls && !r.refinement_evaluations);
    else require(r.fft_calls==8 && r.refinement_evaluations==5*count);
    for(std::size_t j=0;j<count;++j) {
        RatePeak p; p.conjugate=q.size(1,4); p.lag_samples=q.size(16);
        require(std::find(std::begin(cyclic_lags),std::end(cyclic_lags),p.lag_samples)!=std::end(cyclic_lags));
        p.coarse_alpha_hz=q.real(-rate/2,rate/2); p.alpha_hz=q.real(-rate/2,rate/2);
        const double bin=p.coarse_alpha_hz/r.alpha_bin_hz;
        require(std::abs(bin-std::round(bin))<1e-8 && std::abs(bin)>=9 && std::abs(bin)<r.partition_samples/2-1 &&
                (p.conjugate || p.coarse_alpha_hz>0) &&
                std::abs(p.alpha_hz-p.coarse_alpha_hz)<=.50000001*r.alpha_bin_hz && ++types[p.conjugate]<=4);
        for(const auto& old:r.peaks) require(old.conjugate!=p.conjugate ||
            std::abs(old.coarse_alpha_hz-p.coarse_alpha_hz)>=1.99999999*r.alpha_bin_hz);
        p.discovery_coherence_squared=q.real(0,1); p.holdout_coherence_squared=q.real(0,1);
        p.persistent_pattern=q.size(1,4);
        require(p.persistent_pattern==(p.discovery_coherence_squared>=.05 && p.holdout_coherence_squared>=.05));
        r.peaks.push_back(p);
    }
    r.amplitude_cv=q.real(0,std::sqrt(double(samples)));
    r.envelope_low=q.real(0,samples); r.envelope_high=q.real(r.envelope_low,samples);
    r.envelope_high_fraction=q.real(0,1); r.envelope_fit_residual=q.real(0,1e20); r.envelope_contrast=q.real(0,1);
    r.frequency_low_hz=q.real(-rate/2,rate/2); r.frequency_high_hz=q.real(r.frequency_low_hz,rate/2);
    r.frequency_first_fraction=q.real(0,1); r.frequency_second_fraction=q.real(0,1); r.frequency_concentration=q.real(0,1);
    require(std::abs(r.frequency_concentration-(r.frequency_first_fraction+r.frequency_second_fraction))<1e-12);
    r.envelope_transitions=q.size(samples-1); r.frequency_transitions=q.size(r.phase_pairs);
    r.two_level_envelope_pattern=q.size(1,4); r.two_frequency_pattern=q.size(1,4);
    if(r.morphology_status!="measured") require(!r.phase_pairs && !r.envelope_transitions && !r.frequency_transitions &&
        r.amplitude_cv==0 && r.envelope_low==0 && r.envelope_high==0 && r.envelope_fit_residual==0 && r.envelope_contrast==0 &&
        r.envelope_high_fraction==0 && r.frequency_low_hz==0 && r.frequency_high_hz==0 && r.frequency_concentration==0);
    require(r.two_level_envelope_pattern==(r.morphology_status=="measured" && r.envelope_contrast>=.7 &&
        r.envelope_fit_residual<=.05 && r.envelope_high_fraction>=.1 && r.envelope_high_fraction<=.9 && r.envelope_transitions>=8));
    require(r.two_frequency_pattern==(r.morphology_status=="measured" && r.phase_pairs>=.8*(samples-1) && r.amplitude_cv<=.3 &&
        r.frequency_concentration>=.8 && r.frequency_first_fraction>=.1 && r.frequency_second_fraction>=.1 && r.frequency_transitions>=8));
    return r;
}

void write_background(Writer& w,const CyclicBackground& r) {
    w.str(r.status);for(auto n:{r.samples_examined,r.partition_samples,r.holdout_offset,r.fft_calls})w.u64(n);
    require(r.peaks.size()<=8);w.u64(r.peaks.size(),4);
    for(const auto& p:r.peaks) {
        for(const auto* part:{&p.discovery,&p.holdout}) {
            w.u64(part->reference_bins);
            for(auto v:{part->median_coherence_squared,part->upper_coherence_squared,part->line_to_median,part->line_to_upper,
                part->block_phase_coherence_squared,part->min_block_energy_fraction})w.real(v);
        }
        w.u64(p.background_supported,4);
    }
}
CyclicBackground read_background(Reader& q,double rate,const WaveformFeatures& raw) {
    CyclicBackground r;r.status=q.str();require(r.status==raw.cyclic_status);
    r.samples_examined=q.size(raw.samples_examined);r.partition_samples=q.size(8192);r.holdout_offset=q.size(raw.samples_examined);
    require(r.samples_examined==raw.samples_examined && r.partition_samples==raw.partition_samples && r.holdout_offset==raw.holdout_offset);
    r.fft_calls=q.size(16);require(q.size(8,4)==raw.peaks.size());
    std::vector<std::pair<bool,std::size_t>> keys;
    for(const auto& peak:raw.peaks) {
        const auto key=std::make_pair(peak.conjugate,peak.lag_samples);
        if(std::find(keys.begin(),keys.end(),key)==keys.end())keys.push_back(key);
        const auto bin=int(std::round(peak.coarse_alpha_hz/(rate/raw.partition_samples)));
        const auto refs=cyclic_reference_bins(raw.partition_samples,bin).size();
        CyclicBackgroundPeak p;
        for(auto* part:{&p.discovery,&p.holdout}) {
            part->reference_bins=q.size(34);require(part->reference_bins==refs);
            part->median_coherence_squared=q.real(0,1);part->upper_coherence_squared=q.real(part->median_coherence_squared,1);
            part->line_to_median=q.real(0,1e12);part->line_to_upper=q.real(0,1e12);
            const double line=part==&p.discovery?peak.discovery_coherence_squared:peak.holdout_coherence_squared;
            const double median_ratio=line/std::max(part->median_coherence_squared,1e-12),upper_ratio=line/std::max(part->upper_coherence_squared,1e-12);
            require(std::abs(part->line_to_median-median_ratio)<=1e-12*std::max(1.0,median_ratio) &&
                std::abs(part->line_to_upper-upper_ratio)<=1e-12*std::max(1.0,upper_ratio));
            part->block_phase_coherence_squared=q.real(0,1);part->min_block_energy_fraction=q.real(0,.250000000001);
        }
        p.background_supported=q.size(1,4);require(p.background_supported==cyclic_background_supported(p,peak));r.peaks.push_back(p);
    }
    require(r.fft_calls==2*keys.size());return r;
}

void write_structure(Writer& w,const StructureDiscovery& r) {
    w.str(r.status);
    for(auto n:{r.samples_examined,r.partition_samples,r.holdout_offset,r.max_lag,r.ofdm_fft_calls,r.timing_hypotheses,r.spread_hypotheses}) w.u64(n);
    w.real(r.carrier_estimate_hz); w.real(r.carrier_coherence_squared);
    require(r.ofdm.size()<=structure_candidate_limit && r.spread.size()<=structure_candidate_limit);
    w.u64(r.ofdm.size(),4);
    for(const auto& p:r.ofdm) {
        for(auto n:{p.useful_samples,p.prefix_samples,p.phase_samples,p.symbols_per_partition}) w.u64(n);
        for(auto v:{p.lag_coherence_squared,p.train_prefix,p.train_outside,p.holdout_prefix,p.holdout_outside,p.holdout_cyclic}) w.real(v);
        w.u64(p.pattern_consistent,4);
    }
    w.u64(r.spread.size(),4);
    for(const auto& p:r.spread) {
        for(auto n:{p.chip_samples,p.code_phase_samples,p.train_words,p.holdout_words}) w.u64(n);
        for(auto v:{p.carrier_hz,p.train_code_coherence_squared,p.holdout_code_coherence_squared,p.train_other_phase,p.holdout_other_phase}) w.real(v);
        w.u64(p.pattern_consistent,4);
    }
}
std::size_t complete_code_words(std::size_t n,std::size_t origin,std::size_t chip,std::size_t phase) {
    const auto period=11*chip, first=(phase+period-origin%period)%period;
    return first+period<=n ? 1+(n-first-period)/period : 0;
}
StructureDiscovery read_structure(Reader& q,double rate,std::size_t samples) {
    StructureDiscovery r; r.status=q.str(); require(status(r.status,{"measured","insufficient_samples","no_variation"}));
    r.samples_examined=q.size(samples); require(r.samples_examined==samples);
    r.partition_samples=q.size(structure_partition_limit);r.holdout_offset=q.size(samples);r.max_lag=q.size(1024);
    r.ofdm_fft_calls=q.size(6);r.timing_hypotheses=q.size(32);r.spread_hypotheses=q.size(32);
    r.carrier_estimate_hz=q.real(-rate/4,rate/4);r.carrier_coherence_squared=q.real(0,1);
    if(samples<2064) require(r.status=="insufficient_samples" && !r.partition_samples && !r.holdout_offset && !r.max_lag);
    else {
        std::size_t n=1024;while(n<structure_partition_limit && 4*n+16<=samples)n*=2;
        require(r.status!="insufficient_samples" && r.partition_samples==n && r.holdout_offset==samples-n && r.max_lag==std::min<std::size_t>(1024,n/10));
    }
    if(r.status!="measured") require(!r.ofdm_fft_calls && !r.timing_hypotheses && !r.spread_hypotheses && r.carrier_estimate_hz==0 && r.carrier_coherence_squared==0);
    else require(r.ofdm_fft_calls>=2 && r.spread_hypotheses==32 && r.timing_hypotheses<=8*(r.ofdm_fft_calls-2));
    const auto count=q.size(structure_candidate_limit,4);
    require(r.status=="measured" ? count==std::min<std::size_t>(structure_candidate_limit,r.timing_hypotheses) : count==0);
    for(std::size_t j=0;j<count;++j) {
        DiscoveredOfdm p;p.useful_samples=q.size(r.max_lag);require(p.useful_samples>=16);
        p.prefix_samples=q.size(p.useful_samples/2);require(p.prefix_samples>=4);
        const auto period=p.useful_samples+p.prefix_samples;
        p.phase_samples=q.size(period-1);p.symbols_per_partition=q.size(r.partition_samples);
        require(p.symbols_per_partition==(r.partition_samples-p.useful_samples)/period && p.symbols_per_partition>=8);
        p.lag_coherence_squared=q.real(0,1);p.train_prefix=q.real(0,1);p.train_outside=q.real(0,1);
        p.holdout_prefix=q.real(0,1);p.holdout_outside=q.real(0,1);p.holdout_cyclic=q.real(0,1);p.pattern_consistent=q.size(1,4);
        require(p.pattern_consistent==(p.train_prefix>=.5 && p.train_outside<=.1 && p.holdout_prefix>=.5 &&
            p.holdout_outside<=.1 && p.holdout_prefix-p.holdout_outside>=.4 && p.holdout_cyclic>=.005));
        for(const auto& old:r.ofdm) require(old.useful_samples!=p.useful_samples || old.prefix_samples!=p.prefix_samples);
        if(!r.ofdm.empty()) require(p.train_prefix-p.train_outside<=r.ofdm.back().train_prefix-r.ofdm.back().train_outside+1e-12);
        r.ofdm.push_back(p);
    }
    const auto spread=q.size(structure_candidate_limit,4);require(r.status=="measured" || spread==0);
    const double alias=r.carrier_estimate_hz>=0?r.carrier_estimate_hz-rate/2:r.carrier_estimate_hz+rate/2;
    for(std::size_t j=0;j<spread;++j) {
        SpreadMeasurement p;p.chip_samples=q.size(16);require(p.chip_samples>=1);
        p.code_phase_samples=q.size(11*p.chip_samples-1);
        p.train_words=q.size(r.partition_samples/11);p.holdout_words=q.size(r.partition_samples/11);
        // Some zero-power code words may be skipped, but no unsupported words
        // or samples spanning the partition boundary may be reported.
        require(p.train_words>=8 && p.train_words<=complete_code_words(r.partition_samples,0,p.chip_samples,p.code_phase_samples) &&
            p.holdout_words<=complete_code_words(r.partition_samples,r.holdout_offset,p.chip_samples,p.code_phase_samples));
        p.carrier_hz=q.real(-rate/2,rate/2);require(p.carrier_hz==r.carrier_estimate_hz || p.carrier_hz==alias);
        p.train_code_coherence_squared=q.real(0,1);p.holdout_code_coherence_squared=q.real(0,1);
        p.train_other_phase=q.real(0,1);p.holdout_other_phase=q.real(0,1);p.pattern_consistent=q.size(1,4);
        require(p.pattern_consistent==(p.holdout_words>=8 && p.train_code_coherence_squared>=.8 && p.holdout_code_coherence_squared>=.8 &&
            p.train_code_coherence_squared-p.train_other_phase>=.5 && p.holdout_code_coherence_squared-p.holdout_other_phase>=.5));
        if(!p.holdout_words)require(p.holdout_code_coherence_squared==0);
        for(const auto& old:r.spread)require(old.chip_samples!=p.chip_samples || old.carrier_hz!=p.carrier_hz);
        if(!r.spread.empty())require(p.train_code_coherence_squared-p.train_other_phase<=r.spread.back().train_code_coherence_squared-r.spread.back().train_other_phase+1e-12);
        r.spread.push_back(p);
    }
    return r;
}

void write_sweeps(Writer& w,const ChirpDiscovery& r) {
    w.str(r.status);
    for(auto n:{r.samples_examined,r.partition_samples,r.holdout_partition_offset,r.discovery_trials,r.discovery_continuation_trials})w.u64(n);
    require(r.candidates.size()<=sweep_candidate_limit);w.u64(r.candidates.size(),4);
    for(const auto& p:r.candidates) {
        for(auto n:{p.span_samples,p.discovery_offset,p.holdout_offset,p.holdout_trials})w.u64(n);
        for(auto v:{p.slope_hz_per_second,p.sweep_hz,p.discovery_center_hz,p.holdout_center_hz,
            p.discovery_rmse_fraction,p.holdout_rmse_fraction,p.discovery_coherence_squared,p.holdout_coherence_squared,
            p.discovery_amplitude_cv,p.holdout_amplitude_cv})w.real(v);
        w.u64(p.holdout_supported,4);w.u64(p.pattern_consistent,4);
    }
}
ChirpDiscovery read_sweeps(Reader& q,double rate,std::size_t samples) {
    ChirpDiscovery r;r.status=q.str();require(status(r.status,{"measured","insufficient_samples","no_variation"}));
    r.samples_examined=q.size(samples);require(r.samples_examined==samples);
    r.partition_samples=q.size(sweep_partition_limit);r.holdout_partition_offset=q.size(samples);r.discovery_trials=q.size(510);r.discovery_continuation_trials=q.size(1020);
    std::size_t n=0,trials=0;
    if(samples>=1040) {n=512;while(n<sweep_partition_limit && 4*n+16<=samples)n*=2;
        for(std::size_t span=64;span<=2048;span*=2)trials+=sweep_window_count(n,span);}
    require(r.partition_samples==n && r.holdout_partition_offset==(n?samples-n:0));
    require((r.status=="insufficient_samples")==!n);
    require(r.discovery_trials==(r.status=="measured"?trials:0) && r.discovery_continuation_trials<=2*r.discovery_trials);
    const auto count=q.size(sweep_candidate_limit,4);require(r.status=="measured" || !count);
    for(std::size_t j=0;j<count;++j) {
        LinearSweep p;p.span_samples=q.size(std::min<std::size_t>(2048,n));
        require(sweep_window_count(n,p.span_samples)>0);
        p.discovery_offset=q.size(n-p.span_samples);require(p.discovery_offset%(p.span_samples/2)==0);
        p.holdout_offset=q.size(samples);p.holdout_trials=q.size(255);
        require(p.holdout_trials==sweep_window_count(n,p.span_samples));
        p.slope_hz_per_second=q.real(-rate*rate,rate*rate);p.sweep_hz=q.real(.04*rate,.7*rate);
        require(std::abs(p.sweep_hz-std::abs(p.slope_hz_per_second)*(p.span_samples-1)/rate)<=1e-10*rate);
        p.discovery_center_hz=q.real(-.45*rate,.45*rate);p.holdout_center_hz=q.real(-.45*rate,.45*rate);
        p.discovery_rmse_fraction=q.real(0,.003);p.holdout_rmse_fraction=q.real(0,2);
        p.discovery_coherence_squared=q.real(.65,1);p.holdout_coherence_squared=q.real(0,1);
        p.discovery_amplitude_cv=q.real(0,.5);p.holdout_amplitude_cv=q.real(0,10000);
        p.holdout_supported=q.size(1,4);p.pattern_consistent=q.size(1,4);
        require(p.pattern_consistent==(p.holdout_supported && p.holdout_rmse_fraction<=.003 && p.holdout_coherence_squared>=.65 && p.holdout_amplitude_cv<=.5));
        if(p.holdout_supported)require(p.holdout_offset>=samples-n && p.holdout_offset<=samples-p.span_samples &&
            (p.holdout_offset-(samples-n))%(p.span_samples/2)==0);
        else require(!p.holdout_offset && !p.holdout_center_hz && !p.holdout_rmse_fraction && !p.holdout_coherence_squared && !p.holdout_amplitude_cv);
        if(!r.candidates.empty())require(p.sweep_hz<=r.candidates.back().sweep_hz);
        for(const auto& old:r.candidates)require(old.slope_hz_per_second*p.slope_hz_per_second<=0 ||
            std::abs(old.slope_hz_per_second-p.slope_hz_per_second)>.05*std::abs(old.slope_hz_per_second));
        r.candidates.push_back(p);
    }
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
void write_burst(Writer& w,const BurstAnalysis& b) {
    const auto& s=b.selection;
    w.str(s.status);
    for(auto n:{s.eligible_regions,s.short_regions,s.budget_skipped_regions,s.region_index,s.offset,s.samples})w.u64(n);
    w.u64(s.cropped,4);
    if(!s.samples)return;
    require(b.spectral.peaks.empty());write_spectral(w,b.spectral,true);
    write_waveform(w,b.waveform);write_background(w,b.background);
    write_structure(w,b.structure);write_sweeps(w,b.sweeps);
}
BurstAnalysis read_burst(Reader& q,double rate,const RoiMeasurements& roi) {
    BurstAnalysis b;auto& s=b.selection;const auto expected=select_burst_region(roi);
    s.status=q.str();require(s.status==expected.status);
    s.eligible_regions=q.size(roi_region_limit);s.short_regions=q.size(roi_region_limit);
    s.budget_skipped_regions=q.size(roi_region_limit);s.region_index=q.size(roi_region_limit-1);
    s.offset=q.size(roi.samples_examined);s.samples=q.size(burst_sample_limit);s.cropped=q.size(1,4);
    require(s.eligible_regions==expected.eligible_regions && s.short_regions==expected.short_regions &&
        s.budget_skipped_regions==expected.budget_skipped_regions && s.region_index==expected.region_index &&
        s.offset==expected.offset && s.samples==expected.samples && s.cropped==expected.cropped);
    if(!s.samples)return b;
    b.spectral=read_spectral(q,rate,s.samples,true);require(b.spectral.peaks.empty());
    b.waveform=read_waveform(q,rate,s.samples);b.background=read_background(q,rate,b.waveform);
    b.structure=read_structure(q,rate,s.samples);b.sweeps=read_sweeps(q,rate,s.samples);
    review_burst(b,{rate,0,-1});return b;
}
void write_clock(Writer& w,const ClockRefinement& r) {
    w.str(r.status);
    for(auto n:{r.samples_examined,r.partition_samples,r.target_samples,r.holdout_offset,r.grids_tested,r.cp_trials,r.code_trials,r.interpolated_samples})w.u64(n);
    require(r.cp.size()<=1 && r.code.size()<=1);w.u64(r.cp.size(),4);
    for(const auto& p:r.cp) {
        w.u64(p.seed_index,4);w.u64(p.grid_index,4);const auto& m=p.measurement;
        for(auto n:{m.useful_samples,m.prefix_samples,m.phase_samples,m.symbols_per_partition})w.u64(n);
        for(auto v:{m.lag_coherence_squared,m.train_prefix,m.train_outside,m.holdout_prefix,m.holdout_outside,m.holdout_cyclic})w.real(v);
        w.u64(m.pattern_consistent,4);
    }
    w.u64(r.code.size(),4);
    for(const auto& p:r.code) {
        w.u64(p.seed_index,4);w.u64(p.grid_index,4);const auto& m=p.measurement;
        for(auto n:{m.chip_samples,m.code_phase_samples,m.train_words,m.holdout_words})w.u64(n);
        for(auto v:{m.carrier_hz,m.train_code_coherence_squared,m.holdout_code_coherence_squared,m.train_other_phase,m.holdout_other_phase})w.real(v);
        w.u64(m.pattern_consistent,4);
    }
}
ClockRefinement read_clock(Reader& q,double rate,const StructureDiscovery& raw) {
    const auto expected=clock_refinement_plan(raw);ClockRefinement r;r.status=q.str();require(r.status==expected.status);
    r.samples_examined=q.size(65536);r.partition_samples=q.size(8192);r.target_samples=q.size(65536);r.holdout_offset=q.size(65536);
    r.grids_tested=q.size(9);r.cp_trials=q.size(18);r.code_trials=q.size(18);r.interpolated_samples=q.size(11*8192);
    require(r.samples_examined==expected.samples_examined && r.partition_samples==expected.partition_samples &&
        r.target_samples==expected.target_samples && r.holdout_offset==expected.holdout_offset && r.grids_tested==expected.grids_tested &&
        r.cp_trials==expected.cp_trials && r.code_trials==expected.code_trials);
    const auto cps=q.size(1,4);require(cps==(r.cp_trials?1:0));
    for(std::size_t j=0;j<cps;++j) {
        RefinedCp p;p.seed_index=q.size(std::min(timing_seed_limit,raw.ofdm.size())-1,4);p.grid_index=q.size(8,4);
        const auto& seed=raw.ofdm[p.seed_index];auto& m=p.measurement;
        m.useful_samples=q.size(raw.max_lag);m.prefix_samples=q.size(raw.max_lag/2);
        require(m.useful_samples==seed.useful_samples && m.prefix_samples==seed.prefix_samples);
        const auto period=m.useful_samples+m.prefix_samples;m.phase_samples=q.size(period-1);m.symbols_per_partition=q.size(r.partition_samples);
        require(m.symbols_per_partition==(r.partition_samples-m.useful_samples)/period && m.symbols_per_partition>=8);
        m.lag_coherence_squared=q.real(0,1);require(m.lag_coherence_squared==seed.lag_coherence_squared);
        m.train_prefix=q.real(0,1);m.train_outside=q.real(0,1);m.holdout_prefix=q.real(0,1);m.holdout_outside=q.real(0,1);m.holdout_cyclic=q.real(0,1);
        m.pattern_consistent=q.size(1,4);require(m.pattern_consistent==(m.train_prefix>=.5 && m.train_outside<=.1 &&
            m.holdout_prefix>=.5 && m.holdout_outside<=.1 && m.holdout_prefix-m.holdout_outside>=.4 && m.holdout_cyclic>=.005));
        r.cp.push_back(p);
    }
    const auto codes=q.size(1,4);require(r.code_trials || !codes);
    for(std::size_t j=0;j<codes;++j) {
        RefinedCode p;p.seed_index=q.size(std::min(timing_seed_limit,raw.spread.size())-1,4);p.grid_index=q.size(8,4);
        const auto& seed=raw.spread[p.seed_index];auto& m=p.measurement;m.chip_samples=q.size(16);require(m.chip_samples==seed.chip_samples);
        m.code_phase_samples=q.size(11*m.chip_samples-1);m.train_words=q.size(r.partition_samples/11);m.holdout_words=q.size(r.partition_samples/11);
        require(m.train_words>=8 && m.train_words<=complete_code_words(r.partition_samples,0,m.chip_samples,m.code_phase_samples) &&
            m.holdout_words<=complete_code_words(r.partition_samples,r.holdout_offset,m.chip_samples,m.code_phase_samples));
        m.carrier_hz=q.real(-rate/2,rate/2);
        require(std::abs(m.carrier_hz-std::remainder(seed.carrier_hz*timing_grid_scale(p.grid_index),rate))<=rate*1e-12);
        m.train_code_coherence_squared=q.real(0,1);m.holdout_code_coherence_squared=q.real(0,1);m.train_other_phase=q.real(0,1);m.holdout_other_phase=q.real(0,1);
        require(m.holdout_words || m.holdout_code_coherence_squared==0);
        m.pattern_consistent=q.size(1,4);require(m.pattern_consistent==(m.holdout_words>=8 && m.train_code_coherence_squared>=.8 &&
            m.holdout_code_coherence_squared>=.8 && m.train_code_coherence_squared-m.train_other_phase>=.5 &&
            m.holdout_code_coherence_squared-m.holdout_other_phase>=.5));
        r.code.push_back(p);
    }
    require(r.interpolated_samples==(r.grids_tested+cps+codes)*r.partition_samples);return r;
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
        write_waveform(w,tile.waveform);
        write_background(w,tile.cyclic_background);
        write_structure(w,tile.structure);
        write_sweeps(w,tile.sweeps);
        write_burst(w,tile.burst);
        write_clock(w,tile.clock);
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
        tile.waveform=read_waveform(q,rate,tile.source.samples);
        tile.cyclic_background=read_background(q,rate,tile.waveform);
        tile.background_review=review_cyclic_background(tile.spectral,tile.waveform,tile.cyclic_background,{rate,0,-1});
        tile.structure=read_structure(q,rate,tile.source.samples);
        tile.sweeps=read_sweeps(q,rate,tile.source.samples);
        tile.burst=read_burst(q,rate,tile.roi);
        tile.clock=read_clock(q,rate,tile.burst.selection.samples?tile.burst.structure:tile.structure);
        review_clock_refinement(tile.clock,tile.burst.selection.samples?tile.burst.spectral:tile.spectral,{rate,0,-1});
        tile.sweep_review=review_linear_sweeps(tile.spectral,tile.sweeps,{rate,0,-1});
        tile.evidence = assess_link_evidence(tile.spectral, tile.ofdm, tile.chirps, {rate, 0, -1});
        tile.review=review_waveform_measurements(tile.spectral,tile.structure,tile.waveform,{rate,0,-1});
        out.push_back(std::move(tile));
    }
    q.end(); return out;
}
WireBytes encode_error(std::uint64_t id) { Writer w; w.u64(id); return w.bytes; }
void validate_error(const WireBytes& bytes, std::uint64_t id) { Reader q{bytes}; require(q.u64() == id); q.end(); }
} // namespace rfmon::cyclo
