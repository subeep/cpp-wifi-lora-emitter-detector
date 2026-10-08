#include "chirp_discovery.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
constexpr std::size_t phase_average=8;
constexpr double pi=3.14159265358979323846;
struct Partition {
    std::vector<std::complex<double>> x;
    std::vector<double> frequency, averaged;
    std::vector<bool> usable;
    double energy=0;
};
Partition prepare(const std::vector<std::complex<float>>& iq,std::size_t start,std::size_t n) {
    Partition p;double scale=0;
    for(std::size_t i=start;i<start+n;++i)scale=std::max(scale,std::abs(std::complex<double>(iq[i])));
    p.x.resize(n);p.frequency.resize(n-1);p.usable.resize(n-1);
    if(!scale)return p;
    for(std::size_t i=0;i<n;++i){p.x[i]=std::complex<double>(iq[start+i])/scale;p.energy+=std::norm(p.x[i]);}
    const double floor=p.energy/n*1e-4;
    for(std::size_t i=0;i+1<n;++i) {
        p.frequency[i]=std::arg(p.x[i+1]*std::conj(p.x[i]))/(2*pi);
        p.usable[i]=std::norm(p.x[i])>floor && std::norm(p.x[i+1])>floor && std::abs(p.frequency[i])<=.45;
    }
    p.averaged.resize(n-phase_average);
    double sum=0;for(std::size_t i=0;i<phase_average;++i)sum+=p.frequency[i];
    for(std::size_t i=0;i<n-phase_average;++i) {
        if(i)sum+=p.frequency[i+phase_average-1]-p.frequency[i-1];
        p.averaged[i]=sum/phase_average;
    }
    return p;
}
struct Fit {bool supported=false;double slope=0,center=0,rmse=0,coherence=0,cv=0;};
Fit fit(const Partition& p,std::size_t offset,std::size_t span,bool fixed,double slope=0) {
    Fit f;const std::size_t pairs=span-phase_average;const double mid=(span-1)*.5;
    double sum=0,xy=0,xx=0;
    for(std::size_t i=0;i+1<span;++i)if(!p.usable[offset+i])return f;
    for(std::size_t i=0;i<pairs;++i) {
        const double t=i+phase_average*.5-mid, v=p.averaged[offset+i];sum+=v;xy+=t*v;xx+=t*t;
    }
    f.center=sum/pairs;f.slope=fixed?slope:xy/xx;
    double error=0,power=0,amp=0;std::complex<double> cross{};
    for(std::size_t i=0;i<pairs;++i) {
        const double residual=p.averaged[offset+i]-f.center-f.slope*(i+phase_average*.5-mid);error+=residual*residual;
    }
    f.rmse=std::sqrt(error/pairs);
    for(std::size_t i=0;i<span;++i) {
        const double t=i-mid;const auto z=p.x[offset+i];
        cross+=z*std::polar(1.0,-2*pi*(f.center*t+.5*f.slope*t*t));power+=std::norm(z);amp+=std::abs(z);
    }
    if(power<=0 || amp<=0)return f;
    f.coherence=std::clamp(std::norm(cross)/(span*power),0.0,1.0);
    f.cv=std::sqrt(std::max(0.0,power/span-(amp/span)*(amp/span)))/(amp/span);
    f.supported=true;return f;
}
bool consistent(const Fit& f) {return f.supported && f.rmse<=.003 && f.coherence>=.65 && f.cv<=.5;}
double score(const Fit& f) {return f.supported ? std::min(f.coherence,std::max(0.0,1-f.rmse/.003)) : -1;}
}
std::size_t sweep_window_count(std::size_t partition,std::size_t span) {
    return span>=64 && span<=2048 && !(span&(span-1)) && span<=partition ? 1+(partition-span)/(span/2) : 0;
}
ChirpDiscovery discover_linear_sweeps(const std::vector<std::complex<float>>& iq,double rate) {
    if(iq.size()>65536 || !std::isfinite(rate) || rate<=0 || rate>1e9)throw std::invalid_argument("linear sweep input exceeds bounds");
    for(const auto z:iq)if(!std::isfinite(z.real()) || !std::isfinite(z.imag()))throw std::invalid_argument("non-finite linear sweep samples");
    ChirpDiscovery r;r.samples_examined=iq.size();if(iq.size()<1040)return r;
    std::size_t n=512;while(n<sweep_partition_limit && 4*n+16<=iq.size())n*=2;
    r.partition_samples=n;r.holdout_partition_offset=iq.size()-n;
    const auto train=prepare(iq,0,n),held=prepare(iq,r.holdout_partition_offset,n);
    if(!train.energy || !held.energy){r.status="no_variation";return r;}
    r.status="measured";
    struct Proposal {LinearSweep measurement;Fit fit;};std::vector<Proposal> proposals;
    for(std::size_t span=64;span<=2048 && span<=n;span*=2) {
        const auto count=sweep_window_count(n,span);r.discovery_trials+=count;
        for(std::size_t j=0;j<count;++j) {
            const auto offset=j*(span/2);const auto f=fit(train,offset,span,false);
            const double sweep=std::abs(f.slope)*(span-1);
            if(!consistent(f) || sweep<.04 || sweep>.7)continue;
            // Require slope support in a neighbouring half-overlap window.
            // This favours partial spans with room for timing misalignment;
            // an entire sawtooth chirp ending at a reset is not a robust gate.
            bool continued=false;
            if(offset+span+span/2<=n) {
                ++r.discovery_continuation_trials;
                continued=consistent(fit(train,offset+span/2,span,true,f.slope));
            }
            if(!continued && offset>=span/2) {
                ++r.discovery_continuation_trials;
                continued=consistent(fit(train,offset-span/2,span,true,f.slope));
            }
            if(!continued)continue;
            LinearSweep m;m.span_samples=span;m.discovery_offset=offset;m.slope_hz_per_second=f.slope*rate*rate;
            m.sweep_hz=sweep*rate;m.discovery_center_hz=f.center*rate;m.discovery_rmse_fraction=f.rmse;
            m.discovery_coherence_squared=f.coherence;m.discovery_amplitude_cv=f.cv;proposals.push_back({m,f});
        }
    }
    // Prefer the largest supported observed sweep, then coherence. These spans
    // are measurement windows, not estimates of a complete chirp/symbol length.
    std::stable_sort(proposals.begin(),proposals.end(),[](const auto& a,const auto& b){
        if(a.measurement.sweep_hz!=b.measurement.sweep_hz)return a.measurement.sweep_hz>b.measurement.sweep_hz;
        return a.fit.coherence>b.fit.coherence;
    });
    for(const auto& proposal:proposals) {
        bool duplicate=false;
        for(const auto& old:r.candidates)if(old.slope_hz_per_second*proposal.measurement.slope_hz_per_second>0 &&
            std::abs(old.slope_hz_per_second-proposal.measurement.slope_hz_per_second)<=.05*std::abs(old.slope_hz_per_second))duplicate=true;
        if(duplicate)continue;
        auto m=proposal.measurement;const auto count=sweep_window_count(n,m.span_samples);m.holdout_trials=count;
        Fit best;double best_score=-1;
        for(std::size_t j=0;j<count;++j) {
            const auto offset=j*(m.span_samples/2);const auto f=fit(held,offset,m.span_samples,true,proposal.fit.slope);
            if(f.supported && score(f)>best_score){best=f;best_score=score(f);m.holdout_offset=r.holdout_partition_offset+offset;}
        }
        m.holdout_supported=best.supported;
        if(best.supported) {
            m.holdout_center_hz=best.center*rate;m.holdout_rmse_fraction=best.rmse;
            m.holdout_coherence_squared=best.coherence;m.holdout_amplitude_cv=best.cv;
        }
        m.pattern_consistent=consistent(best);r.candidates.push_back(m);
        if(r.candidates.size()==sweep_candidate_limit)break;
    }
    return r;
}
} // namespace rfmon::cyclo
