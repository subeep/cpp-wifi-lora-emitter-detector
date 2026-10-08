#include "cyclostationary/waveform_review.hpp"
#include <iostream>
#include <stdexcept>
#include <cmath>
using namespace rfmon::cyclo;
namespace {
int failures=0;
void check(bool ok,const char* why){if(!ok){std::cerr<<"FAIL: "<<why<<'\n';++failures;}}
const ReviewedPattern& pattern(const WaveformReview& r,const char* kind){for(const auto& p:r.candidates)if(p.kind==kind)return p;throw std::runtime_error("missing review group");}
SpectralFeatures good(){SpectralFeatures s;s.quality="measured";s.frames=64;s.variance_power=1;s.power_fraction.assign(512,1./512);s.bin_hz=20e6/512;s.occupied_low_hz=-5e6;s.occupied_high_hz=5e6;s.strongest_bin_fraction=1./512;return s;}
StructureDiscovery cp(){StructureDiscovery d;d.samples_examined=65536;d.status="measured";d.partition_samples=8192;d.ofdm.push_back({96,24,0,64,.04,.9,.01,.9,.01,.03,false});return d;}
StructureDiscovery code(){auto d=cp();d.ofdm.clear();d.spread.push_back({8,0,90,90,3e6,.95,.95,.1,.1,false});return d;}
void tests(){
    const EvidenceContext band{20e6,16e6,-1};auto s=good();auto d=cp();WaveformFeatures w;
    auto r=review_waveform_measurements(s,d,w,band);
    check(pattern(r,"cp_timing").pattern_consistent && pattern(r,"cp_timing").status=="experimental_pattern","derive pattern from measurements, not supplied flag");
    check(pattern(r,"two_frequency").status=="not_requested","absent morphology was treated as a negative result");
    check(!r.quality_checks.back().known,"unknown float ADC rails pretended known");
    auto unknown=review_waveform_measurements(s,d,w,{20e6,0,-1});
    check(pattern(unknown,"cp_timing").status=="passband_unverified","Fs substituted for usable receiver width");
    auto clipped=review_waveform_measurements(s,d,w,{20e6,8e6,-1});
    check(pattern(clipped,"cp_timing").status=="passband_rejected","incomplete declared coverage ignored");
    auto dirty=s;dirty.dc_fraction=.2;
    auto quality=review_waveform_measurements(dirty,d,w,band);
    check(pattern(quality,"cp_timing").status=="quality_rejected" && pattern(quality,"cp_timing").pattern_consistent,"raw pattern hidden by quality rejection");
    auto rails=review_waveform_measurements(s,d,w,{20e6,16e6,.02});check(rails.status=="quality_rejected","known source rails ignored");
    d.spread=code().spread;auto both=review_waveform_measurements(s,d,w,band);
    check(both.cp_code_ambiguous && pattern(both,"cp_timing").status=="ambiguous_patterns" && pattern(both,"barker_11").status=="ambiguous_patterns","CP/Barker competing explanations incorrectly resolved");
    auto blocked=review_waveform_measurements(dirty,d,w,{20e6,8e6,-1});
    check(blocked.cp_code_ambiguous && pattern(blocked,"cp_timing").reasons.size()>=3,"multiple blockers discarded by status precedence");
    d=cp();d.ofdm[0].holdout_prefix=.1;d.ofdm[0].pattern_consistent=true;
    auto held=review_waveform_measurements(s,d,w,band);check(!pattern(held,"cp_timing").pattern_consistent && !pattern(held,"cp_timing").reasons.empty(),"holdout failure promoted by raw flag");
    d=code();d.spread[0].holdout_words=7;check(!pattern(review_waveform_measurements(s,d,w,band),"barker_11").pattern_consistent,"short held code support accepted");
    d.spread[0].holdout_words=90;d.spread[0].holdout_other_phase=.6;
    check(!pattern(review_waveform_measurements(s,d,w,band),"barker_11").pattern_consistent,"weak competing-phase margin accepted");
    w.samples_examined=65536;w.morphology_status="measured";w.cyclic_status="measured";
    w.phase_pairs=65535;w.amplitude_cv=.1;w.frequency_first_fraction=.5;w.frequency_second_fraction=.5;w.frequency_concentration=1;w.frequency_transitions=20;
    w.peaks.push_back({false,0,1e6,1e6,.8,.001,true});
    auto shapes=review_waveform_measurements(s,{},w,band);
    check(pattern(shapes,"two_frequency").pattern_consistent && !pattern(shapes,"ordinary_cyclic").pattern_consistent,"shape and discovery-only cyclic evidence conflated");
    check(pattern(shapes,"two_frequency").status=="morphology_only" && shapes.status=="diagnostic_patterns_only","shape-only result promoted to specific structure");
    w.peaks[0].holdout_coherence_squared=.8;w.frequency_concentration=0;
    auto cycles=review_waveform_measurements(s,{},w,band);
    check(pattern(cycles,"ordinary_cyclic").status=="cyclic_structure_only" && cycles.status=="diagnostic_patterns_only","cyclic-only result promoted to specific structure");
    auto missing=review_waveform_measurements({}, {}, {},band);check(missing.status=="unavailable","absent measurements reported as a supported negative");
    auto full=cp();full.ofdm.resize(4,full.ofdm.front());full.spread.resize(4,code().spread.front());w.peaks.resize(8);for(std::size_t j=0;j<8;++j)w.peaks[j].conjugate=j>=4;
    check(review_waveform_measurements(s,full,w,band).candidates.size()==review_candidate_limit,"full retained profile exceeds metadata budget");
    ChirpDiscovery sweeps;sweeps.status="measured";sweeps.samples_examined=65536;
    LinearSweep sw;sw.sweep_hz=8e6;sw.discovery_coherence_squared=.99;sw.holdout_coherence_squared=.99;sw.holdout_supported=true;
    sweeps.candidates.push_back(sw);
    auto sr=review_linear_sweeps(s,sweeps,band);
    check(sr.status=="diagnostic_patterns_only" && pattern(sr,"linear_sweep").status=="morphology_only","sweep promoted to drone or specific structure");
    check(review_linear_sweeps(dirty,sweeps,band).status=="quality_rejected","sweep bypasses observed quality gate");
    check(review_linear_sweeps(s,sweeps,{20e6,8e6,-1}).status=="passband_rejected","sweep bypasses declared passband");
    sweeps.candidates[0].holdout_supported=false;sweeps.candidates[0].pattern_consistent=true;
    check(!pattern(review_linear_sweeps(s,sweeps,band),"linear_sweep").pattern_consistent,"sweep ignores unsupported held samples");
    WaveformFeatures selected;selected.peaks.push_back({true,0,1e6,1e6,.8,.8,true});
    CyclicBackground background;background.status="measured";background.samples_examined=65536;
    CyclicBackgroundPeak peak;
    for(auto* part:{&peak.discovery,&peak.holdout}){part->reference_bins=34;part->line_to_median=50;part->line_to_upper=20;
        part->block_phase_coherence_squared=.95;part->min_block_energy_fraction=.05;}
    background.peaks.push_back(peak);
    auto br=review_cyclic_background(s,selected,background,band);
    check(br.status=="diagnostic_patterns_only" && br.candidates[0].pattern_consistent,"background-supported cycle assigned excess specificity or missing");
    background.peaks[0].holdout.line_to_upper=1;background.peaks[0].background_supported=true;
    br=review_cyclic_background(s,selected,background,band);
    check(br.candidates[0].status=="background_rejected" && !br.candidates[0].pattern_consistent && !br.candidates[0].reasons.empty(),"background rejection hidden or supplied flag trusted");
    auto reject=[&](auto f){bool failed=false;try{f();}catch(const std::invalid_argument&){failed=true;}check(failed,"invalid review accepted");};
    full.ofdm.push_back(full.ofdm.front());reject([&]{review_waveform_measurements(s,full,w,band);});
    d=cp();d.ofdm[0].holdout_prefix=NAN;reject([&]{review_waveform_measurements(s,d,w,band);});
}
}
int main(){try{tests();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}return failures?1:0;}
