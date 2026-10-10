#include "cyclostationary/named_link_candidates.hpp"
#include <iostream>
#include <random>
using namespace rfmon::cyclo;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try {
    SpectralFeatures s;s.quality="measured";s.frames=64;s.variance_power=1;s.power_fraction.assign(512,1./512);
    s.bin_hz=20e6/512;s.occupied_low_hz=-9e6;s.occupied_high_hz=9e6;s.strongest_bin_fraction=1./512;
    for(double rate:{20e6,60e6,200e6})for(std::size_t profile=0;profile<3;++profile) {
        std::vector<std::complex<float>> iq(65536);const auto& p=ocusync_marker_profiles[profile];
        const std::size_t start=811,n=std::llround(rate*66.67e-6);
        for(std::size_t i=0;i<n;++i) {
            const double t=i/rate,begin=-p.slope*66.67e-6/2;
            iq[start+i]=std::polar(1.f,float(2*3.14159265358979323846*(begin*t+p.slope*t*t/2)));
        }
        auto measured=measure_chirp_structure(iq,rate);
        auto r=assess_named_link_candidates(s,measured,{rate,rate,-1},iq.size());
        check(r.count==3 && r.candidates[profile].marker_compatible,"physical marker not matched");
        check(std::string(r.candidates[profile].status)=="unvalidated_candidate","synthetic marker must never be accepted family");
        check(r.candidates[profile].blocker_count>3,"missing validation blockers");
        measured[profile].label="arbitrary_not_a_vendor_label";
        check(assess_named_link_candidates(s,measured,{rate,rate,-1},iq.size()).candidates[profile].marker_compatible,"label used as evidence");
        measured[profile].frequency_slope_hz_per_second*=.7;
        check(!assess_named_link_candidates(s,measured,{rate,rate,-1},iq.size()).candidates[profile].marker_compatible,"wrong measured slope accepted");
        measured=measure_chirp_structure(iq,rate);measured[profile].phase_unwraps=1;
        check(!assess_named_link_candidates(s,measured,{rate,rate,-1},iq.size()).candidates[profile].marker_compatible,"alias ambiguity accepted");
        measured[profile].phase_unwraps=0;measured[profile].peak_offset=iq.size();
        check(!assess_named_link_candidates(s,measured,{rate,rate,-1},iq.size()).candidates[profile].marker_compatible,"support outside window accepted");
    }
    std::mt19937 rng(1093);std::normal_distribution<float> normal;
    std::vector<std::complex<float>> noise(65536);for(auto& x:noise)x={normal(rng),normal(rng)};
    for(bool tone:{false,true}) {
        if(tone)for(std::size_t i=0;i<noise.size();++i)noise[i]=std::polar(1.f,float(.2*i));
        auto r=assess_named_link_candidates(s,measure_chirp_structure(noise,20e6),{20e6,20e6,-1},noise.size());
        for(std::size_t i=0;i<r.count;++i)check(!r.candidates[i].marker_compatible,"tone/noise promoted to marker");
    }
    auto m=measure_chirp_structure({},20e6);
    check(std::string(assess_named_link_candidates(s,m,{20e6,0,-1},0).candidates[0].status)=="unavailable","empty input promoted");
    m[0].slope_hz_per_second=1;
    check(assess_named_link_candidates(s,m,{20e6,0,-1},0).count==2,"arbitrary template assigned family");
    std::cout<<"named marker controls, labels, sample support and conservative status passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
