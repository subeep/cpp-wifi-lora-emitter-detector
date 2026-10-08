#include "cyclostationary/chirp_discovery.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <limits>
using namespace rfmon::cyclo;
namespace {
constexpr double pi=3.14159265358979323846;
void check(bool yes,const char* why){if(!yes){std::cerr<<why<<'\n';std::exit(1);}}
std::vector<std::complex<float>> chirps(double slope,std::size_t span=512,std::size_t count=32768,double carrier=.025) {
    std::vector<std::complex<float>> x(count);
    for(std::size_t i=0;i<count;++i){double t=double(i%span)-(span-1)*.5;x[i]=std::polar(1.f,float(2*pi*(carrier*t+.5*slope*t*t)));}
    return x;
}
bool match(const ChirpDiscovery& r){for(const auto& p:r.candidates)if(p.pattern_consistent)return true;return false;}
}
int main() {
    for(double rate:{2e6,20e6,60e6})for(double direction:{-1.,1.})for(std::size_t span:{128u,512u,2048u}) {
        const double slope=direction*.5/(span-1);const auto x=chirps(slope,span);const auto r=discover_linear_sweeps(x,rate);
        check(r.status=="measured" && match(r),"clean sweep not recovered");
        const auto& p=r.candidates.front();check(std::abs(p.slope_hz_per_second/(slope*rate*rate)-1)<1e-5,"slope differs from phase oracle");
        check(p.holdout_coherence_squared>.999 && p.holdout_rmse_fraction<1e-5,"held dechirp differs from clean oracle");
        check(p.span_samples<=span && p.discovery_offset+p.span_samples<=r.partition_samples,"span exceeds support");
        check(p.holdout_offset>=r.holdout_partition_offset && p.holdout_offset+p.span_samples<=x.size(),"held span escapes partition");
        check(r.discovery_trials<=510 && r.candidates.size()<=3,"search exceeds bounds");
    }
    const auto x=chirps(.5/511);auto changed=x;
    // Holdout may move its carrier centre but cannot change the selected slope.
    for(std::size_t i=changed.size()-8192;i<changed.size();++i)changed[i]*=std::polar(1.f,float(2*pi*.08*double(i%512)));
    check(match(discover_linear_sweeps(changed,20e6)),"holdout nuisance carrier shift rejected");
    changed=x;std::fill(changed.end()-8192,changed.end(),std::complex<float>{1,0});
    check(!match(discover_linear_sweeps(changed,20e6)),"discovery-only sweep accepted");
    changed=x;const auto reverse=chirps(-.5/511);std::copy(reverse.end()-8192,reverse.end(),changed.end()-8192);
    check(!match(discover_linear_sweeps(changed,20e6)),"held slope refitted to opposite direction");
    changed=x;for(auto& z:changed)z*=1e-25f;
    check(match(discover_linear_sweeps(changed,20e6)),"tiny gain changes pattern");
    changed=x;for(auto& z:changed)z*=1e25f;
    check(match(discover_linear_sweeps(changed,20e6)),"large gain changes pattern");
    std::mt19937 rng(20261005);std::normal_distribution<float> normal;
    changed=x;for(auto& z:changed)z+=std::complex<float>{normal(rng),normal(rng)}*.02236068f;
    const auto noisy=discover_linear_sweeps(changed,20e6);
    check(match(noisy),"short phase average did not recover noisy linear sweep");
    const auto& proposal=noisy.candidates.front();const auto begin=proposal.discovery_offset,span=proposal.span_samples;
    double sum=0,xy=0,xx=0;std::vector<double> averaged;
    for(std::size_t i=0;i+8<span;++i) {
        double f=0;for(std::size_t j=0;j<8;++j)f+=std::arg(std::complex<double>(changed[begin+i+j+1])*std::conj(std::complex<double>(changed[begin+i+j])))/(2*pi);
        f/=8;averaged.push_back(f);const double t=i+4.-(span-1)*.5;sum+=f;xy+=t*f;xx+=t*t;
    }
    const double slope_oracle=xy/xx,center_oracle=sum/averaged.size();double squared_error=0;
    for(std::size_t i=0;i<averaged.size();++i){const double error=averaged[i]-center_oracle-slope_oracle*(i+4.-(span-1)*.5);squared_error+=error*error;}
    check(std::abs(proposal.slope_hz_per_second/(20e6*20e6)-slope_oracle)<1e-12,"smoothed frequency slope differs from direct oracle");
    check(std::abs(proposal.discovery_rmse_fraction-std::sqrt(squared_error/averaged.size()))<1e-12,"smoothed residual differs from direct oracle");
    for(int seed=0;seed<12;++seed){changed=x;for(auto& z:changed)z={normal(rng),normal(rng)};
        check(!match(discover_linear_sweeps(changed,20e6)),"white noise accepted as sweep");}
    changed=x;for(std::size_t i=0;i<changed.size();++i)changed[i]=std::polar(1.f,float(2*pi*.1*(i%10)));
    check(discover_linear_sweeps(changed,20e6).candidates.empty(),"tone accepted as sweep");
    check(discover_linear_sweeps(std::vector<std::complex<float>>(32768),20e6).status=="no_variation","zero signal not abstained");
    check(discover_linear_sweeps(std::vector<std::complex<float>>(1039),20e6).status=="insufficient_samples","short signal not abstained");
    for(int mode=0;mode<3;++mode){bool rejected=false;try{changed=x;if(mode==0)changed.resize(65537);if(mode==1)changed[0]={NAN,0};
        discover_linear_sweeps(changed,mode==2?0:20e6);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"bad input accepted");}
    std::cout<<"bounded linear sweep discovery passed\n";
}
