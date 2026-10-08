#include "cyclic_background.hpp"
#include <kissfft/kiss_fft.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
constexpr double pi=3.14159265358979323846;
struct Free {void operator()(kiss_fft_state* p)const{kiss_fft_free(p);}};
using Samples=std::vector<std::complex<double>>;
Samples centered(const std::vector<std::complex<float>>& iq,std::size_t at,std::size_t n,double scale) {
    Samples x(n);std::complex<double> mean{};
    for(std::size_t i=0;i<n;++i){x[i]=std::complex<double>(iq[at+i])/scale;mean+=x[i];}
    mean/=n;for(auto& z:x)z-=mean;return x;
}
struct Spectrum {
    std::vector<double> coherence;
};
Spectrum spectrum(const Samples& x,std::size_t lag,bool conjugate,kiss_fft_state* fft) {
    const auto n=x.size(),length=n-lag;std::vector<kiss_fft_cpx> input(n),output(n);double a=0,b=0;
    for(std::size_t j=0;j<length;++j) {
        const double w=.5-.5*std::cos(2*pi*j/length);
        const auto v=w*x[j+lag]*(conjugate?x[j]:std::conj(x[j]));input[j]={float(v.real()),float(v.imag())};
        a+=w*std::norm(x[j+lag]);b+=w*std::norm(x[j]);
    }
    kiss_fft(fft,input.data(),output.data());Spectrum s;s.coherence.resize(n);
    if(a>0 && b>0)for(std::size_t i=0;i<n;++i)s.coherence[i]=std::clamp(
        (double(output[i].r)*output[i].r+double(output[i].i)*output[i].i)/(a*b),0.0,1.0);
    return s;
}
BackgroundPartition examine(const Samples& x,const Spectrum& s,const RatePeak& raw,
    const std::vector<std::size_t>& refs,double normalized_alpha,double line) {
    BackgroundPartition r;r.reference_bins=refs.size();std::vector<double> scores;
    for(auto k:refs)scores.push_back(s.coherence[k]);
    if(!scores.empty()) {
        std::sort(scores.begin(),scores.end());r.median_coherence_squared=scores[(scores.size()-1)/2];
        r.upper_coherence_squared=scores[std::size_t(.9*(scores.size()-1))];
        r.line_to_median=line/std::max(r.median_coherence_squared,1e-12);
        r.line_to_upper=line/std::max(r.upper_coherence_squared,1e-12);
    }
    std::array<std::complex<double>,4> cross{};std::array<double,4> energy{};
    const auto length=x.size()-raw.lag_samples;const auto advance=std::polar(1.0,-2*pi*normalized_alpha);
    std::complex<double> oscillator{1,0};
    for(std::size_t j=0;j<length;++j) {
        if(j%512==0)oscillator=std::polar(1.0,-2*pi*normalized_alpha*j);
        const double w=.5-.5*std::cos(2*pi*j/length);
        const auto product=x[j+raw.lag_samples]*(raw.conjugate?x[j]:std::conj(x[j]));
        const auto block=std::min<std::size_t>(3,4*j/length);
        cross[block]+=w*product*oscillator;energy[block]+=w*std::norm(product);oscillator*=advance;
    }
    std::complex<double> sum{};double magnitude=0,total=0;
    for(std::size_t b=0;b<4;++b){sum+=cross[b];magnitude+=std::abs(cross[b]);total+=energy[b];}
    if(magnitude>0)r.block_phase_coherence_squared=std::clamp(std::norm(sum)/(magnitude*magnitude),0.0,1.0);
    if(total>0)r.min_block_energy_fraction=*std::min_element(energy.begin(),energy.end())/total;
    return r;
}
bool support(const BackgroundPartition& p){return p.reference_bins>=16 && p.line_to_median>=16 && p.line_to_upper>=8 &&
    p.block_phase_coherence_squared>=.8 && p.min_block_energy_fraction>=.02;}
}
std::vector<std::size_t> cyclic_reference_bins(std::size_t n,int coarse) {
    if(n<1024 || n>8192 || (n&(n-1)) || std::abs(coarse)<9 || std::abs(coarse)>=int(n/2)-1)
        throw std::invalid_argument("invalid cyclic reference geometry");
    std::vector<std::size_t> bins;
    for(int sign:{-1,1})for(int distance=4;distance<=20;++distance) {
        const int k=coarse+sign*distance;
        if(k*coarse>0 && std::abs(k)>=9 && std::abs(k)<int(n/2)-1)bins.push_back(k<0?n+k:std::size_t(k));
    }
    return bins;
}
bool cyclic_background_supported(const CyclicBackgroundPeak& p,const RatePeak& raw) {
    return raw.discovery_coherence_squared>=.05 && raw.holdout_coherence_squared>=.05 && support(p.discovery) && support(p.holdout);
}
CyclicBackground measure_cyclic_background(const std::vector<std::complex<float>>& iq,double rate,const WaveformFeatures& selected) {
    if(iq.size()>65536 || !std::isfinite(rate) || rate<=0 || rate>1e9 || selected.samples_examined!=iq.size() || selected.peaks.size()>8)
        throw std::invalid_argument("invalid cyclic background input");
    double scale=0;for(auto z:iq) {
        if(!std::isfinite(z.real()) || !std::isfinite(z.imag()))throw std::invalid_argument("nonfinite cyclic background IQ");
        scale=std::max({scale,std::abs(double(z.real())),std::abs(double(z.imag()))});
    }
    CyclicBackground r;r.samples_examined=iq.size();r.status=selected.cyclic_status;
    r.partition_samples=selected.partition_samples;r.holdout_offset=selected.holdout_offset;
    std::size_t n=0;if(iq.size()>=2064){n=1024;while(n<8192 && 4*n+16<=iq.size())n*=2;}
    if(r.partition_samples!=n || r.holdout_offset!=(n?iq.size()-n:0) ||
        (r.status!="measured" && r.status!="no_variation" && r.status!="insufficient_samples") ||
        (r.status=="insufficient_samples")!=(!n) || (r.status!="measured" && !selected.peaks.empty()))
        throw std::invalid_argument("cyclic background partition mismatch");
    if(r.status!="measured")return r;
    if(!scale)throw std::invalid_argument("missing cyclic background energy");
    const auto train=centered(iq,0,n,scale),held=centered(iq,r.holdout_offset,n,scale);
    std::unique_ptr<kiss_fft_state,Free> fft(kiss_fft_alloc(int(n),0,nullptr,nullptr));if(!fft)throw std::bad_alloc();
    std::array<Spectrum,8> train_spectra,held_spectra;
    for(const auto& raw:selected.peaks) {
        const auto lag=std::find(std::begin(cyclic_lags),std::end(cyclic_lags),raw.lag_samples);
        if(lag==std::end(cyclic_lags) || !std::isfinite(raw.alpha_hz) || !std::isfinite(raw.coarse_alpha_hz) ||
            !std::isfinite(raw.discovery_coherence_squared) || !std::isfinite(raw.holdout_coherence_squared) ||
            raw.discovery_coherence_squared<0 || raw.discovery_coherence_squared>1 || raw.holdout_coherence_squared<0 || raw.holdout_coherence_squared>1)
            throw std::invalid_argument("invalid selected cyclic peak");
        const double coarse=raw.coarse_alpha_hz/(rate/n);
        if(!std::isfinite(coarse) || std::abs(coarse)<9 || std::abs(coarse)>=double(n/2)-1)throw std::invalid_argument("invalid cyclic coarse bin");
        const int bin=int(std::round(coarse));
        if(std::abs(coarse-bin)>1e-8 || std::abs(raw.alpha_hz-raw.coarse_alpha_hz)>.500000001*rate/n || (!raw.conjugate && bin<=0))
            throw std::invalid_argument("unsearched cyclic rate");
        const auto refs=cyclic_reference_bins(n,bin);const auto key=std::size_t(lag-std::begin(cyclic_lags))+4*raw.conjugate;
        if(train_spectra[key].coherence.empty()) {
            train_spectra[key]=spectrum(train,raw.lag_samples,raw.conjugate,fft.get());
            held_spectra[key]=spectrum(held,raw.lag_samples,raw.conjugate,fft.get());r.fft_calls+=2;
        }
        CyclicBackgroundPeak p;
        p.discovery=examine(train,train_spectra[key],raw,refs,raw.alpha_hz/rate,raw.discovery_coherence_squared);
        p.holdout=examine(held,held_spectra[key],raw,refs,raw.alpha_hz/rate,raw.holdout_coherence_squared);
        p.background_supported=cyclic_background_supported(p,raw);r.peaks.push_back(p);
    }
    return r;
}
} // namespace rfmon::cyclo
