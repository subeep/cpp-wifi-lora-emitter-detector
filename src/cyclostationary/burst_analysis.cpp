#include "burst_analysis.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rfmon::cyclo {
BurstSelection select_burst_region(const RoiMeasurements& roi) {
    if(roi.samples_examined>65536 || roi.regions.size()>roi_region_limit)
        throw std::invalid_argument("oversized burst ROI context");
    BurstSelection s;
    for(std::size_t i=0;i<roi.regions.size();++i) {
        const auto& r=roi.regions[i];
        if(!std::isfinite(r.energy_fraction) || r.energy_fraction<0 || r.energy_fraction>1 ||
            r.offset>roi.samples_examined || !r.samples || r.samples>roi.samples_examined-r.offset)
            throw std::invalid_argument("invalid burst region");
        if(roi.status!="contrast_regions" || !r.contrast_selected)continue;
        if(r.samples<burst_min_samples){++s.short_regions;continue;}
        if(!s.eligible_regions || r.energy_fraction>roi.regions[s.region_index].energy_fraction ||
            (r.energy_fraction==roi.regions[s.region_index].energy_fraction && r.offset<roi.regions[s.region_index].offset))
            s.region_index=i;
        ++s.eligible_regions;
    }
    if(!s.eligible_regions) {
        if(s.short_regions)s.status="insufficient_burst_support";
        return s;
    }
    const auto& r=roi.regions[s.region_index];
    s.samples=std::min(r.samples,burst_sample_limit);s.cropped=s.samples<r.samples;
    s.offset=r.offset+(r.samples-s.samples)/2;
    s.budget_skipped_regions=s.eligible_regions-1;s.status="analyzed";
    return s;
}
void review_burst(BurstAnalysis& b,const EvidenceContext& context) {
    if(!b.selection.samples)return;
    b.review=review_waveform_measurements(b.spectral,b.structure,b.waveform,context);
    b.background_review=review_cyclic_background(b.spectral,b.waveform,b.background,context);
    b.sweep_review=review_linear_sweeps(b.spectral,b.sweeps,context);
}
BurstAnalysis analyze_burst(const std::vector<std::complex<float>>& iq,double rate,const RoiMeasurements& roi) {
    if(iq.size()>65536 || roi.samples_examined!=iq.size() || !std::isfinite(rate) || rate<=0 || rate>1e9)
        throw std::invalid_argument("invalid burst input");
    for(const auto& z:iq)if(!std::isfinite(z.real()) || !std::isfinite(z.imag()))
        throw std::invalid_argument("nonfinite burst input");
    BurstAnalysis b;b.selection=select_burst_region(roi);
    if(!b.selection.samples)return b;
    const auto begin=iq.begin()+b.selection.offset;
    const std::vector<std::complex<float>> local(begin,begin+b.selection.samples);
    SpectralConfig cfg;cfg.sample_rate_hz=rate;cfg.max_frames=256;cfg.max_alpha_bins=1;
    b.spectral=measure_spectral_correlation(local,cfg);
    b.spectral.peaks.clear(); // quality/PSD only; rate search is the separate CAF branch
    b.waveform=measure_waveform_features(local,rate);
    b.background=measure_cyclic_background(local,rate,b.waveform);
    b.structure=discover_waveform_structure(local,rate);
    b.sweeps=discover_linear_sweeps(local,rate);
    return b;
}
} // namespace rfmon::cyclo
