#include "chirp_refinement.hpp"
#include <kissfft/kiss_fft.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
constexpr double pi = 3.14159265358979323846;
struct FftFree { void operator()(kiss_fft_state* p) const { kiss_fft_free(p); } };
struct Peak { std::size_t offset; double score; };
double score(std::complex<double> dot, double a, double b, double floor) {
    return a > floor && b > floor ? std::clamp(std::norm(dot)/(a*b), 0.0, 1.0) : 0;
}
// Keep four neighborhoods instead of allowing adjacent maxima to consume the
// shortlist. The full FFT checks still share selection data, not holdout data.
void retain(std::vector<Peak>& peaks, Peak next, std::size_t separation) {
    for (auto& p : peaks) if (std::max(p.offset,next.offset)-std::min(p.offset,next.offset) < separation) {
        if (next.score > p.score) p = next;
        return;
    }
    if (peaks.size() < 4) peaks.push_back(next);
    else {
        auto worst = std::min_element(peaks.begin(),peaks.end(),[](auto a, auto b) { return a.score < b.score; });
        if (next.score > worst->score) *worst = next;
    }
}
struct Spectrum {
    std::unique_ptr<kiss_fft_state,FftFree> fft;
    std::vector<kiss_fft_cpx> input, output;
    explicit Spectrum(std::size_t n) : fft(kiss_fft_alloc(int(n),0,nullptr,nullptr)),input(n),output(n) {
        if (!fft) throw std::runtime_error("refinement FFT allocation failed");
    }
    std::vector<double> power(const std::vector<std::complex<double>>& x, std::size_t first,
                              std::size_t count, double rate, double slope, std::size_t phase_origin) {
        std::fill(input.begin(),input.end(),kiss_fft_cpx{});
        // Normalize each admitted span separately to avoid overflow and retain
        // amplitude-scale invariance; energies used for balance remain separate.
        double scale = 0;
        for (std::size_t j=0;j<count;++j) scale = std::max(scale,std::abs(x[first+j]));
        if (scale == 0) return std::vector<double>(input.size());
        for (std::size_t j=0;j<count;++j) {
            const double t = double(first+j-phase_origin)/rate;
            const double hann = (1-std::cos(2*pi*j/count))/2;
            const auto z = x[first+j]/scale * hann * std::polar(1.0,std::remainder(-pi*slope*t*t,2*pi));
            input[j] = {float(z.real()),float(z.imag())};
        }
        kiss_fft(fft.get(),input.data(),output.data());
        std::vector<double> p(input.size());
        for (std::size_t k=0;k<p.size();++k) p[k] = double(output[k].r)*output[k].r+double(output[k].i)*output[k].i;
        return p;
    }
};
double fraction(const std::vector<double>& p, std::size_t start, std::size_t width) {
    double total=0, selected=0;
    for (auto v:p) total+=v;
    for (std::size_t j=0;j<width;++j) selected+=p[(start+j)%p.size()];
    return total > 0 ? std::clamp(selected/total,0.0,1.0) : 0;
}
std::pair<std::size_t,double> best_band(const std::vector<double>& p, std::size_t width) {
    double total=0, sum=0;
    for (auto v:p) total+=v;
    for (std::size_t j=0;j<width;++j) sum+=p[j];
    double best=sum;std::size_t index=0;
    for (std::size_t k=1;k<p.size();++k) {
        sum+=p[(k+width-1)%p.size()]-p[k-1];
        if (sum>best) {best=sum;index=k;}
    }
    return {index,total>0 ? std::clamp(best/total,0.0,1.0) : 0};
}
}
std::vector<ChirpRefinement> refine_chirp_candidates(const std::vector<std::complex<float>>& iq,
                                                    double rate, const std::vector<ChirpMeasurement>& legacy) {
    if (!std::isfinite(rate) || rate<=0 || rate>1e9 || iq.size()>65536 || legacy.size()!=3)
        throw std::invalid_argument("invalid refinement rate, sample or hypothesis budget");
    const auto bank = measure_chirp_structure({},rate);
    std::complex<double> mean{};
    for (auto z:iq) {
        if (!std::isfinite(z.real()) || !std::isfinite(z.imag())) throw std::invalid_argument("nonfinite refinement IQ");
        mean+=std::complex<double>(z);
    }
    if (!iq.empty()) mean/=double(iq.size());
    std::vector<std::complex<double>> x(iq.size());double scale=0,energy=0;
    for (std::size_t i=0;i<x.size();++i) {x[i]=std::complex<double>(iq[i])-mean;scale=std::max(scale,std::abs(x[i]));}
    if (scale>0) for (auto& z:x) {z/=scale;energy+=std::norm(z);}
    std::vector<ChirpRefinement> results;
    for (std::size_t index=0;index<bank.size();++index) {
        const auto& h=bank[index];const auto& old=legacy[index];
        if (old.label!=h.label || old.slope_hz_per_second!=h.slope_hz_per_second || old.nominal_sweep_hz!=h.nominal_sweep_hz)
            throw std::invalid_argument("unexpected refinement hypothesis");
        ChirpRefinement r;r.label=h.label;r.nominal_slope_hz_per_second=h.slope_hz_per_second;
        r.span_samples=2*h.lag_samples;
        const auto lag=std::size_t(std::llround(rate*8e-6));
        if (rate<=h.nominal_sweep_hz || lag<8) {r.status="unsupported_sample_rate";results.push_back(r);continue;}
        if (iq.size()<r.span_samples) {results.push_back(r);continue;}
        if (energy<=0) {r.status="no_variation";results.push_back(r);continue;}
        const auto gate=r.span_samples-lag,positions=x.size()-r.span_samples+1;
        const double floor=energy/x.size()*gate*1e-10;
        std::size_t fft_size=32;while (fft_size<r.span_samples) fft_size*=2;
        Spectrum fft(fft_size);const double bin=rate/fft_size;
        bool selected=false;double best=-1;
        for (double factor:{.998,.999,1.0,1.001,1.002}) {
            const double slope=h.slope_hz_per_second*factor;
            const double phase_step=-2*pi*slope*lag/(rate*rate);
            const auto advance=std::polar(1.0,phase_step);std::complex<double> oscillator{1,0};
            std::vector<std::complex<double>> products(x.size()-lag);
            for (std::size_t n=0;n<products.size();++n) {
                if (n%1024==0) oscillator=std::polar(1.0,std::remainder(phase_step*n,2*pi));
                products[n]=x[n+lag]*std::conj(x[n])*oscillator;oscillator*=advance;
            }
            std::vector<Peak> peaks;std::complex<double> dot{};double a=0,b=0;
            for (std::size_t start=0;start<positions;++start) {
                if (start%1024==0) {
                    dot={};a=b=0;
                    for (std::size_t n=start;n<start+gate;++n) {dot+=products[n];a+=std::norm(x[n]);b+=std::norm(x[n+lag]);}
                }
                if (a>floor && b>floor) retain(peaks,{start,score(dot,a,b,floor)},lag);
                if (start+1<positions) {
                    const auto end=start+gate;dot+=products[end]-products[start];
                    a+=std::norm(x[end])-std::norm(x[start]);b+=std::norm(x[end+lag])-std::norm(x[start+lag]);
                }
            }
            r.positions_examined+=positions;
            if (old.status=="measured") {
                if (old.peak_offset>=positions) throw std::invalid_argument("escaping legacy refinement anchor");
                peaks.push_back({old.peak_offset,0});
            }
            // Echo-delay allowance 2 us + four FFT bins. This is an engineering
            // morphology tolerance, not a measured delay spread or bandwidth.
            const auto width=std::min(fft_size,std::size_t(std::ceil(std::abs(slope)*2e-6/bin))+4);
            for (const auto& candidate:peaks) {
                const auto start=candidate.offset;
                auto full=fft.power(x,start,r.span_samples,rate,slope,start);
                const auto band=best_band(full,width);
                auto first=fft.power(x,start,r.span_samples/2,rate,slope,start);
                auto second=fft.power(x,start+r.span_samples/2,r.span_samples-r.span_samples/2,rate,slope,start);
                const double f=fraction(first,band.first,width),s=fraction(second,band.first,width);
                double pa=0,pb=0;
                for (std::size_t j=0;j<r.span_samples;++j) (j<r.span_samples/2 ? pa : pb)+=std::norm(x[start+j]);
                const double balance=std::max(pa,pb)>0 ? std::min(pa,pb)/std::max(pa,pb) : 0;
                // Rank on the weaker full/half spectrum, so a single bright
                // fragment cannot dominate selection of a supposed full span.
                const double merit=std::min({band.second,f,s});r.spectra_examined+=3;
                if (!selected || merit>best) {
                    selected=true;best=merit;r.peak_offset=start;r.selected_slope_hz_per_second=slope;
                    r.coarse_coherence_squared=candidate.score;r.dechirped_band_power_fraction=band.second;
                    r.first_half_band_power_fraction=f;r.second_half_band_power_fraction=s;r.half_energy_balance=balance;
                    r.dechirped_band_width_hz=width*bin;
                    double center=(band.first+(width-1)/2.0)*bin;
                    center=std::remainder(center,rate);
                    r.dechirped_band_center_hz=center;
                    r.estimated_span_mid_frequency_hz=center+slope*(r.span_samples-1)/(2*rate);
                }
            }
        }
        r.status=selected ? "measured" : "no_eligible_energy";
        if (selected) {
            // The legacy anchor did not participate in the coarse search.
            // Measure its coarse score explicitly rather than publishing the
            // shortlist placeholder (zero) as an actual observation.
            std::complex<double> dot{};double a=0,b=0;
            const double phase_step=-2*pi*r.selected_slope_hz_per_second*lag/(rate*rate);
            for (std::size_t j=0;j<gate;++j) {
                const auto n=r.peak_offset+j;
                dot+=x[n+lag]*std::conj(x[n])*std::polar(1.0,std::remainder(phase_step*n,2*pi));
                a+=std::norm(x[n]);b+=std::norm(x[n+lag]);
            }
            r.coarse_coherence_squared=score(dot,a,b,floor);
        }
        r.shape_consistent=selected && r.dechirped_band_power_fraction>=.8 &&
            r.first_half_band_power_fraction>=.7 && r.second_half_band_power_fraction>=.7 && r.half_energy_balance>=.25;
        results.push_back(r);
    }
    return results;
}
} // namespace rfmon::cyclo
