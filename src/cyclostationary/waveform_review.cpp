#include "waveform_review.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
void add(ReviewedPattern& p,const char* name,double value,const char* comparison,double threshold) {
    if(!std::isfinite(value)) throw std::invalid_argument("nonfinite review measurement");
    p.checks.push_back({name,comparison,value,threshold,true,comparison[0]=='>'?value>=threshold:value<=threshold});
}
void complete(ReviewedPattern& p,const WaveformReview& review,bool measured,bool requested) {
    p.pattern_consistent=measured && std::all_of(p.checks.begin(),p.checks.end(),[](const auto& c){return c.passed;});
    if(!measured) {
        p.status=requested?"unavailable":"not_requested";
        p.reasons.push_back(requested?"insufficient_measurement_support":"measurement_not_requested");
        return;
    }
    for(const auto& c:p.checks) if(!c.passed) p.reasons.push_back(c.name);
    if(!p.pattern_consistent) {p.status="no_match";return;}
    for(const auto& c:review.quality_checks) if(c.known && !c.passed)p.reasons.push_back(c.name);
    if(review.passband_status!="fits_declared_band")p.reasons.push_back(review.passband_status);
    const bool competing=review.cp_code_ambiguous && (p.kind=="cp_timing" || p.kind=="barker_11");
    if(competing)p.reasons.push_back("competing_cp_and_barker_patterns");
    // Retain all blockers even when precedence selects a single display status.
    p.status=!review.observed_quality_passed?"quality_rejected":
        review.passband_status=="outside_declared_band"?"passband_rejected":
        competing?"ambiguous_patterns":review.passband_status=="unknown"?"passband_unverified":
        p.kind=="ordinary_cyclic" || p.kind=="conjugate_cyclic"?"cyclic_structure_only":
        p.kind=="two_frequency" || p.kind=="two_level_envelope" || p.kind=="linear_sweep"?"morphology_only":"experimental_pattern";
}
}
WaveformReview review_waveform_measurements(const SpectralFeatures& s,const StructureDiscovery& d,
    const WaveformFeatures& w,const EvidenceContext& context) {
    if(d.ofdm.size()>4 || d.spread.size()>4 || w.peaks.size()>8 || d.samples_examined>65536 || w.samples_examined>65536)
        throw std::invalid_argument("oversized waveform review");
    // Reuse the frozen observed-quality checks. No legacy candidates/rules or
    // existing output fields are modified by this separate review.
    auto quality=assess_link_evidence(s,{}, {},context);
    WaveformReview r;r.context=context;r.quality_checks=std::move(quality.quality_checks);
    r.observed_quality_passed=quality.observed_quality_passed;
    r.extent_plus_bin_hz=std::max(std::abs(s.occupied_low_hz),std::abs(s.occupied_high_hz))+s.bin_hz;
    r.passband_status=context.usable_bandwidth_hz<=0 || s.power_fraction.empty()?"unknown":
        r.extent_plus_bin_hz<=context.usable_bandwidth_hz/2?"fits_declared_band":"outside_declared_band";
    const auto cp_match=[](const auto& p){return p.symbols_per_partition>=8 && p.train_prefix>=.5 && p.train_outside<=.1 &&
        p.holdout_prefix>=.5 && p.holdout_outside<=.1 && p.holdout_prefix-p.holdout_outside>=.4 && p.holdout_cyclic>=.005;};
    const auto code_match=[](const auto& p){return p.train_words>=8 && p.holdout_words>=8 && p.train_code_coherence_squared>=.8 &&
        p.holdout_code_coherence_squared>=.8 && p.train_code_coherence_squared-p.train_other_phase>=.5 &&
        p.holdout_code_coherence_squared-p.holdout_other_phase>=.5;};
    r.cp_code_ambiguous=d.status=="measured" && std::any_of(d.ofdm.begin(),d.ofdm.end(),cp_match) &&
        std::any_of(d.spread.begin(),d.spread.end(),code_match);
    for(std::size_t i=0;i<d.ofdm.size();++i) {
        const auto& m=d.ofdm[i];ReviewedPattern p;p.kind="cp_timing";p.label="CP "+std::to_string(m.useful_samples)+" + "+std::to_string(m.prefix_samples)+" samples";p.measurement_index=i;
        add(p,"symbols_per_partition",m.symbols_per_partition,">=",8);
        add(p,"train_cp_squared_coherence",m.train_prefix,">=",.5);add(p,"train_outside_squared_coherence",m.train_outside,"<=",.1);
        add(p,"holdout_cp_squared_coherence",m.holdout_prefix,">=",.5);add(p,"holdout_outside_squared_coherence",m.holdout_outside,"<=",.1);
        add(p,"holdout_cp_contrast",m.holdout_prefix-m.holdout_outside,">=",.4);add(p,"holdout_symbol_squared_coherence",m.holdout_cyclic,">=",.005);
        complete(p,r,d.status=="measured",d.samples_examined>0);r.candidates.push_back(std::move(p));
    }
    for(std::size_t i=0;i<d.spread.size();++i) {
        const auto& m=d.spread[i];ReviewedPattern p;p.kind="barker_11";p.label="Barker-11, "+std::to_string(m.chip_samples)+" samples/chip";p.measurement_index=i;
        add(p,"train_code_words",m.train_words,">=",8);add(p,"holdout_code_words",m.holdout_words,">=",8);
        add(p,"train_code_squared_coherence",m.train_code_coherence_squared,">=",.8);add(p,"holdout_code_squared_coherence",m.holdout_code_coherence_squared,">=",.8);
        add(p,"train_code_phase_contrast",m.train_code_coherence_squared-m.train_other_phase,">=",.5);
        add(p,"holdout_code_phase_contrast",m.holdout_code_coherence_squared-m.holdout_other_phase,">=",.5);
        complete(p,r,d.status=="measured",d.samples_examined>0);r.candidates.push_back(std::move(p));
    }
    for(const auto kind:{"cp_timing","barker_11"})if(std::none_of(r.candidates.begin(),r.candidates.end(),[&](const auto& p){return p.kind==kind;})) {
        ReviewedPattern p;p.kind=kind;p.label=kind==std::string("cp_timing")?"General CP timing":"Barker-11 compatibility";
        complete(p,r,false,d.samples_examined>0);r.candidates.push_back(std::move(p));
    }
    ReviewedPattern frequency;frequency.kind="two_frequency";frequency.label="Two-frequency shape";
    add(frequency,"phase_support_fraction",w.samples_examined>1?double(w.phase_pairs)/(w.samples_examined-1):0,">=",.8);
    add(frequency,"amplitude_cv",w.amplitude_cv,"<=",.3);add(frequency,"frequency_concentration",w.frequency_concentration,">=",.8);
    add(frequency,"first_state_fraction",w.frequency_first_fraction,">=",.1);add(frequency,"second_state_fraction",w.frequency_second_fraction,">=",.1);
    add(frequency,"frequency_transitions",w.frequency_transitions,">=",8);
    complete(frequency,r,w.morphology_status=="measured",w.samples_examined>0);r.candidates.push_back(std::move(frequency));
    ReviewedPattern envelope;envelope.kind="two_level_envelope";envelope.label="Two-level envelope";
    add(envelope,"envelope_contrast",w.envelope_contrast,">=",.7);add(envelope,"envelope_fit_residual",w.envelope_fit_residual,"<=",.05);
    add(envelope,"envelope_high_fraction_min",w.envelope_high_fraction,">=",.1);add(envelope,"envelope_high_fraction_max",w.envelope_high_fraction,"<=",.9);
    add(envelope,"envelope_transitions",w.envelope_transitions,">=",8);
    complete(envelope,r,w.morphology_status=="measured",w.samples_examined>0);r.candidates.push_back(std::move(envelope));
    for(std::size_t i=0;i<w.peaks.size();++i) {
        const auto& m=w.peaks[i];ReviewedPattern p;p.kind=m.conjugate?"conjugate_cyclic":"ordinary_cyclic";
        p.label=m.conjugate?"Conjugate cyclic pattern":"Ordinary cyclic pattern";p.measurement_index=i;
        add(p,"discovery_cyclic_squared_coherence",m.discovery_coherence_squared,">=",.05);
        add(p,"holdout_cyclic_squared_coherence",m.holdout_coherence_squared,">=",.05);
        complete(p,r,w.cyclic_status=="measured",w.samples_examined>0);r.candidates.push_back(std::move(p));
    }
    for(const auto kind:{"ordinary_cyclic","conjugate_cyclic"})if(std::none_of(r.candidates.begin(),r.candidates.end(),[&](const auto& p){return p.kind==kind;})) {
        ReviewedPattern p;p.kind=kind;p.label=kind==std::string("ordinary_cyclic")?"Ordinary cyclic pattern":"Conjugate cyclic pattern";
        // A measured search that finds no retained peaks is a no-match, whereas
        // absent/short/constant inputs have no measurement support.
        if(w.cyclic_status=="measured")p.status="no_match";
        else complete(p,r,false,w.samples_examined>0);
        r.candidates.push_back(std::move(p));
    }
    if(r.candidates.size()>review_candidate_limit)throw std::invalid_argument("waveform review candidate cap");
    const bool any=std::any_of(r.candidates.begin(),r.candidates.end(),[](const auto& p){return p.pattern_consistent;});
    const bool measured=d.status=="measured" || w.cyclic_status=="measured" || w.morphology_status=="measured";
    r.status=!any?(measured?"no_supported_pattern":"unavailable"):!r.observed_quality_passed?"quality_rejected":
        r.passband_status=="outside_declared_band"?"passband_rejected":r.cp_code_ambiguous?"ambiguous_patterns":
        r.passband_status=="unknown"?"passband_unverified":
        std::any_of(r.candidates.begin(),r.candidates.end(),[](const auto& p){return p.status=="experimental_pattern";})?
        "experimental_patterns":"diagnostic_patterns_only";
    return r;
}
WaveformReview review_linear_sweeps(const SpectralFeatures& s,const ChirpDiscovery& d,const EvidenceContext& context) {
    if(d.candidates.size()>sweep_candidate_limit || d.samples_examined>65536)
        throw std::invalid_argument("oversized linear sweep review");
    auto r=review_waveform_measurements(s,{}, {},context);r.candidates.clear();
    for(std::size_t i=0;i<d.candidates.size();++i) {
        const auto& m=d.candidates[i];ReviewedPattern p;p.kind="linear_sweep";p.measurement_index=i;
        p.label=m.slope_hz_per_second>=0?"Upward linear sweep":"Downward linear sweep";
        add(p,"held_window_supported",m.holdout_supported,">=",1);
        add(p,"observed_sweep_min_sample_rate_fraction",m.sweep_hz/context.sample_rate_hz,">=",.04);
        add(p,"observed_sweep_max_sample_rate_fraction",m.sweep_hz/context.sample_rate_hz,"<=",.7);
        add(p,"discovery_phase_frequency_rmse_fraction",m.discovery_rmse_fraction,"<=",.003);
        add(p,"held_fixed_slope_rmse_fraction",m.holdout_rmse_fraction,"<=",.003);
        add(p,"discovery_dechirp_coherence_squared",m.discovery_coherence_squared,">=",.65);
        add(p,"held_dechirp_coherence_squared",m.holdout_coherence_squared,">=",.65);
        add(p,"discovery_amplitude_cv",m.discovery_amplitude_cv,"<=",.5);
        add(p,"held_amplitude_cv",m.holdout_amplitude_cv,"<=",.5);
        complete(p,r,d.status=="measured",d.samples_examined>0);r.candidates.push_back(std::move(p));
    }
    const bool any=std::any_of(r.candidates.begin(),r.candidates.end(),[](const auto& p){return p.pattern_consistent;});
    r.status=!any?(d.status=="measured"?"no_supported_pattern":"unavailable"):!r.observed_quality_passed?"quality_rejected":
        r.passband_status=="outside_declared_band"?"passband_rejected":r.passband_status=="unknown"?"passband_unverified":"diagnostic_patterns_only";
    return r;
}
WaveformReview review_cyclic_background(const SpectralFeatures& s,const WaveformFeatures& w,
    const CyclicBackground& b,const EvidenceContext& context) {
    if(b.peaks.size()!=w.peaks.size() || b.peaks.size()>8 || b.samples_examined>65536)
        throw std::invalid_argument("invalid cyclic background review");
    auto r=review_waveform_measurements(s,{}, {},context);r.candidates.clear();
    for(std::size_t i=0;i<b.peaks.size();++i) {
        const auto& raw=w.peaks[i];const auto& m=b.peaks[i];ReviewedPattern p;p.measurement_index=i;
        p.kind=raw.conjugate?"conjugate_cyclic":"ordinary_cyclic";p.label=raw.conjugate?"Conjugate line background":"Ordinary line background";
        add(p,"discovery_cyclic_squared_coherence",raw.discovery_coherence_squared,">=",.05);
        add(p,"holdout_cyclic_squared_coherence",raw.holdout_coherence_squared,">=",.05);
        for(const auto which:{0,1}) {
            const auto& part=which?m.holdout:m.discovery;
            const std::string prefix=which?"held_":"discovery_";
            add(p,(prefix+"local_reference_bins").c_str(),part.reference_bins,">=",16);
            add(p,(prefix+"line_to_local_median").c_str(),part.line_to_median,">=",16);
            add(p,(prefix+"line_to_local_upper_background").c_str(),part.line_to_upper,">=",8);
            add(p,(prefix+"block_phase_coherence_squared").c_str(),part.block_phase_coherence_squared,">=",.8);
            add(p,(prefix+"min_block_product_energy_fraction").c_str(),part.min_block_energy_fraction,">=",.02);
        }
        complete(p,r,b.status=="measured",b.samples_examined>0);
        if(p.status=="no_match" && raw.discovery_coherence_squared>=.05 && raw.holdout_coherence_squared>=.05)p.status="background_rejected";
        r.candidates.push_back(std::move(p));
    }
    const bool any=std::any_of(r.candidates.begin(),r.candidates.end(),[](const auto& p){return p.pattern_consistent;});
    r.status=!any?(b.status=="measured"?"no_supported_pattern":"unavailable"):!r.observed_quality_passed?"quality_rejected":
        r.passband_status=="outside_declared_band"?"passband_rejected":r.passband_status=="unknown"?"passband_unverified":"diagnostic_patterns_only";
    return r;
}
} // namespace rfmon::cyclo
