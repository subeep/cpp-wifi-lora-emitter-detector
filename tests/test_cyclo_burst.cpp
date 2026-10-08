#include "cyclostationary/burst_analysis.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace rfmon::cyclo;
namespace {
constexpr double pi=3.14159265358979323846;
void check(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
std::size_t supported(const CyclicBackground& b){std::size_t n=0;for(const auto& p:b.peaks)n+=p.background_supported;return n;}
template<class F>void rejects(F f){try{f();}catch(const std::invalid_argument&){return;}throw std::runtime_error("invalid burst input accepted");}
}
int main(){try {
    std::mt19937 rng(20261008);std::normal_distribution<float> normal;
    std::vector<std::complex<float>> x(65536);
    for(std::size_t i=16384;i<24576;++i)x[i]=(rng()%2?1.f:-1.f)*std::polar(1.f,float(pi*(i%256)/128));
    const auto original=x;const auto roi=measure_rois(x,20e6);
    auto b=analyze_burst(x,20e6,roi);
    check(b.selection.status=="analyzed" && b.selection.offset==16384 && b.selection.samples==8192 &&
        b.selection.eligible_regions==1 && !b.selection.cropped,"central burst selection differs from raw coordinates");
    check(supported(b.background)>0,"local conjugate burst lost cyclic support");
    auto whole=measure_waveform_features(x,20e6);
    check(supported(measure_cyclic_background(x,20e6,whole))==0,"whole-tile control unexpectedly sees middle-only burst");
    const std::vector<std::complex<float>> local(x.begin()+16384,x.begin()+24576);
    const auto oracle=measure_waveform_features(local,20e6);
    check(b.waveform.peaks.size()==oracle.peaks.size() && b.waveform.peaks[0].alpha_hz==oracle.peaks[0].alpha_hz &&
        b.waveform.peaks[0].holdout_coherence_squared==oracle.peaks[0].holdout_coherence_squared && x==original,
        "burst differs from direct contiguous-crop oracle or mutates input");
    review_burst(b,{20e6,0,-1});check(b.background_review.passband_status=="unknown","live passband was invented");
    review_burst(b,{20e6,1e6,-1});check(b.background_review.passband_status=="outside_declared_band","local passband rejection absent");
    // Independent bursts remain independent; select strongest eligible region,
    // count short and budget-skipped regions, and freeze earliest equal-energy tie.
    std::fill(x.begin(),x.end(),std::complex<float>{});
    for(std::size_t i=4096;i<8192;++i)x[i]=i%2?std::complex<float>{1,1}:std::complex<float>{-1,-1};
    for(std::size_t i=20480;i<28672;++i)x[i]=i%2?std::complex<float>{2,2}:std::complex<float>{-2,-2};
    for(std::size_t i=40960;i<41984;++i)x[i]=i%2?std::complex<float>{3,3}:std::complex<float>{-3,-3};
    const auto multiple=measure_rois(x,20e6);const auto sel=select_burst_region(multiple);
    check(sel.eligible_regions==2 && sel.short_regions==1 && sel.budget_skipped_regions==1 && sel.offset==20480,
        "short/disjoint bursts joined or selection budget not disclosed");
    auto tied=multiple;tied.regions[0].energy_fraction=tied.regions[1].energy_fraction;
    check(select_burst_region(tied).offset==4096,"equal-energy selection not deterministic");
    std::fill(x.begin(),x.end(),std::complex<float>{});
    for(std::size_t i=8192;i<40960;++i)x[i]=i%2?std::complex<float>{1,1}:std::complex<float>{-1,-1};
    const auto cropped=analyze_burst(x,20e6,measure_rois(x,20e6));
    check(cropped.selection.cropped && cropped.selection.samples==16384 && cropped.selection.offset==16384,
        "oversized burst not bounded by disclosed central crop");
    std::fill(x.begin(),x.end(),std::complex<float>{});for(std::size_t i=16384;i<24576;++i)x[i]={1,1};
    const auto constant=analyze_burst(x,20e6,measure_rois(x,20e6));
    check(constant.selection.samples==8192 && constant.spectral.quality=="no_variation" && constant.waveform.cyclic_status=="no_variation",
        "constant burst treated as modulated structure");
    for(auto& z:x)z={normal(rng),normal(rng)};
    check(!analyze_burst(x,20e6,measure_rois(x,20e6)).selection.samples,"uniform noise redundantly analyzed as burst");
    std::fill(x.begin(),x.end(),std::complex<float>{});for(std::size_t i=10240;i<11264;++i)x[i]={1,0};
    check(analyze_burst(x,20e6,measure_rois(x,20e6)).selection.status=="insufficient_burst_support",
        "short burst padded to fabricate support");
    // Noise inside a high-energy gate is a real activity ROI, not a drone or
    // coherent periodic waveform. Test several independent development seeds.
    for(int seed=0;seed<8;++seed){std::fill(x.begin(),x.end(),std::complex<float>{});
        for(std::size_t i=16384;i<24576;++i)x[i]={normal(rng),normal(rng)};
        const auto n=analyze_burst(x,20e6,measure_rois(x,20e6));
        check(n.selection.samples>0 && supported(n.background)==0,"gated noise passed local cyclic background controls");
        for(const auto& p:n.structure.ofdm)check(!p.pattern_consistent,"gated noise passed CP checks");
        for(const auto& p:n.structure.spread)check(!p.pattern_consistent,"gated noise passed code checks");
        for(const auto& p:n.sweeps.candidates)check(!p.pattern_consistent,"gated noise passed sweep checks");
    }
    const auto valid_roi=measure_rois(x,20e6);rejects([&]{analyze_burst(x,0,valid_roi);});
    auto bad=valid_roi;bad.regions[0].offset=x.size();rejects([&]{select_burst_region(bad);});
    bad=valid_roi;bad.regions[0].energy_fraction=NAN;rejects([&]{select_burst_region(bad);});
    x.back()={NAN,0};rejects([&]{analyze_burst(x,20e6,valid_roi);});
    std::cout<<"bounded burst recovery/crop/support controls passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
