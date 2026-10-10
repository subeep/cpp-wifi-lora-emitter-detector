#include "marker_structure.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <stdexcept>
namespace rfmon::cyclo {
namespace {
constexpr double pi=3.14159265358979323846;
using Complex=std::complex<double>;
std::size_t nearest(double x){return std::size_t(std::llround(x));}
bool admitted(const ChirpMeasurement& c) {
    const bool profile=c.slope_hz_per_second==135.2e9 || c.slope_hz_per_second==270.2e9 || c.slope_hz_per_second==-135.3e9;
    return profile && c.status=="measured" && c.frequency_status=="measured" && !c.phase_unwraps &&
        c.peak_coherence_squared>=.8 && c.first_half_coherence_squared>=.8 && c.second_half_coherence_squared>=.8 &&
        std::isfinite(c.frequency_mean_hz) && std::isfinite(c.frequency_slope_hz_per_second) &&
        std::abs(c.frequency_slope_hz_per_second/c.slope_hz_per_second-1)<=.02;
}
std::pair<double,double> compare(const std::vector<Complex>& x,std::size_t a,std::size_t b,std::size_t n) {
    Complex cross{};double pa=0,pb=0;
    for(std::size_t j=0;j<n;++j){cross+=std::conj(x[a+j])*x[b+j];pa+=std::norm(x[a+j]);pb+=std::norm(x[b+j]);}
    if(std::min(pa,pb)<=1e-18)return {0,0};
    return {std::min(1.,std::norm(cross)/(pa*pb)),std::min(pa,pb)/std::max(pa,pb)};
}
struct Span {double minimum=0,energy=0;bool contrast=false;};
Span span_check(const std::vector<Complex>& x,const std::vector<Complex>& z,double rate,std::size_t start) {
    Span r;const auto span=nearest(rate/15000),end=start+span,before=nearest(rate*8e-6),after=nearest(rate*4e-6);
    if(start<before || end+after>x.size())return r;
    auto measure=[&](std::size_t a,std::size_t b){Complex sum{};double power=0;
        for(auto i=a;i<b;++i){sum+=z[i];power+=std::norm(x[i]);}
        return std::pair<double,double>{power>1e-20?std::min(1.,std::norm(sum)/(double(b-a)*power)):0,power/double(b-a)};};
    std::array<double,34> energy{};r.minimum=1;
    for(std::size_t i=0;i<34;++i){
        const auto a=std::size_t(std::nearbyint(double(start)+double(span)*i/34));
        const auto b=std::size_t(std::nearbyint(double(start)+double(span)*(i+1)/34));
        auto m=measure(a,b);r.minimum=std::min(r.minimum,m.first);energy[i]=m.second;
    }
    std::sort(energy.begin(),energy.end());const double median=(energy[16]+energy[17])/2;
    if(median<=1e-20)return {};
    r.energy=energy[0]/median;
    auto pre=measure(start-before,start-nearest(rate*6e-6));
    auto post=measure(end+nearest(rate*2e-6),end+after);
    r.contrast=(pre.first<=.5 || pre.second<=.1*median) && (post.first<=.5 || post.second<=.1*median);
    return r;
}
}
bool valid_marker_structure(const MarkerStructure& r,double rate,std::size_t samples,const ChirpMeasurement& c) {
    for(double v:{r.coherence,r.first_half,r.second_half,r.balance,r.guard_max,r.span_min,r.span_energy})
        if(!std::isfinite(v) || v<0 || v>1)return false;
    if(!r.evaluated)return !r.prefix_compatible && !r.joint_compatible && !r.boundary_contrast &&
        !r.start && !r.lag && !r.gate && !r.trials && r.coherence==0 && r.first_half==0 && r.second_half==0 &&
        r.balance==0 && r.guard_max==0 && r.span_min==0 && r.span_energy==0;
    if(!std::isfinite(rate) || rate<1e6 || rate>1e9 || samples>65536 || r.start>samples || r.lag>65536 || c.peak_offset>=samples || !admitted(c))return false;
    const auto gate=nearest(rate*4e-6),lag=nearest(rate/15000),guard=nearest(rate*1e-6);
    if(rate<=c.nominal_sweep_hz || r.gate!=gate || r.lag+1<lag || r.lag>lag+1 || !r.trials || r.trials>99 ||
        r.start<gate || r.start+r.lag+guard>samples)return false;
    const auto margin=nearest(rate*.5e-6),legacy=2*nearest(rate*32e-6);
    const auto low=static_cast<long long>(c.peak_offset)-static_cast<long long>(lag-legacy+margin);
    if(static_cast<long long>(r.start)<low || r.start>c.peak_offset+margin)return false;
    if(r.boundary_contrast && (r.start<nearest(rate*8e-6) || r.start+lag+nearest(rate*4e-6)>samples))return false;
    const bool prefix=r.coherence>=.8 && r.first_half>=.8 && r.second_half>=.8 && r.balance>=.5 && r.guard_max<=.5;
    return r.prefix_compatible==prefix && r.joint_compatible==(prefix && r.span_min>=.8 && r.span_energy>=.1 && r.boundary_contrast);
}
std::array<MarkerStructure,3> measure_marker_structures(const std::vector<std::complex<float>>& iq,double rate,
    const std::vector<ChirpMeasurement>& chirps) {
    if(!std::isfinite(rate) || rate<1e6 || rate>1e9 || iq.size()>65536 || chirps.size()>3)
        throw std::invalid_argument("marker structure input bounds");
    std::array<MarkerStructure,3> out{};double scale=0;
    for(auto v:iq){if(!std::isfinite(v.real()) || !std::isfinite(v.imag()))throw std::invalid_argument("nonfinite marker IQ");
        scale=std::max({scale,std::abs(double(v.real())),std::abs(double(v.imag()))});}
    if(!scale || iq.empty())return out;
    std::vector<Complex> x(iq.size());Complex mean{};
    for(std::size_t i=0;i<x.size();++i){x[i]=Complex(iq[i])/scale;mean+=x[i];}mean/=double(x.size());for(auto& v:x)v-=mean;
    const auto nominal=nearest(rate/15000),gate=nearest(rate*4e-6),guard=nearest(rate*1e-6),margin=nearest(rate*.5e-6);
    const auto legacy=2*nearest(rate*32e-6);
    for(std::size_t index=0;index<chirps.size();++index) {
        const auto& c=chirps[index];if(!admitted(c) || rate<=c.nominal_sweep_hz || c.peak_offset>=iq.size())continue;
        std::vector<Complex> z(x.size());
        for(std::size_t i=0;i<x.size();++i){const double t=(double(i)-c.peak_offset-double(legacy-1)/2)/rate;
            z[i]=x[i]*std::polar(1.,-2*pi*(c.frequency_mean_hz*t+c.slope_hz_per_second*t*t/2));}
        std::set<long long> starts;const double low=-double(nominal-legacy+margin),high=double(margin);
        for(int i=0;i<33;++i)starts.insert(static_cast<long long>(c.peak_offset)+static_cast<long long>(std::nearbyint(low+(high-low)*i/32)));
        MarkerStructure best;std::size_t trials=0;
        auto rank=[](const MarkerStructure& r){return std::make_tuple(r.joint_compatible,r.prefix_compatible,
            std::min({r.coherence,r.first_half,r.second_half}),r.balance,-r.guard_max);};
        for(auto signed_start:starts){if(signed_start<static_cast<long long>(gate))continue;
            const auto start=std::size_t(signed_start),begin=start-gate;Span span;bool span_computed=false;
            for(auto lag:{nominal-1,nominal,nominal+1}){
                const auto tail=begin+lag;if(tail<guard || tail+gate+guard>x.size())continue;++trials;
                MarkerStructure r;r.evaluated=true;r.start=start;r.lag=lag;r.gate=gate;
                auto whole=compare(x,begin,tail,gate);r.coherence=whole.first;r.balance=whole.second;
                r.first_half=compare(x,begin,tail,gate/2).first;r.second_half=compare(x,begin+gate/2,tail+gate/2,gate-gate/2).first;
                r.guard_max=std::max(compare(x,begin,tail-guard,gate).first,compare(x,begin,tail+guard,gate).first);
                r.prefix_compatible=r.coherence>=.8 && r.first_half>=.8 && r.second_half>=.8 && r.balance>=.5 && r.guard_max<=.5;
                if(r.prefix_compatible){if(!span_computed){span=span_check(x,z,rate,start);span_computed=true;}
                    r.span_min=span.minimum;r.span_energy=span.energy;r.boundary_contrast=span.contrast;
                    r.joint_compatible=r.span_min>=.8 && r.span_energy>=.1 && r.boundary_contrast;}
                if(!best.evaluated || rank(r)>rank(best))best=r;
            }
        }
        if(best.evaluated){best.trials=trials;if(!valid_marker_structure(best,rate,iq.size(),c))throw std::runtime_error("invalid marker structure result");}
        out[index]=best;
    }
    return out;
}
}
