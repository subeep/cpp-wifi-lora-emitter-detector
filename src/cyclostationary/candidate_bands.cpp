#include "candidate_bands.hpp"
#include "analysis_samples.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rfmon::cyclo {
CandidateBands prepare_candidate_bands(const std::vector<std::complex<float>>& iq, double rate,
                                      const RoiMeasurements& rois, double source_width) {
    if(!std::isfinite(rate) || rate<=0 || rate>1e9 || iq.size()>65536 ||
       !std::isfinite(source_width) || source_width<0 || source_width>rate || rois.samples_examined!=iq.size() || rois.regions.size()>roi_region_limit)
        throw std::invalid_argument("invalid candidate profile");
    for(auto z:iq) if(!std::isfinite(z.real()) || !std::isfinite(z.imag())) throw std::invalid_argument("nonfinite candidate IQ");
    struct Proposal { CandidateBand band; double priority; };
    std::vector<Proposal> proposals;
    CandidateBands result; result.regions_seen=rois.regions.size();
    for(std::size_t ri=0;ri<rois.regions.size();++ri) {
        const auto& region=rois.regions[ri];
        if(region.bands.size()>roi_band_limit || !std::isfinite(region.energy_fraction) || region.energy_fraction<0 || region.energy_fraction>1)
            throw std::invalid_argument("invalid candidate region support");
        if(region.offset>iq.size() || region.samples>iq.size()-region.offset)
            throw std::invalid_argument("candidate ROI escapes input");
        if(region.spectral_status!="measured") {++result.regions_without_spectrum;continue;}
        if(region.samples<2064) {++result.short_regions;continue;}
        auto add=[&](double low,double high,double fraction,const char* kind) {
            if(!std::isfinite(low) || !std::isfinite(high) || low>=high || low < -rate/2 || high>rate/2 ||
               !std::isfinite(fraction) || fraction<0 || fraction>1)
                throw std::invalid_argument("invalid candidate interval");
            CandidateBand c; c.source_offset=region.offset; c.source_samples=region.samples;
            c.region_index=ri;c.proposal_kind=kind;
            c.offset_hz=(low+high)/2; c.width_hz=high-low+4*rate/roi_fft_size;
            proposals.push_back({c,region.energy_fraction*fraction});
        };
        // Full region power span gets priority, preserving multiple spectral
        // states. Narrow interval proposals supplement it, not replace context.
        add(region.occupied_low_hz,region.occupied_high_hz,1,"occupied_span");
        if(region.bands.size()>=2) {
            double low=rate/2,high=-rate/2,fraction=0;
            for(const auto& b:region.bands) {low=std::min(low,b.low_hz);high=std::max(high,b.high_hz);fraction+=b.power_fraction;}
            // Preserve separated spectral states as one proposal; a narrow
            // band around just one FSK state would erase the distinguishing shape.
            add(low,high,std::min(1.0,fraction)*.95,"joint_bands");
        }
        for(const auto& b:region.bands) add(b.low_hz,b.high_hz,b.power_fraction*.9,"spectral_band");
    }
    auto eligible=[&](const Proposal& p){return source_width>0 && std::abs(p.band.offset_hz)+p.band.width_hz/2<source_width/2 && p.band.width_hz<=.8*rate;};
    std::stable_sort(proposals.begin(),proposals.end(),[&](const Proposal& a,const Proposal& b){
        if(eligible(a)!=eligible(b)) return eligible(a)>eligible(b);
        return a.priority>b.priority;
    });
    result.proposals_seen=proposals.size();
    result.geometry_eligible_proposals=std::count_if(proposals.begin(),proposals.end(),eligible);
    for(auto proposal:proposals) {
        auto c=proposal.band;
        bool duplicate=false; for(const auto& old:result.candidates) if(old.source_offset==c.source_offset &&
            std::abs(old.offset_hz-c.offset_hz)<rate/roi_fft_size && std::abs(old.width_hz-c.width_hz)<rate/roi_fft_size) duplicate=true;
        if(duplicate) {++result.duplicates_of_returned;continue;}
        // Inspect remaining metadata only; never run extra FIR or feature work.
        if(result.candidates.size()==prepared_candidate_limit) {++result.budget_omitted;continue;}
        if(source_width>0) {
            c.status="outside_declared_passband";
            if(std::abs(c.offset_hz)+c.width_hz/2<source_width/2 && c.width_hz<=.8*rate) {
                std::vector<std::complex<float>> private_iq(iq.begin()+c.source_offset,iq.begin()+c.source_offset+c.source_samples);
                c.status="filter_unavailable";
                auto decimation=std::min<std::size_t>(16,std::max<std::size_t>(1,std::size_t(std::floor(.8*rate/c.width_hz))));
                for(;decimation;--decimation) {
                    const double stop=std::min(rate/decimation/2,source_width/2-std::abs(c.offset_hz));
                    const double transition=stop-c.width_hz/2;
                    if(transition<=0) continue;
                    const double wanted=std::ceil(3.3*rate/transition);
                    if(wanted>1023) continue;
                    auto taps=std::max<std::size_t>(33,std::size_t(wanted)); if(taps%2==0) ++taps;
                    if(private_iq.size()<taps) continue;
                    const auto expected_operations=(1+(private_iq.size()-taps)/decimation)*taps;
                    if(expected_operations>64000000-result.fir_operations) { c.status="fir_budget_exceeded"; break; }
                    BandSelection selection{rate,source_width,c.offset_hz,c.width_hz,decimation,false};
                    PreparedSamples prepared;
                    try { prepared=select_analysis_band(private_iq,selection); }
                    catch(const std::invalid_argument&) { continue; }
                    const auto operations=prepared.samples.size()*prepared.filter_taps;
                    if(operations>64000000-result.fir_operations) { c.status="fir_budget_exceeded"; break; }
                    result.fir_operations+=operations;
                    c.first_input_center=c.source_offset+prepared.first_input_center; c.input_step=prepared.input_step;
                    c.filter_taps=prepared.filter_taps; c.output_samples=prepared.samples.size(); c.output_rate_hz=prepared.sample_rate_hz;
                    c.waveform=measure_waveform_features(prepared.samples,prepared.sample_rate_hz);
                    c.status="prepared_declared_passband"; break;
                }
            }
        }
        result.candidates.push_back(std::move(c));
    }
    return result;
}
} // namespace rfmon::cyclo
