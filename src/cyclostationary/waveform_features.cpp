#include "waveform_features.hpp"
#include <kissfft/kiss_fft.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
constexpr double pi = 3.14159265358979323846;
struct FftFree { void operator()(kiss_fft_state* p) const { kiss_fft_free(p); } };
using Samples = std::vector<std::complex<double>>;
Samples partition(const std::vector<std::complex<float>>& iq, std::size_t at, std::size_t n, double scale) {
    Samples x(n); std::complex<double> mean{};
    for (std::size_t i=0;i<n;++i) { x[i]=std::complex<double>(iq[at+i])/scale; mean+=x[i]; }
    mean/=double(n); for (auto& z:x) z-=mean; return x;
}
double power(const Samples& x) { double p=0; for(auto z:x) p+=std::norm(z); return p; }
double weight(std::size_t j, std::size_t n) { return .5-.5*std::cos(2*pi*j/n); }
double coherence(const Samples& x, std::size_t lag, bool conjugate, double alpha_normalized) {
    std::complex<double> sum{}, oscillator{1,0}; double a=0,b=0;
    const auto advance=std::polar(1.0,-2*pi*alpha_normalized); const auto n=x.size()-lag;
    for(std::size_t j=0;j<n;++j) {
        if(j%512==0) oscillator=std::polar(1.0,-2*pi*alpha_normalized*j);
        const double w=weight(j,n);
        sum+=w*x[j+lag]*(conjugate?x[j]:std::conj(x[j]))*oscillator;
        a+=w*std::norm(x[j+lag]); b+=w*std::norm(x[j]); oscillator*=advance;
    }
    return a>0 && b>0 ? std::clamp(std::norm(sum)/(a*b),0.0,1.0):0;
}
struct Proposal { bool conjugate; std::size_t lag; int bin; double score; };
int distance(int a,int b) { const int d=std::abs(a-b); return std::min(d,256-d); }
double quantile(const std::vector<double>& sorted, double p) { return sorted[std::size_t(p*(sorted.size()-1))]; }
void morphology(const std::vector<std::complex<float>>& iq, double scale, double rate, WaveformFeatures& r) {
    if(iq.size()<128) return;
    if(scale==0) { r.morphology_status="no_energy"; return; }
    std::vector<double> powers(iq.size()); double sum=0,amp=0;
    for(std::size_t i=0;i<iq.size();++i) { powers[i]=std::norm(std::complex<double>(iq[i])/scale); sum+=powers[i]; amp+=std::sqrt(powers[i]); }
    if(sum==0) return;
    r.morphology_status="measured"; const double mean=sum/iq.size(), mean_amp=amp/iq.size();
    r.amplitude_cv=std::sqrt(std::max(0.0,mean-mean_amp*mean_amp))/mean_amp;
    auto sorted=powers; std::sort(sorted.begin(),sorted.end());
    const double low=quantile(sorted,.2),high=quantile(sorted,.8),spread=high-low;
    r.envelope_low=low/mean; r.envelope_high=high/mean;
    if(spread>1e-8*mean) {
        r.envelope_contrast=spread/high; bool last=powers[0]>(low+high)/2; double residual=0;
        for(std::size_t i=0;i<powers.size();++i) {
            const double p=powers[i]; const bool state=p>(low+high)/2;
            r.envelope_high_fraction+=state; if(i && state!=last) ++r.envelope_transitions;
            last=state; residual+=std::min((p-low)*(p-low),(p-high)*(p-high));
        }
        r.envelope_high_fraction/=powers.size(); r.envelope_fit_residual=residual/(powers.size()*spread*spread);
        r.two_level_envelope_pattern=r.envelope_contrast>=.7 && r.envelope_fit_residual<=.05 &&
            r.envelope_high_fraction>=.1 && r.envelope_high_fraction<=.9 && r.envelope_transitions>=8;
    }
    std::array<std::size_t,256> histogram{}; std::vector<int> bins(iq.size()-1,-1);
    std::vector<double> phases(iq.size()-1);
    for(std::size_t j=0;j+1<iq.size();++j) {
        if(powers[j]<=mean*1e-4 || powers[j+1]<=mean*1e-4) continue;
        const auto product=std::complex<double>(iq[j+1])/scale*std::conj(std::complex<double>(iq[j])/scale);
        const double phase=std::arg(product); phases[j]=phase;
        const int bin=std::min(255,int((phase+pi)/(2*pi)*256));
        bins[j]=bin; ++histogram[bin]; ++r.phase_pairs;
    }
    if(!r.phase_pairs) return;
    int first=0; for(int k=1;k<256;++k) if(histogram[k]>histogram[first]) first=k;
    int second=-1; for(int k=0;k<256;++k) if(distance(k,first)>=5 && (second<0 || histogram[k]>histogram[second])) second=k;
    std::size_t a=0,b=0; std::complex<double> center_a{},center_b{}; int last=-1; std::size_t last_index=0;
    for(std::size_t j=0;j<bins.size();++j) {
        const int k=bins[j]; int state=-1;
        if(k>=0 && distance(k,first)<=2) { ++a; center_a+=std::polar(1.0,phases[j]); state=0; }
        else if(k>=0 && distance(k,second)<=2) { ++b; center_b+=std::polar(1.0,phases[j]); state=1; }
        if(state>=0 && last>=0 && j==last_index+1 && state!=last) ++r.frequency_transitions;
        last=state; last_index=j;
    }
    r.frequency_first_fraction=double(a)/r.phase_pairs; r.frequency_second_fraction=double(b)/r.phase_pairs;
    r.frequency_concentration=std::min(1.0,r.frequency_first_fraction+r.frequency_second_fraction);
    if(a) r.frequency_low_hz=std::arg(center_a)*rate/(2*pi);
    if(b) r.frequency_high_hz=std::arg(center_b)*rate/(2*pi);
    if(r.frequency_low_hz>r.frequency_high_hz) std::swap(r.frequency_low_hz,r.frequency_high_hz);
    r.two_frequency_pattern=r.phase_pairs>=.8*(iq.size()-1) && r.amplitude_cv<=.3 &&
        r.frequency_concentration>=.8 && r.frequency_first_fraction>=.1 && r.frequency_second_fraction>=.1 &&
        r.frequency_transitions>=8;
}
} // namespace

