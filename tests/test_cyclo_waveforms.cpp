#include "cyclostationary/waveform_features.hpp"
#include "cyclostationary/candidate_bands.hpp"
#include "cyclostationary/analysis_samples.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace rfmon::cyclo;
namespace {
constexpr double pi=3.14159265358979323846;
using IQ=std::vector<std::complex<float>>;
int failures=0;
void check(bool ok,const char* message) {if(!ok){std::cerr<<"FAIL: "<<message<<'\n';++failures;}}
template<class F> void rejects(F f,const char* message) {try{f();check(false,message);}catch(const std::exception&) {}}
IQ noise(unsigned seed,std::size_t n=65536) {
    std::mt19937 engine(seed); std::normal_distribution<float> normal; IQ x(n);
    for(auto& z:x) z={normal(engine),normal(engine)}; return x;
}
// Independent oracle: absolute exponent at each lag pair, ordinary/conjugate
// definitions and separate source means; no FFT or oscillator recurrence.
double oracle(const IQ& x,std::size_t at,std::size_t n,const RatePeak& p,double rate) {
    std::complex<double> mean{}; for(std::size_t j=0;j<n;++j) mean+=std::complex<double>(x[at+j]); mean/=double(n);
    std::complex<double> sum{}; double a=0,b=0; const auto pairs=n-p.lag_samples;
    for(std::size_t j=0;j<pairs;++j) {
        const auto u=std::complex<double>(x[at+j+p.lag_samples])-mean,v=std::complex<double>(x[at+j])-mean;
        const double w=.5-.5*std::cos(2*pi*j/pairs);
        sum+=w*u*(p.conjugate?v:std::conj(v))*std::polar(1.0,-2*pi*p.alpha_hz*(at+j)/rate);
        a+=w*std::norm(u); b+=w*std::norm(v);
    }
    return std::norm(sum)/(a*b);
}
IQ fsk(double rate,double carrier,double deviation,std::size_t n=65536) {
    std::mt19937 e(722); IQ x(n); double phase=0; bool bit=false;
    for(std::size_t j=0;j<n;++j) {if(j%64==0) bit=e()&1; phase+=2*pi*(carrier+(bit?deviation:-deviation))/rate; x[j]=std::complex<float>(std::polar(1.0,phase));}
    return x;
}
void cyclic_tests() {
    const double rate=20e6,alpha=72.25*rate/8192;
    auto x=noise(171);
    for(std::size_t j=0;j<x.size();++j) x[j]*=float(1+.8*std::cos(2*pi*alpha*j/rate));
    auto r=measure_waveform_features(x,rate);
    check(r.cyclic_status=="measured" && r.partition_samples==8192 && r.holdout_offset==x.size()-8192,"bounded disjoint partitions");
    check(r.peaks.size()<=8 && r.fft_calls==8 && r.refinement_evaluations<=40,"search is bounded");
    bool recovered=false;
    for(const auto& p:r.peaks) {
        check(std::abs(oracle(x,0,r.partition_samples,p,rate)-p.discovery_coherence_squared)<2e-7,"discovery direct-sum oracle");
        check(std::abs(oracle(x,r.holdout_offset,r.partition_samples,p,rate)-p.holdout_coherence_squared)<2e-7,"holdout direct-sum oracle");
        recovered|=!p.conjugate && std::abs(p.alpha_hz-alpha)<.26*r.alpha_bin_hz && p.persistent_pattern;
    }
    check(recovered,"off-grid periodic envelope recovered on holdout");
    auto changed=x; auto null=noise(172);
    for(std::size_t j=r.holdout_offset;j<x.size();++j) changed[j]=null[j];
    auto held=measure_waveform_features(changed,rate); bool absent=false;
    for(const auto& p:held.peaks) if(!p.conjugate && std::abs(p.alpha_hz-alpha)<r.alpha_bin_hz) absent|=!p.persistent_pattern && p.holdout_coherence_squared<.05;
    check(absent,"holdout rejects discovery-only periodicity without refitting");
    for(double gain:{1e-25,1e25}) {
        auto scaled=x; for(auto& z:scaled) z*=float(gain); auto s=measure_waveform_features(scaled,rate);
        check(s.peaks.size()==r.peaks.size(),"scale invariant peak count");
        for(std::size_t j=0;j<std::min(s.peaks.size(),r.peaks.size());++j)
            check(std::abs(s.peaks[j].holdout_coherence_squared-r.peaks[j].holdout_coherence_squared)<1e-5,"scale invariant coherence");
    }
    IQ tone(65536); const double carrier=313.25*rate/8192;
    for(std::size_t j=0;j<tone.size();++j) tone[j]=std::complex<float>(std::polar(1.0,2*pi*carrier*j/rate));
    auto t=measure_waveform_features(tone,rate); bool conjugate=false;
    for(const auto& p:t.peaks) conjugate|=p.conjugate && std::abs(p.alpha_hz-2*carrier)<r.alpha_bin_hz && p.holdout_coherence_squared>.99;
    check(conjugate,"conjugate tone cycle has correct sign/frequency; deliberately a confusable");
    check(!t.two_frequency_pattern && !t.two_level_envelope_pattern,"single tone is not a two-state shape");
    for(unsigned seed=100;seed<120;++seed) {
        auto n=measure_waveform_features(noise(seed),rate);
        check(!n.two_frequency_pattern && !n.two_level_envelope_pattern,"fresh noise does not match two-state morphology");
        for(const auto& p:n.peaks) check(!p.persistent_pattern,"fresh noise has no persistent CAF pattern");
    }
    auto zero=measure_waveform_features(IQ(65536),rate);
    check(zero.cyclic_status=="no_variation" && zero.morphology_status=="no_energy" && zero.peaks.empty(),"zero input abstains");
    auto short_input=measure_waveform_features(IQ(16,{1,0}),rate);
    check(short_input.cyclic_status=="insufficient_samples" && short_input.morphology_status=="insufficient_samples","short input abstains");
    rejects([&]{measure_waveform_features(IQ(65537),rate);},"oversized input accepted");
    rejects([&]{measure_waveform_features(IQ(4000,{NAN,0}),rate);},"nonfinite input accepted");
    rejects([&]{measure_waveform_features(x,0);},"invalid rate accepted");
}
void morphology_and_candidates() {
    const double rate=20e6; auto x=fsk(rate,3e6,.5e6);
    auto r=measure_waveform_features(x,rate);
    check(r.two_frequency_pattern && r.frequency_concentration>.99 && r.frequency_transitions>100,"constant-envelope two-frequency control");
    check(std::abs(r.frequency_low_hz-2.5e6)<100 && std::abs(r.frequency_high_hz-3.5e6)<100,"frequency-state centres");
    IQ ook(65536); std::mt19937 e(32); bool on=false;
    for(std::size_t j=0;j<ook.size();++j) {if(j%64==0) on=e()&1; ook[j]=std::complex<float>(std::polar(on?1.0:0.0,2*pi*2e6*j/rate));}
    auto a=measure_waveform_features(ook,rate);
    check(a.two_level_envelope_pattern && !a.two_frequency_pattern,"OOK-like envelope control, no invented frequency states");
    auto boundary=fsk(rate,9.5e6,1e6); auto wrap=measure_waveform_features(boundary,rate);
    check(wrap.two_frequency_pattern && wrap.frequency_low_hz<0 && wrap.frequency_high_hz>0,"phase-frequency aliasing stays explicit");
    // The context span includes both FSK states. Filtering is independently
    // replayed through the existing FIR to check source mapping and features.
    RoiMeasurements roi; roi.samples_examined=x.size();
    EnergyRegion region; region.samples=x.size(); region.spectral_status="measured"; region.energy_fraction=1;
    region.occupied_low_hz=2e6; region.occupied_high_hz=4e6; roi.regions.push_back(region);
    auto original=x;
    auto unknown=prepare_candidate_bands(x,rate,roi,0);
    check(unknown.candidates.size()==1 && unknown.candidates[0].status=="passband_unknown" && !unknown.fir_operations,"unknown passband does not filter");
    auto outside=prepare_candidate_bands(x,rate,roi,6e6);
    check(outside.candidates[0].status=="outside_declared_passband" && !outside.fir_operations,"partial passband rejected");
    auto prepared=prepare_candidate_bands(x,rate,roi,16e6); const auto& c=prepared.candidates[0];
    check(c.status=="prepared_declared_passband" && c.input_step>1 && c.output_samples<x.size(),"eligible candidate privately decimated");
    check(prepared.fir_operations<=64000000 && c.first_input_center+(c.output_samples-1)*c.input_step<x.size(),"bounded filter support and coordinates");
    auto independent=select_analysis_band(x,{rate,16e6,c.offset_hz,c.width_hz,c.input_step,false});
    auto expected=measure_waveform_features(independent.samples,independent.sample_rate_hz);
    check(c.first_input_center==independent.first_input_center && c.waveform.phase_pairs==expected.phase_pairs &&
        c.waveform.frequency_concentration==expected.frequency_concentration,"candidate filter/mapping replay equality");
    check(x==original,"source IQ remains unchanged");
    auto noisy_roi=roi; noisy_roi.regions[0].occupied_low_hz=-9e6; noisy_roi.regions[0].occupied_high_hz=9e6;
    noisy_roi.regions[0].bands={{2.2e6,2.8e6,.48,false,false},{3.2e6,3.8e6,.48,false,false}};
    auto joint=prepare_candidate_bands(x,rate,noisy_roi,16e6);
    check(joint.candidates[0].status=="prepared_declared_passband" && joint.candidates[0].width_hz>1.6e6 &&
        joint.candidates[0].offset_hz-joint.candidates[0].width_hz/2<=2.2e6 &&
        joint.candidates[0].offset_hz+joint.candidates[0].width_hz/2>=3.8e6,
        "qualified joint band retains both spectral states instead of the invalid wide span");
    for(int i=0;i<7;++i) roi.regions.push_back(region);
    const auto duplicates=prepare_candidate_bands(x,rate,roi,16e6);
    check(duplicates.candidates.size()==1 && duplicates.duplicates_of_returned==7 && !duplicates.budget_omitted,
        "duplicates counted even beyond returned candidate cap");
    check(duplicates.geometry_eligible_proposals==8 && duplicates.regions_seen==8,"all eligible proposal metadata counted");
    auto many=roi;
    for(std::size_t i=0;i<many.regions.size();++i) {many.regions[i].offset=i*8192;many.regions[i].samples=8192;}
    const auto budget=prepare_candidate_bands(x,rate,many,16e6);
    check(budget.candidates.size()==2 && budget.budget_omitted==6 && !budget.duplicates_of_returned,
        "distinct omitted regions counted without extra filters");
    check(budget.proposals_seen==budget.candidates.size()+budget.budget_omitted+budget.duplicates_of_returned,
        "proposal accounting is exhaustive");
    check(budget.candidates[0].region_index==0 && budget.candidates[1].region_index==1 &&
        budget.candidates[0].proposal_kind=="occupied_span","stable equal-priority selection and provenance");
    many.regions[0].spectral_status="insufficient_samples";many.regions[1].samples=1000;
    const auto skip=prepare_candidate_bands(x,rate,many,0);
    check(skip.regions_without_spectrum==1 && skip.short_regions==1 && skip.proposals_seen==6 &&
        !skip.geometry_eligible_proposals && !skip.fir_operations,"short and missing spectra have disjoint skip counts");
    roi.regions[0].samples=x.size()+1;
    rejects([&]{prepare_candidate_bands(x,rate,roi,16e6);},"escaping candidate ROI accepted");
}
}
int main() {try{cyclic_tests();morphology_and_candidates();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}return failures?1:0;}
