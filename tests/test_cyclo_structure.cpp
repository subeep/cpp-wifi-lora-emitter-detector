#include "cyclostationary/structure_discovery.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace rfmon::cyclo;
namespace {
constexpr double pi=3.14159265358979323846, rate=20e6;
using IQ=std::vector<std::complex<float>>;
int failures=0;
void check(bool ok,const char* why){if(!ok){std::cerr<<"FAIL: "<<why<<'\n';++failures;}}
IQ noise(unsigned seed){std::mt19937 e(seed);std::normal_distribution<float> d;IQ x(65536);for(auto& z:x)z={d(e),d(e)};return x;}
IQ cp(unsigned seed,std::size_t useful,std::size_t prefix){
    // Independent random complex time-domain symbols, with an exact copied
    // suffix. Tests CP geometry without reusing discovery/FFT implementation.
    auto random=noise(seed);IQ x; x.reserve(65536);std::size_t source=0;
    while(x.size()<65536){const auto at=source;source=(source+useful)%random.size();
        for(std::size_t j=useful-prefix;j<useful && x.size()<65536;++j)x.push_back(random[(at+j)%random.size()]);
        for(std::size_t j=0;j<useful && x.size()<65536;++j)x.push_back(random[(at+j)%random.size()]);}return x;
}
std::vector<std::complex<double>> centered(const IQ& x,std::size_t start,std::size_t n){
    std::complex<double> mean{};for(std::size_t j=0;j<n;++j)mean+=std::complex<double>(x[start+j]);mean/=double(n);
    std::vector<std::complex<double>> z(n);for(std::size_t j=0;j<n;++j)z[j]=std::complex<double>(x[start+j])-mean;return z;
}
// Direct sums with absolute coordinates; no FFT, lag ranking, or folded rows.
void cp_oracle(const IQ& x,const StructureDiscovery& s,const DiscoveredOfdm& r){
    auto train=centered(x,0,s.partition_samples);std::complex<double> sum{};double a=0,b=0;
    for(std::size_t j=0;j+r.useful_samples<train.size();++j){sum+=train[j+r.useful_samples]*std::conj(train[j]);a+=std::norm(train[j+r.useful_samples]);b+=std::norm(train[j]);}
    check(std::abs(std::norm(sum)/(a*b)-r.lag_coherence_squared)<1e-6,"linear FFT lag autocorrelation equals direct oracle");
    for(std::size_t origin:{std::size_t(0),s.holdout_offset}){
        const auto z=centered(x,origin,s.partition_samples);const auto period=r.useful_samples+r.prefix_samples;
        std::complex<double> inside{},outside{},cyclic{};double ia=0,ib=0,oa=0,ob=0;
        const auto pairs=(z.size()-r.useful_samples)/period*period;
        for(std::size_t j=0;j<pairs;++j){const auto u=z[j+r.useful_samples],v=z[j];const auto phase=(origin+j)%period;
            const auto product=u*std::conj(v);cyclic+=product*std::polar(1.0,-2*pi*phase/period);
            if((phase+period-r.phase_samples)%period<r.prefix_samples){inside+=product;ia+=std::norm(u);ib+=std::norm(v);}
            else {outside+=product;oa+=std::norm(u);ob+=std::norm(v);}}
        check(std::abs(std::norm(inside)/(ia*ib)-(origin?r.holdout_prefix:r.train_prefix))<1e-9,"CP direct gated oracle");
        check(std::abs(std::norm(outside)/(oa*ob)-(origin?r.holdout_outside:r.train_outside))<1e-9,"outside direct oracle");
        if(origin)check(std::abs(std::norm(cyclic)/((ia+oa)*(ib+ob))-r.holdout_cyclic)<1e-9,"symbol cycle direct oracle");
    }
}
IQ barker(std::size_t chip,double carrier,int shift=3,bool wrong=false){
    constexpr int code[]={1,1,1,-1,-1,-1,1,-1,-1,1,-1};IQ x(65536);std::mt19937 e(701);int bit=1;std::size_t word=99999;
    for(std::size_t j=0;j<x.size();++j){auto index=(j+shift)/chip; if(index/11!=word){word=index/11;bit=(e()&1)?1:-1;}
        const int sign=wrong?((index%11<5)?1:-1):code[index%11];x[j]=std::complex<float>(double(bit*sign)*std::polar(1.0,2*pi*carrier*j/rate));}return x;
}
void code_oracle(const IQ& x,const StructureDiscovery& s,const SpreadMeasurement& r){
    constexpr int code[]={1,1,1,-1,-1,-1,1,-1,-1,1,-1};
    for(std::size_t origin:{std::size_t(0),s.holdout_offset}){
        auto z=centered(x,origin,s.partition_samples);double sum=0;std::size_t words=0;
        const auto period=11*r.chip_samples;
        for(std::size_t j=0;j+period<=z.size();++j){if((origin+j)%period!=r.code_phase_samples)continue;
            std::complex<double> correlation{};double power=0;
            for(std::size_t k=0;k<11;++k){std::complex<double> integrated{};
                for(std::size_t t=0;t<r.chip_samples;++t){const auto index=j+k*r.chip_samples+t;integrated+=z[index]*std::polar(1.0,-2*pi*r.carrier_hz*(origin+index)/rate);}
                integrated/=double(r.chip_samples);correlation+=double(code[k])*integrated;power+=std::norm(integrated);}
            if(power>1e-20){sum+=std::norm(correlation)/(11*power);++words;}}
        check(words==(origin?r.holdout_words:r.train_words),"complete held code word count and absolute phase");
        check(words && std::abs(sum/words-(origin?r.holdout_code_coherence_squared:r.train_code_coherence_squared))<1e-8,"direct chip integration/code oracle");
    }
}
void tests(){
    for(auto timing:{std::pair<std::size_t,std::size_t>{96,24},{96,20},{128,16},{160,32},{256,64},{512,64}}){
        auto x=cp(42,timing.first,timing.second);auto original=x;auto s=discover_waveform_structure(x,rate);bool found=false;
        check(s.ofdm_fft_calls<=6 && s.timing_hypotheses<=32 && s.spread_hypotheses==32,"bounded searches");
        for(auto& r:s.ofdm){cp_oracle(x,s,r);found|=r.useful_samples==timing.first && r.prefix_samples==timing.second && r.pattern_consistent;}
        if(!found){std::cerr<<"missing timing "<<timing.first<<'+'<<timing.second<<'\n';for(auto& r:s.ofdm)std::cerr<<r.useful_samples<<'+'<<r.prefix_samples<<' '<<r.train_prefix<<' '<<r.holdout_prefix<<'\n';}
        check(found,"non-WLAN CP timing recovered");check(x==original,"structure leaves source untouched");
        auto null=noise(93);std::copy(null.begin()+s.holdout_offset,null.end(),x.begin()+s.holdout_offset);
        auto held=discover_waveform_structure(x,rate);for(auto& r:held.ofdm)check(!r.pattern_consistent,"discovery-only CP rejected on holdout");
    }
    for(auto chip:{1,4,8,16})for(auto frequency:{0.,350000.,-350000.,7000000.}){
        auto x=barker(chip,frequency);auto s=discover_waveform_structure(x,rate);bool found=false;
        for(const auto& r:s.spread){code_oracle(x,s,r);found|=r.chip_samples==std::size_t(chip) && r.pattern_consistent;}
        check(found,"Barker chip/offset/carrier alias recovered");
        auto null=noise(23);std::copy(null.begin()+s.holdout_offset,null.end(),x.begin()+s.holdout_offset);
        auto held=discover_waveform_structure(x,rate);for(auto& r:held.spread)check(!r.pattern_consistent,"discovery-only Barker rejected without refitting");
    }

    auto reference=barker(8,350000); const auto original=discover_waveform_structure(reference,rate);
    for(double gain:{1e-25,1e25}) {
        auto scaled=reference;for(auto& z:scaled)z*=float(gain);const auto s=discover_waveform_structure(scaled,rate);
        check(s.spread.size()==original.spread.size(),"gain-invariant code proposal count");
        for(std::size_t j=0;j<s.spread.size();++j)check(std::abs(s.spread[j].holdout_code_coherence_squared-original.spread[j].holdout_code_coherence_squared)<1e-6,"gain-invariant held code coherence");
    }
    bool invalid_rate=false;try{discover_waveform_structure(reference,0);}catch(const std::invalid_argument&){invalid_rate=true;}check(invalid_rate,"invalid rate rejected");
    for(unsigned seed=500;seed<520;++seed){auto s=discover_waveform_structure(noise(seed),rate);for(auto& r:s.ofdm)check(!r.pattern_consistent,"fresh white noise CP rejection");for(auto& r:s.spread)check(!r.pattern_consistent,"fresh white noise short-code rejection");}
    auto wrong=discover_waveform_structure(barker(8,350000,3,true),rate);for(auto& r:wrong.spread)check(!r.pattern_consistent,"different short code rejected");
    check(discover_waveform_structure(IQ(65536),rate).status=="no_variation","zero abstains");
    check(discover_waveform_structure(IQ(2000),rate).status=="insufficient_samples","short abstains");
    for(auto x:{IQ(65537),IQ(3000,{NAN,0})}){bool rejected=false;try{discover_waveform_structure(x,rate);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"oversized/nonfinite rejected");}
}
}
int main(){tests();return failures?1:0;}
