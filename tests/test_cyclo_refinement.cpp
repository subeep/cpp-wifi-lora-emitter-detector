#include "cyclostationary/chirp_refinement.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace rfmon::cyclo;
namespace {
constexpr double pi=3.14159265358979323846,rate=20e6;
void check(bool b,const char* message) {if (!b) throw std::runtime_error(message);}
template<class F> void rejects(F f) {try {f();} catch (const std::exception&) {return;} throw std::runtime_error("invalid refinement input accepted");}
std::vector<std::complex<float>> waveform(double slope,double sweep,double noise_amplitude,double clock=1) {
    std::mt19937 rng(519037);std::normal_distribution<float> normal;
    std::vector<std::complex<float>> x(8192);
    for (auto& z:x) z={float(noise_amplitude*normal(rng)),float(noise_amplitude*normal(rng))};
    for (std::size_t i=0;i<1334;++i) {
        const double t=i/rate*clock;
        x[2000+i]+=std::complex<float>(std::polar(1.0,2*pi*(-std::copysign(sweep/2,slope)*t+slope*t*t/2)));
    }
    return x;
}
// Direct DFT oracle over the selected Hann/dechirped span, independent of
// kissfft and the circular sliding-band sum used by the implementation.
double direct_band(const std::vector<std::complex<float>>& iq,const ChirpRefinement& r) {
    std::complex<double> mean{};for(auto z:iq) mean+=std::complex<double>(z);mean/=double(iq.size());
    std::size_t n=32;while(n<r.span_samples)n*=2;
    const double bin=rate/n;const auto width=std::size_t(std::llround(r.dechirped_band_width_hz/bin));
    double total=0,selected=0;
    for(std::size_t k=0;k<n;++k) {
        std::complex<double> z{};
        for(std::size_t j=0;j<r.span_samples;++j) {
            const double t=j/rate;
            z+=(std::complex<double>(iq[r.peak_offset+j])-mean)*((1-std::cos(2*pi*j/r.span_samples))/2)*
                std::polar(1.0,-pi*r.selected_slope_hz_per_second*t*t-2*pi*k*j/n);
        }
        const double power=std::norm(z);total+=power;
        const double signed_distance=std::remainder(k*bin-r.dechirped_band_center_hz,rate);
        if(std::abs(signed_distance)<=(width-1)*bin/2+1) selected+=power;
    }
    return selected/total;
}
}
int main() {
    try {
        auto x=waveform(135.2e9,9e6,.001);auto original=x;
        auto r=refine_chirp_candidates(x,rate,measure_chirp_structure(x,rate));
        check(x==original,"refinement changed source IQ");
        check(r[0].shape_consistent && r[0].dechirped_band_power_fraction>.99,"clean analytic chirp not recovered");
        check(std::abs(direct_band(x,r[0])-r[0].dechirped_band_power_fraction)<1e-6,"dechirp FFT disagrees with independent DFT");
        for(std::size_t which=0;which<3;++which) {
            const auto bank=measure_chirp_structure({},rate);const auto& h=bank[which];
            auto noisy=waveform(h.slope_hz_per_second,h.nominal_sweep_hz,std::sqrt(.1/2));
            auto measured=refine_chirp_candidates(noisy,rate,measure_chirp_structure(noisy,rate));
            check(measured[which].shape_consistent,"analytic 10 dB noise chirp not recovered");
            for(int delay:{3,20}) {
                auto clean=waveform(h.slope_hz_per_second,h.nominal_sweep_hz,.001);auto echo=clean;
                for(std::size_t j=delay;j<echo.size();++j) echo[j]+=.5f*std::complex<float>(std::polar(1.0,.7))*clean[j-delay];
                auto result=refine_chirp_candidates(echo,rate,measure_chirp_structure(echo,rate));
                check(result[which].shape_consistent,"analytic two-path chirp not recovered");
            }
            for(double clock:{.9995,1.0005}) {
                auto shifted=waveform(h.slope_hz_per_second,h.nominal_sweep_hz,.001,clock);
                check(refine_chirp_candidates(shifted,rate,measure_chirp_structure(shifted,rate))[which].shape_consistent,
                      "analytic clock-shifted chirp not recovered");
            }
        }
        for(float scale:{1e-25f,1e25f}) {
            auto scaled=x;for(auto& z:scaled)z*=scale;
            const auto result=refine_chirp_candidates(scaled,rate,measure_chirp_structure(scaled,rate));
            check(std::abs(result[0].dechirped_band_power_fraction-r[0].dechirped_band_power_fraction)<1e-6,"amplitude invariance failed");
        }
        std::mt19937 rng(151937);std::normal_distribution<float> normal;
        for(auto& z:x)z={normal(rng),normal(rng)};
        for(const auto& h:refine_chirp_candidates(x,rate,measure_chirp_structure(x,rate)))check(!h.shape_consistent,"noise became a chirp shape");
        for(std::size_t j=0;j<x.size();++j)x[j]=std::complex<float>(std::polar(1.0,2*pi*2e6*j/rate));
        for(const auto& h:refine_chirp_candidates(x,rate,measure_chirp_structure(x,rate)))check(!h.shape_consistent,"tone became a chirp shape");
        check(r[0].spectra_examined<=75 && r[0].positions_examined==5*(8192-1280+1),"refinement resource counts incorrect");
        auto short_iq=std::vector<std::complex<float>>(16);
        check(refine_chirp_candidates(short_iq,rate,measure_chirp_structure(short_iq,rate))[0].status=="insufficient_samples","short input did not abstain");
        auto bank=measure_chirp_structure({},rate);
        rejects([&]{refine_chirp_candidates(std::vector<std::complex<float>>(65537),rate,bank);});
        rejects([&]{refine_chirp_candidates({},NAN,bank);});
        x[1]={INFINITY,0};rejects([&]{refine_chirp_candidates(x,rate,bank);});
        bank[0].label="invented";rejects([&]{refine_chirp_candidates({},rate,bank);});
        std::cout<<"refinement DFT oracle, noise/echo/clock, nulls, scale and bounds passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
