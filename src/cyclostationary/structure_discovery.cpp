#include "structure_discovery.hpp"
#include "clock_refinement.hpp"
#include <kissfft/kiss_fft.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
constexpr double pi=3.14159265358979323846;
constexpr int code[]={1,1,1,-1,-1,-1,1,-1,-1,1,-1};
using Samples=std::vector<std::complex<double>>;
struct FreeFft {void operator()(kiss_fft_state* p) const {kiss_fft_free(p);}};
Samples centered(const std::vector<std::complex<float>>& iq,std::size_t first,std::size_t n,double scale) {
    Samples x(n);std::complex<double> mean{};
    for(std::size_t j=0;j<n;++j){x[j]=std::complex<double>(iq[first+j])/scale;mean+=x[j];}
    mean/=double(n);for(auto& z:x)z-=mean;return x;
}
double energy(const Samples& x){double sum=0;for(auto z:x)sum+=std::norm(z);return sum;}
struct Moment {
    std::complex<double> cross{};double a=0,b=0;
    Moment& operator+=(const Moment& v){cross+=v.cross;a+=v.a;b+=v.b;return *this;}
    Moment& operator-=(const Moment& v){cross-=v.cross;a-=v.a;b-=v.b;return *this;}
};
double coherence(const Moment& m){return m.a>0&&m.b>0?std::clamp(std::norm(m.cross)/(m.a*m.b),0.0,1.0):0;}
// Fold complete periods of pairs; both endpoints remain in this partition.
// The holdout origin keeps discovery phase in absolute tile coordinates.
std::vector<Moment> fold(const Samples& x,std::size_t lag,std::size_t period,std::size_t origin){
    std::vector<Moment> rows(period);const auto pairs=(x.size()-lag)/period*period;
    for(std::size_t j=0;j<pairs;++j){auto& m=rows[(origin+j)%period];const auto a=x[j+lag],b=x[j];m.cross+=a*std::conj(b);m.a+=std::norm(a);m.b+=std::norm(b);}
    return rows;
}
Moment gate(const std::vector<Moment>& rows,std::size_t phase,std::size_t length){Moment m;for(std::size_t j=0;j<length;++j)m+=rows[(phase+j)%rows.size()];return m;}
Moment total(const std::vector<Moment>& rows){Moment m;for(const auto& r:rows)m+=r;return m;}
DiscoveredOfdm fit_ofdm(const Samples& train,std::size_t lag,std::size_t prefix,double lag_score){
    DiscoveredOfdm r;r.useful_samples=lag;r.prefix_samples=prefix;r.lag_coherence_squared=lag_score;
    const auto period=lag+prefix;r.symbols_per_partition=(train.size()-lag)/period;
    const auto rows=fold(train,lag,period,0); const auto all=total(rows);auto inside=gate(rows,0,prefix);double best=-2;
    for(std::size_t phase=0;phase<period;++phase){auto outside=all;outside-=inside;const double score=coherence(inside)-coherence(outside);
        if(score>best){best=score;r.phase_samples=phase;r.train_prefix=coherence(inside);r.train_outside=coherence(outside);}
        inside-=rows[phase];inside+=rows[(phase+prefix)%period];}
    return r;
}
void hold_ofdm(const Samples& hold,std::size_t origin,DiscoveredOfdm& r){
    const auto period=r.useful_samples+r.prefix_samples;const auto rows=fold(hold,r.useful_samples,period,origin); const auto all=total(rows);
    const auto inside=gate(rows,r.phase_samples,r.prefix_samples);auto outside=all;outside-=inside;
    r.holdout_prefix=coherence(inside);r.holdout_outside=coherence(outside);auto cyclic=all;cyclic.cross={};
    for(std::size_t phase=0;phase<period;++phase)cyclic.cross+=rows[phase].cross*std::polar(1.0,-2*pi*phase/period);
    r.holdout_cyclic=coherence(cyclic);
    r.pattern_consistent=r.symbols_per_partition>=8 && r.train_prefix>=.5 && r.train_outside<=.1 &&
        r.holdout_prefix>=.5 && r.holdout_outside<=.1 && r.holdout_prefix-r.holdout_outside>=.4 && r.holdout_cyclic>=.005;
}
std::vector<DiscoveredOfdm> discover_ofdm(const Samples& train,const Samples& hold,std::size_t origin,StructureDiscovery& result){
    // Zero padding prevents circular wraparound in the lag proposal oracle.
    const auto n=train.size(),size=2*n;
    std::unique_ptr<kiss_fft_state,FreeFft> forward(kiss_fft_alloc(int(size),0,nullptr,nullptr)),inverse(kiss_fft_alloc(int(size),1,nullptr,nullptr)),period_fft(kiss_fft_alloc(int(n),0,nullptr,nullptr));
    if(!forward||!inverse||!period_fft)throw std::bad_alloc();
    std::vector<kiss_fft_cpx> input(size),output(size),correlation(size);
    for(std::size_t j=0;j<n;++j)input[j]={float(train[j].real()),float(train[j].imag())};
    kiss_fft(forward.get(),input.data(),output.data());
    for(std::size_t j=0;j<size;++j)input[j]={output[j].r*output[j].r+output[j].i*output[j].i,0};
    kiss_fft(inverse.get(),input.data(),correlation.data());result.ofdm_fft_calls=2;
    std::vector<double> cumulative(n+1);for(std::size_t j=0;j<n;++j)cumulative[j+1]=cumulative[j]+std::norm(train[j]);
    std::vector<std::pair<double,std::size_t>> lags;
    std::vector<double> scores(result.max_lag+2);
    for(std::size_t lag=15;lag<=result.max_lag+1;++lag){const double a=cumulative[n]-cumulative[lag],b=cumulative[n-lag];
        const std::complex<double> v(correlation[lag].r/size,correlation[lag].i/size);scores[lag]=a>0&&b>0?std::clamp(std::norm(v)/(a*b),0.0,1.0):0;}
    for(std::size_t lag=16;lag<=result.max_lag;++lag)if(scores[lag]>=scores[lag-1]&&scores[lag]>=scores[lag+1])lags.push_back({scores[lag],lag});
    std::stable_sort(lags.begin(),lags.end(),[](auto a,auto b){return a.first>b.first;});
    std::vector<std::pair<double,std::size_t>> selected;
    for(auto lag:lags){bool duplicate=false;for(auto old:selected)duplicate|=std::abs(double(old.second)-lag.second)<3;
        if(!duplicate)selected.push_back(lag);
        if(selected.size()==4)break;}
    std::vector<DiscoveredOfdm> trials;std::vector<kiss_fft_cpx> product(n),spectrum(n);
    for(auto proposal:selected){const auto lag=proposal.second;const auto count=n-lag;
        for(std::size_t j=0;j<n;++j){std::complex<double> v{};if(j<count)v=(.5-.5*std::cos(2*pi*j/count))*train[j+lag]*std::conj(train[j]);product[j]={float(v.real()),float(v.imag())};}
        kiss_fft(period_fft.get(),product.data(),spectrum.data());++result.ofdm_fft_calls;
        std::vector<std::pair<double,std::size_t>> bins;
        for(std::size_t k=1;k<n/2;++k){const double period=double(n)/k;
            if(period<lag+4 || period>lag+lag/2)continue;
            const double power=double(spectrum[k].r)*spectrum[k].r+double(spectrum[k].i)*spectrum[k].i;
            bins.push_back({power,k});}
        std::stable_sort(bins.begin(),bins.end(),[](auto a,auto b){return a.first>b.first;});
        std::set<std::size_t> prefixes;std::vector<std::size_t> order;
        auto add=[&](long prefix){if(prefix>=4 && prefix<=long(lag/2) && prefixes.insert(std::size_t(prefix)).second && order.size()<8)order.push_back(std::size_t(prefix));};
        for(std::size_t j=0;j<std::min<std::size_t>(2,bins.size());++j){const auto period=std::lround(double(n)/bins[j].second);for(int delta:{0,-1,1})add(period+delta-long(lag));}
        for(auto divisor:{4,8,16,32})add(long(lag/divisor));
        for(auto prefix:order){if((n-lag)/(lag+prefix)<8)continue;++result.timing_hypotheses;trials.push_back(fit_ofdm(train,lag,prefix,proposal.first));}
    }
    std::stable_sort(trials.begin(),trials.end(),[](const auto& a,const auto& b){return a.train_prefix-a.train_outside>b.train_prefix-b.train_outside;});
    if(trials.size()>structure_candidate_limit)trials.resize(structure_candidate_limit);
    for(auto& r:trials)hold_ofdm(hold,origin,r);
    return trials;
}
// Each phase bucket contains non-overlapping complete eleven-chip words.
// Other phase buckets are competing explanations, not independent votes.
struct CodeFold {std::array<double,11> means{};std::array<std::size_t,11> counts{};};
Samples prefix_mixed(const Samples& x,double carrier_normalized,std::size_t origin){
    Samples prefix(x.size()+1);auto oscillator=std::polar(1.0,-2*pi*carrier_normalized*origin),advance=std::polar(1.0,-2*pi*carrier_normalized);
    for(std::size_t j=0;j<x.size();++j){if(j%512==0)oscillator=std::polar(1.0,-2*pi*carrier_normalized*(origin+j));prefix[j+1]=prefix[j]+x[j]*oscillator;oscillator*=advance;}return prefix;
}
CodeFold code_fold(const Samples& prefix,std::size_t chip,std::size_t sample_phase,std::size_t origin){
    CodeFold out;const auto n=prefix.size()-1,first=(sample_phase+chip-origin%chip)%chip;
    std::vector<std::complex<double>> chips;
    for(std::size_t j=first;j+chip<=n;j+=chip)chips.push_back((prefix[j+chip]-prefix[j])/double(chip));
    const auto global_first=(origin+first)/chip;
    for(std::size_t j=0;j+11<=chips.size();++j){std::complex<double> sum{};double power=0;
        for(std::size_t k=0;k<11;++k){sum+=double(code[k])*chips[j+k];power+=std::norm(chips[j+k]);}
        const auto phase=(global_first+j)%11;if(power<=1e-20)continue;
        out.means[phase]+=std::clamp(std::norm(sum)/(11*power),0.0,1.0);++out.counts[phase];}
    for(std::size_t phase=0;phase<11;++phase)if(out.counts[phase])out.means[phase]/=out.counts[phase];
    return out;
}
double other_phase(const CodeFold& fold,std::size_t chosen){double largest=0;for(std::size_t phase=0;phase<11;++phase)if(phase!=chosen)largest=std::max(largest,fold.means[phase]);return largest;}
std::vector<SpreadMeasurement> discover_spread(const Samples& train,const Samples& hold,std::size_t origin,double rate,StructureDiscovery& result){
    // Squaring suppresses BPSK signs; this carrier estimate is modulo Fs/2.
    // Fit both aliases only on discovery and never refine using holdout.
    Moment carrier;for(std::size_t j=0;j+1<train.size();++j){const auto a=train[j+1]*train[j+1],b=train[j]*train[j];carrier.cross+=a*std::conj(b);carrier.a+=std::norm(a);carrier.b+=std::norm(b);}
    result.carrier_coherence_squared=coherence(carrier);result.carrier_estimate_hz=std::arg(carrier.cross)*rate/(4*pi);
    const double base=result.carrier_estimate_hz,alias=base>=0?base-rate/2:base+rate/2;
    std::vector<SpreadMeasurement> trials;
    for(double frequency:{base,alias}){const auto prefix=prefix_mixed(train,frequency/rate,0);
        for(std::size_t chip=1;chip<=16;++chip){++result.spread_hypotheses;SpreadMeasurement best;best.chip_samples=chip;best.carrier_hz=frequency;double score=-2;
            for(std::size_t phase=0;phase<chip;++phase){const auto rows=code_fold(prefix,chip,phase,0);
                for(std::size_t code_phase=0;code_phase<11;++code_phase){if(rows.counts[code_phase]<8)continue;const double contrast=rows.means[code_phase]-other_phase(rows,code_phase);
                    if(contrast>score){score=contrast;best.code_phase_samples=phase+code_phase*chip;best.train_words=rows.counts[code_phase];best.train_code_coherence_squared=rows.means[code_phase];best.train_other_phase=other_phase(rows,code_phase);}}}
            if(best.train_words)trials.push_back(best);}}
    std::stable_sort(trials.begin(),trials.end(),[](const auto& a,const auto& b){return a.train_code_coherence_squared-a.train_other_phase>b.train_code_coherence_squared-b.train_other_phase;});
    if(trials.size()>structure_candidate_limit)trials.resize(structure_candidate_limit);
    for(auto& r:trials){const auto prefix=prefix_mixed(hold,r.carrier_hz/rate,origin);const auto sample_phase=r.code_phase_samples%r.chip_samples,code_phase=r.code_phase_samples/r.chip_samples;
        const auto rows=code_fold(prefix,r.chip_samples,sample_phase,origin);r.holdout_words=rows.counts[code_phase];r.holdout_code_coherence_squared=rows.means[code_phase];r.holdout_other_phase=other_phase(rows,code_phase);
        r.pattern_consistent=r.train_words>=8 && r.holdout_words>=8 && r.train_code_coherence_squared>=.8 && r.holdout_code_coherence_squared>=.8 &&
            r.train_code_coherence_squared-r.train_other_phase>=.5 && r.holdout_code_coherence_squared-r.holdout_other_phase>=.5;}
    return trials;
}
} // namespace
StructureDiscovery discover_waveform_structure(const std::vector<std::complex<float>>& iq,double rate){
    if(!std::isfinite(rate)||rate<=0||rate>1e9||iq.size()>65536)throw std::invalid_argument("invalid structure rate/sample budget");
    double scale=0;for(auto z:iq){if(!std::isfinite(z.real())||!std::isfinite(z.imag()))throw std::invalid_argument("nonfinite structure IQ");scale=std::max({scale,std::abs(double(z.real())),std::abs(double(z.imag()))});}
    StructureDiscovery r;r.samples_examined=iq.size();if(iq.size()<2064)return r;
    std::size_t n=1024;while(n<structure_partition_limit && 4*n+16<=iq.size())n*=2;
    r.partition_samples=n;r.holdout_offset=iq.size()-n;r.max_lag=std::min<std::size_t>(1024,n/10);
    if(!scale){r.status="no_variation";return r;}const auto train=centered(iq,0,n,scale),hold=centered(iq,r.holdout_offset,n,scale);
    if(energy(train)<1e-20||energy(hold)<1e-20){r.status="no_variation";return r;}
    r.status="measured";r.ofdm=discover_ofdm(train,hold,r.holdout_offset,r);r.spread=discover_spread(train,hold,r.holdout_offset,rate,r);return r;
}
double timing_grid_scale(std::size_t index) {
    if(index>=timing_grid_ppm.size())throw std::invalid_argument("invalid timing grid index");
    return 1+timing_grid_ppm[index]*1e-6;
}
ClockRefinement clock_refinement_plan(const StructureDiscovery& raw) {
    if(raw.samples_examined>65536 || raw.ofdm.size()>4 || raw.spread.size()>4 ||
        (raw.status!="measured" && raw.status!="no_variation" && raw.status!="insufficient_samples"))
        throw std::invalid_argument("invalid timing refinement context");
    std::size_t expected=raw.samples_examined<2064?0:1024;
    while(expected && expected<8192 && 4*expected+16<=raw.samples_examined)expected*=2;
    if(raw.partition_samples!=expected || (raw.status=="insufficient_samples")!=(expected==0) ||
        (expected && raw.max_lag!=std::min<std::size_t>(1024,expected/10)))
        throw std::invalid_argument("inconsistent timing partition");
    ClockRefinement r;r.samples_examined=raw.samples_examined;r.partition_samples=raw.partition_samples;
    if(raw.status!="measured"){r.status=raw.status;return r;}
    if(raw.samples_examined<2064 || raw.partition_samples<1024 || raw.partition_samples>8192 ||
        (raw.partition_samples&(raw.partition_samples-1)) || 2*raw.partition_samples+16>raw.samples_examined)
        throw std::invalid_argument("invalid timing partition");
    r.target_samples=std::size_t(std::floor(double(raw.samples_examined-1-2*timing_source_margin)/timing_grid_scale(8)))+1;
    r.holdout_offset=r.target_samples-r.partition_samples;
    if(r.target_samples<2*r.partition_samples+16){r.status="insufficient_resampling_support";return r;}
    if(raw.ofdm.empty() && raw.spread.empty()){r.status="no_proposals";return r;}
    r.status="measured";r.grids_tested=timing_grid_ppm.size();
    r.cp_trials=r.grids_tested*std::min(timing_seed_limit,raw.ofdm.size());
    r.code_trials=r.grids_tested*std::min(timing_seed_limit,raw.spread.size());
    return r;
}
namespace {
Samples retimed(const std::vector<std::complex<float>>& iq,std::size_t origin,std::size_t n,double factor,double scale) {
    Samples x(n);std::complex<double> mean{};
    for(std::size_t j=0;j<n;++j) {
        const double at=timing_source_margin+factor*(origin+j);const auto k=std::size_t(std::floor(at));
        if(k+1>=iq.size())throw std::invalid_argument("timing interpolation escapes source");
        const double part=at-k;x[j]=((1-part)*std::complex<double>(iq[k])+part*std::complex<double>(iq[k+1]))/scale;
        mean+=x[j];
    }
    mean/=double(n);for(auto& z:x)z-=mean;return x;
}
SpreadMeasurement fit_refined_code(const Samples& train,const SpreadMeasurement& seed,double factor) {
    SpreadMeasurement best;best.chip_samples=seed.chip_samples;best.carrier_hz=std::remainder(seed.carrier_hz*factor,1.0);
    const auto prefix=prefix_mixed(train,best.carrier_hz,0); // discovery-seed frequency in canonical cycles/sample
    double score=-2;
    for(std::size_t phase=0;phase<best.chip_samples;++phase) {
        const auto rows=code_fold(prefix,best.chip_samples,phase,0);
        for(std::size_t k=0;k<11;++k)if(rows.counts[k]>=8) {
            const double other=other_phase(rows,k),contrast=rows.means[k]-other;
            if(contrast>score){score=contrast;best.code_phase_samples=phase+k*best.chip_samples;
                best.train_words=rows.counts[k];best.train_code_coherence_squared=rows.means[k];best.train_other_phase=other;}
        }
    }
    return best;
}
}
ClockRefinement refine_structure_clock(const std::vector<std::complex<float>>& iq,double rate,const StructureDiscovery& raw) {
    if(iq.size()!=raw.samples_examined || !std::isfinite(rate) || rate<=0 || rate>1e9)
        throw std::invalid_argument("invalid timing input");
    auto r=clock_refinement_plan(raw);double scale=0;
    for(const auto& z:iq)if(!std::isfinite(z.real()) || !std::isfinite(z.imag()))throw std::invalid_argument("nonfinite timing IQ");
    if(r.status!="measured")return r;
    // Scaling depends only on the union of discovery interpolation support.
    const auto discovery_end=timing_source_margin+std::size_t(std::floor(timing_grid_scale(8)*(r.partition_samples-1)))+2;
    for(std::size_t j=0;j<discovery_end;++j)scale=std::max({scale,std::abs(double(iq[j].real())),std::abs(double(iq[j].imag()))});
    if(!scale)scale=1; // a zero discovery region supplies no nonzero code words
    for(std::size_t j=0;j<std::min(timing_seed_limit,raw.ofdm.size());++j) {
        const auto& s=raw.ofdm[j];
        if(s.useful_samples<16 || s.useful_samples>raw.max_lag || s.prefix_samples<4 || s.prefix_samples>s.useful_samples/2 ||
            (r.partition_samples-s.useful_samples)/(s.useful_samples+s.prefix_samples)<8 || !std::isfinite(s.lag_coherence_squared))
            throw std::invalid_argument("invalid CP timing seed");
    }
    for(std::size_t j=0;j<std::min(timing_seed_limit,raw.spread.size());++j) {
        const auto& s=raw.spread[j];if(!s.chip_samples || s.chip_samples>16 || !std::isfinite(s.carrier_hz) || std::abs(s.carrier_hz)>rate/2)
            throw std::invalid_argument("invalid code timing seed");
    }
    double best_cp=-2,best_code=-2;
    for(std::size_t grid=0;grid<r.grids_tested;++grid) {
        const auto factor=timing_grid_scale(grid);const auto train=retimed(iq,0,r.partition_samples,factor,scale);
        r.interpolated_samples+=r.partition_samples;
        for(std::size_t j=0;j<std::min(timing_seed_limit,raw.ofdm.size());++j) {
            const auto& seed=raw.ofdm[j];auto p=fit_ofdm(train,seed.useful_samples,seed.prefix_samples,seed.lag_coherence_squared);
            const double score=p.train_prefix-p.train_outside;
            if(score>best_cp){best_cp=score;r.cp={{j,grid,p}};}
        }
        for(std::size_t j=0;j<std::min(timing_seed_limit,raw.spread.size());++j) {
            auto normalized=raw.spread[j];normalized.carrier_hz/=rate;
            auto p=fit_refined_code(train,normalized,factor);p.carrier_hz*=rate;
            if(!p.train_words)continue;
            const double score=p.train_code_coherence_squared-p.train_other_phase;
            if(score>best_code){best_code=score;r.code={{j,grid,p}};}
        }
    }
    // Freeze the winning grid, geometry, phase and carrier before reading held
    // samples. No held score participates in candidate/grid/phase selection.
    for(auto& selected:r.cp) {
        const auto held=retimed(iq,r.holdout_offset,r.partition_samples,timing_grid_scale(selected.grid_index),scale);
        r.interpolated_samples+=r.partition_samples;hold_ofdm(held,r.holdout_offset,selected.measurement);
    }
    for(auto& selected:r.code) {
        auto& p=selected.measurement;
        const auto held=retimed(iq,r.holdout_offset,r.partition_samples,timing_grid_scale(selected.grid_index),scale);
        r.interpolated_samples+=r.partition_samples;
        const auto prefix=prefix_mixed(held,p.carrier_hz/rate,r.holdout_offset);
        const auto rows=code_fold(prefix,p.chip_samples,p.code_phase_samples%p.chip_samples,r.holdout_offset);
        const auto phase=p.code_phase_samples/p.chip_samples;p.holdout_words=rows.counts[phase];
        p.holdout_code_coherence_squared=rows.means[phase];p.holdout_other_phase=other_phase(rows,phase);
        p.pattern_consistent=p.train_words>=8 && p.holdout_words>=8 && p.train_code_coherence_squared>=.8 &&
            p.holdout_code_coherence_squared>=.8 && p.train_code_coherence_squared-p.train_other_phase>=.5 &&
            p.holdout_code_coherence_squared-p.holdout_other_phase>=.5;
    }
    return r;
}
void review_clock_refinement(ClockRefinement& r,const SpectralFeatures& quality,const EvidenceContext& context) {
    StructureDiscovery refined;refined.status=r.status=="measured"?"measured":r.status=="no_variation"?"no_variation":"insufficient_samples";
    refined.samples_examined=r.samples_examined;refined.partition_samples=r.partition_samples;refined.holdout_offset=r.holdout_offset;
    for(const auto& p:r.cp)refined.ofdm.push_back(p.measurement);
    for(const auto& p:r.code)refined.spread.push_back(p.measurement);
    r.review=review_waveform_measurements(quality,refined,{},context);
}
} // namespace rfmon::cyclo
