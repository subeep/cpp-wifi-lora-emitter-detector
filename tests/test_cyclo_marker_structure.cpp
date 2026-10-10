#include "cyclostationary/marker_structure.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <limits>
using namespace rfmon::cyclo;
void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
int main(){try {
    constexpr double pi=3.14159265358979323846;
    for(double rate:{20e6,25e6,100e6,120e6,200e6})for(double slope:{135.2e9,270.2e9,-135.3e9}) {
        const std::size_t start=std::llround(rate*20e-6),n=std::llround(rate/15000),g=std::llround(rate*4e-6);
        std::vector<std::complex<float>> x(std::llround(rate*120e-6));
        for(std::size_t j=0;j<n;++j){double t=(double(j)-double(n-1)/2)/rate;x[start+j]=std::polar(1.f,float(pi*slope*t*t));}
        for(std::size_t j=0;j<g;++j)x[start-g+j]=x[start+n-g+j];
        auto chirps=measure_chirp_structure(x,rate);auto result=measure_marker_structures(x,rate,chirps);
        bool matched=false;for(std::size_t i=0;i<3;++i){check(valid_marker_structure(result[i],rate,x.size(),chirps[i]),"invalid result");
            if(chirps[i].slope_hz_per_second==slope){matched=result[i].joint_compatible;
                auto forged=result[i];forged.start=x.size();check(!valid_marker_structure(forged,rate,x.size(),chirps[i]),"escaping span accepted");
                forged=result[i];forged.coherence=std::numeric_limits<double>::quiet_NaN();check(!valid_marker_structure(forged,rate,x.size(),chirps[i]),"nan accepted");}}
        check(matched,"synthetic copied-prefix chirp not matched");
        for(std::size_t j=0;j<g;++j)x[start-g+j]={};
        chirps=measure_chirp_structure(x,rate);result=measure_marker_structures(x,rate,chirps);
        for(auto r:result)check(!r.joint_compatible,"missing prefix accepted");
    }
    bool rejected=false;try{measure_marker_structures(std::vector<std::complex<float>>(65537),100e6,{});}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"sample budget not enforced");
    std::cout<<"native marker structure: five rates, three profiles, prefix rejection and validation passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