WaveformFeatures measure_waveform_features(const std::vector<std::complex<float>>& iq, double rate) {
    if(!std::isfinite(rate) || rate<=0 || rate>1e9 || iq.size()>65536)
        throw std::invalid_argument("invalid waveform rate or sample budget");
    double scale=0; for(auto z:iq) {
        if(!std::isfinite(z.real()) || !std::isfinite(z.imag())) throw std::invalid_argument("nonfinite waveform IQ");
        scale=std::max({scale,std::abs(double(z.real())),std::abs(double(z.imag()))});
    }
    WaveformFeatures r; r.samples_examined=iq.size(); morphology(iq,scale,rate,r);
    if(iq.size()<2064) return r;
    std::size_t n=1024; while(n<cyclic_partition_limit && 2*(n*2)+16<=iq.size()) n*=2;
    r.partition_samples=n; r.holdout_offset=iq.size()-n; r.alpha_bin_hz=rate/n;
    if(!scale) { r.cyclic_status="no_variation"; return r; }
    const auto train=partition(iq,0,n,scale),hold=partition(iq,r.holdout_offset,n,scale);
    if(power(train)<1e-20 || power(hold)<1e-20) { r.cyclic_status="no_variation"; return r; }
    r.cyclic_status="measured";
    std::unique_ptr<kiss_fft_state,FftFree> fft(kiss_fft_alloc(int(n),0,nullptr,nullptr));
    if(!fft) throw std::bad_alloc();
    std::vector<kiss_fft_cpx> input(n),output(n); std::vector<Proposal> proposals;
    for(bool conjugate:{false,true}) for(auto lag:cyclic_lags) {
        const auto length=n-lag; double a=0,b=0;
        for(std::size_t j=0;j<n;++j) {
            std::complex<double> v{};
            if(j<length) {
                const double w=weight(j,length); v=w*train[j+lag]*(conjugate?train[j]:std::conj(train[j]));
                a+=w*std::norm(train[j+lag]); b+=w*std::norm(train[j]);
            }
            input[j]={float(v.real()),float(v.imag())};
        }
        kiss_fft(fft.get(),input.data(),output.data()); ++r.fft_calls;
        if(a<=0 || b<=0) continue;
        auto score=[&](std::size_t k) { return double(output[k].r)*output[k].r+double(output[k].i)*output[k].i; };
        for(std::size_t k=1;k<n;++k) {
            const int bin=k<n/2?int(k):int(k)-int(n);
            if(std::abs(bin)<9 || std::abs(bin)>=int(n/2)-1 || (!conjugate && bin<0)) continue;
            if(score(k)>=score(k-1) && score(k)>=score((k+1)%n))
                proposals.push_back({conjugate,lag,bin,std::clamp(score(k)/(a*b),0.0,1.0)});
        }
    }
    std::stable_sort(proposals.begin(),proposals.end(),[](const Proposal& a,const Proposal& b){return a.score>b.score;});
    for(bool conjugate:{false,true}) {
        std::size_t count=0;
        for(const auto& p:proposals) {
            if(p.conjugate!=conjugate) continue;
            bool duplicate=false; for(const auto& old:r.peaks) if(old.conjugate==conjugate &&
                std::abs(old.coarse_alpha_hz-p.bin*r.alpha_bin_hz)<2*r.alpha_bin_hz) duplicate=true;
            if(duplicate) continue;
            RatePeak peak; peak.conjugate=conjugate; peak.lag_samples=p.lag; peak.coarse_alpha_hz=p.bin*r.alpha_bin_hz;
            for(double delta:{-.5,-.25,0.0,.25,.5}) {
                const double alpha=(p.bin+delta)/n;
                const double value=coherence(train,p.lag,conjugate,alpha); ++r.refinement_evaluations;
                if(value>peak.discovery_coherence_squared) { peak.discovery_coherence_squared=value; peak.alpha_hz=alpha*rate; }
            }
            // Selection uses discovery only. Holdout cannot move the selected frequency/lag.
            if(peak.discovery_coherence_squared==0) peak.alpha_hz=peak.coarse_alpha_hz;
            peak.holdout_coherence_squared=coherence(hold,p.lag,conjugate,peak.alpha_hz/rate);
            peak.persistent_pattern=peak.discovery_coherence_squared>=.05 && peak.holdout_coherence_squared>=.05;
            r.peaks.push_back(peak); if(++count==4) break;
        }
    }
    return r;
}
} // namespace rfmon::cyclo
