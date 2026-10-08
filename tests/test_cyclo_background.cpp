#include "cyclostationary/cyclic_background.hpp"
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
}
int main(){try {
    std::mt19937 rng(20261006);std::normal_distribution<float> normal;
    std::vector<std::complex<float>> x(32768);
    for(std::size_t i=0;i<x.size();++i)x[i]=std::sqrt(1.f+.9f*float(std::cos(2*pi*i/128)))*std::complex<float>{normal(rng),normal(rng)};
    const auto w=measure_waveform_features(x,20e6);const auto b=measure_cyclic_background(x,20e6,w);
    check(supported(b)>0,"periodic envelope line failed local background check");
    check(b.peaks.size()==w.peaks.size() && b.fft_calls<=16,"unbounded cyclic background result");
    // Coherent conjugate line with random BPSK signs has a known cyclic rate.
    for(std::size_t i=0;i<x.size();++i)x[i]=(rng()%2?1.f:-1.f)*std::polar(1.f,float(pi*(i%256)/128));
    const auto cw=measure_waveform_features(x,20e6), cw_copy=cw;const auto cb=measure_cyclic_background(x,20e6,cw);
    bool found=false;
    for(std::size_t i=0;i<cw.peaks.size();++i)if(cw.peaks[i].conjugate && std::abs(cw.peaks[i].alpha_hz-20e6/128)<1) {
        found=true;check(cb.peaks[i].background_supported && cb.peaks[i].holdout.block_phase_coherence_squared>.99,"conjugate line not phase consistent");
        const auto& p=cb.peaks[i];const auto n=cw.partition_samples;
        const auto refs=cyclic_reference_bins(n,int(std::round(cw.peaks[i].coarse_alpha_hz/(20e6/n))));
        // Direct double DFT oracle, using the same centred partition but no FFT.
        std::vector<std::complex<double>> y(n);std::complex<double> mean{};
        for(std::size_t j=0;j<n;++j){y[j]=std::complex<double>(x[j]);mean+=y[j];}mean/=n;for(auto& z:y)z-=mean;
        std::vector<double> powers;const auto lag=cw.peaks[i].lag_samples, length=n-lag;double a=0,d=0;
        for(std::size_t j=0;j<length;++j){double weight=.5-.5*std::cos(2*pi*j/length);a+=weight*std::norm(y[j+lag]);d+=weight*std::norm(y[j]);}
        for(auto k:refs){std::complex<double> sum{};for(std::size_t j=0;j<length;++j)sum+=(.5-.5*std::cos(2*pi*j/length))*y[j+lag]*y[j]*std::polar(1.,-2*pi*k*j/n);powers.push_back(std::norm(sum)/(a*d));}
        std::sort(powers.begin(),powers.end());check(p.discovery.reference_bins==powers.size(),"reference support differs from DFT oracle");
        check(std::abs(p.discovery.median_coherence_squared-powers[(powers.size()-1)/2])<1e-7 &&
            std::abs(p.discovery.upper_coherence_squared-powers[std::size_t(.9*(powers.size()-1))])<1e-7,"local background differs from direct DFT oracle");
    }
    check(found && cw.peaks[0].alpha_hz==cw_copy.peaks[0].alpha_hz,"selected cyclic rate mutated or missing");
    auto maximum=cw;maximum.peaks.clear();
    for(bool conjugate:{false,true})for(std::size_t j=0;j<4;++j) {
        const double alpha=(32+16*j)*(20e6/maximum.partition_samples);
        maximum.peaks.push_back({conjugate,cyclic_lags[j],alpha,alpha,.1,.1,true});
    }
    const auto all=measure_cyclic_background(x,20e6,maximum);
    check(all.fft_calls==16 && all.peaks.size()==8,"full distinct-lag cache budget not exercised");
    auto gated=x;
    for(std::size_t i=0;i<gated.size();++i)if(i%8192>=1024)gated[i]={0,0};
    const auto gw=measure_waveform_features(gated,20e6);const auto gb=measure_cyclic_background(gated,20e6,gw);bool checked_gate=false;
    for(std::size_t i=0;i<gw.peaks.size();++i)if(gw.peaks[i].conjugate && std::abs(gw.peaks[i].alpha_hz-20e6/128)<1) {
        checked_gate=true;check(!gb.peaks[i].background_supported && gb.peaks[i].holdout.min_block_energy_fraction<.02,"one-partition-portion burst passed block support gate");
    }
    check(checked_gate,"gated conjugate raw candidate missing");
    for(auto& z:x)z*=1e-25f;const auto tiny=measure_waveform_features(x,20e6);check(supported(measure_cyclic_background(x,20e6,tiny))>0,"small gain breaks cyclic support");
    for(auto& z:x)z*=1e25f;
    // Raw CAF false matches in strongly correlated Gaussian noise remain raw;
    // this separate diagnostic must not assign an emitter or replace them.
    for(int seed=0;seed<12;++seed){std::complex<float> previous{};for(auto& z:x){previous=.975f*previous+std::complex<float>{normal(rng),normal(rng)};z=previous;}
        const auto nw=measure_waveform_features(x,20e6);const auto nb=measure_cyclic_background(x,20e6,nw);
        check(supported(nb)==0,"coloured-noise cyclic line passed development background controls");}
    for(auto& z:x)z={0,0};const auto zw=measure_waveform_features(x,20e6);check(measure_cyclic_background(x,20e6,zw).status=="no_variation","zero input not abstained");
    x.resize(16);const auto sw=measure_waveform_features(x,20e6);check(measure_cyclic_background(x,20e6,sw).status=="insufficient_samples","short input not abstained");
    auto reject=[&](auto f){bool failed=false;try{f();}catch(const std::invalid_argument&){failed=true;}check(failed,"invalid background input accepted");};
    auto huge=cw;huge.peaks[0].coarse_alpha_hz=1e300;
    std::vector<std::complex<float>> original(32768,{1,1});
    reject([&]{measure_cyclic_background(original,20e6,huge);});
    auto bad=sw;bad.partition_samples=1024;reject([&]{measure_cyclic_background(x,20e6,bad);});
    x[0]={NAN,0};reject([&]{measure_cyclic_background(x,20e6,sw);});
    check(cyclic_reference_bins(1024,9).size()>=16 && cyclic_reference_bins(1024,-9).size()>=16,"edge cyclic rates lack declared support");
    reject([&]{cyclic_reference_bins(1024,2);});
    std::cout<<"cyclic local background oracles/support controls passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
