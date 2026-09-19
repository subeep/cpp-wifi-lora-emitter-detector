#include "wifi_ofdm_rx.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <kissfft/kiss_fft.h>
namespace rfmon::wifi {
namespace {
using C=std::complex<float>;
constexpr double pi=3.14159265358979323846;
// IEEE 802.11 legacy L-LTF, subcarriers -26..26. Same standard sequence
// as the existing Wi-Fi fingerprint engine; no LoRa dependency.
constexpr int training[53]={1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,0,1,-1,-1,1,1,-1,1,-1,1,-1,-1,-1,-1,-1,1,1,-1,-1,1,-1,1,-1,1,1,1,1};
struct Rate { int code,mbps,bpsc,dbps; };
constexpr Rate rates[]={{11,6,1,24},{15,9,1,36},{10,12,2,48},{14,18,2,72},{9,24,4,96},{13,36,4,144},{8,48,6,192},{12,54,6,216}};
int parity(unsigned v){return __builtin_parity(v);}
// With newest input in bit 0, the K=7 (133,171 octal) taps are
// bit-reversed to (155,117). Using un-reversed taps passes a circular
// loopback but fails over the air.
// Soft metrics are positive for a transmitted 1. Erasures have zero metric.
std::vector<uint8_t> viterbi(const std::vector<float>& llr,size_t bits,bool terminated) {
    std::array<float,64> cost,next; cost.fill(1e30f);cost[0]=0;
    std::vector<std::array<uint8_t,64>> prev(bits);
    for(size_t t=0;t<bits;++t){
        next.fill(1e30f);
        for(unsigned s=0;s<64;++s) for(unsigned b=0;b<2;++b){
            unsigned reg=(s<<1)|b,ns=reg&63;
            float metric=cost[s]-(parity(reg&0155)?1.f:-1.f)*llr[2*t]-(parity(reg&0117)?1.f:-1.f)*llr[2*t+1];
            if(metric<next[ns]){next[ns]=metric;prev[t][ns]=uint8_t(s);}
        }
        cost=next;
    }
    unsigned state=terminated?0:unsigned(std::min_element(cost.begin(),cost.end())-cost.begin());
    std::vector<uint8_t> out(bits);
    for(size_t t=bits;t-->0;){out[t]=state&1;state=prev[t][state];}
    return out;
}
std::vector<float> deinterleave(const std::vector<float>& in,int bpsc){
    int count=int(in.size()),s=std::max(bpsc/2,1);std::vector<float> out(count);
    for(int k=0;k<count;++k){int i=(count/16)*(k%16)+k/16;int j=s*(i/s)+(i+count-(16*i)/count)%s;out[k]=in[j];}
    return out;
}
std::array<C,64> fft(const std::vector<C>& x,size_t start,kiss_fft_cfg cfg){
    kiss_fft_cpx a[64],b[64];for(int i=0;i<64;++i){a[i].r=x[start+i].real();a[i].i=x[start+i].imag();}kiss_fft(cfg,a,b);
    std::array<C,64> out;for(int i=0;i<64;++i)out[i]={b[i].r,b[i].i};return out;
}
// IEEE Gray labels: sign bit followed by magnitude bits, I then Q.
float axis(unsigned bits,int width){
    float magnitude=1;
    if(width==2)magnitude=(bits&2)?1:3;
    if(width==3){const int levels[]={7,1,5,3};magnitude=float(levels[(bits>>1)&3]);}
    return (bits&1)?magnitude:-magnitude;
}
std::vector<float> demap(const std::array<C,64>& symbols,const std::array<C,64>& channel,int bpsc){
    std::vector<float> out;out.reserve(48*bpsc);
    int width=bpsc==1?1:bpsc/2;float scale=std::sqrt(bpsc==1?1.f:bpsc==2?2.f:bpsc==4?10.f:42.f);
    for(int k=-26;k<=26;++k){
        if(k==0||std::abs(k)==7||std::abs(k)==21)continue;
        int bin=(k+64)%64;C y=symbols[bin];
        for(int bit=0;bit<bpsc;++bit){
            float d0=1e30f,d1=1e30f;
            for(unsigned v=0;v<(1u<<bpsc);++v){
                C point(axis(v,width)/scale,bpsc==1?0:axis(v>>width,width)/scale);
                float d=std::norm(y-point);if((v>>bit)&1)d1=std::min(d1,d);else d0=std::min(d0,d);
            }
            out.push_back((d0-d1)*std::norm(channel[bin]));
        }
    }
    return deinterleave(out,bpsc);
}
// Integer-period pilot sequence starts at L-SIG; transmitter scrambler and
// pilot PRBS share the polynomial but have independent state/initialization.
int pilot_polarity(size_t symbol){unsigned state=127;unsigned bit=0;for(size_t i=0;i<=symbol%127;++i){bit=((state>>6)^(state>>3))&1;state=((state<<1)|bit)&127;}return bit?-1:1;}
std::array<C,64> equalize(const std::array<C,64>& raw,const std::array<C,64>& h,size_t symbol){
    std::array<C,64> y{};
    for(int k=0;k<64;++k)if(std::norm(h[k])>1e-18f)y[k]=raw[k]/h[k];
    constexpr int pilots[]={-21,-7,7,21};
    float phases[4];int polarity=pilot_polarity(symbol);
    for(int i=0;i<4;++i){phases[i]=std::arg(y[(pilots[i]+64)%64]*float((i==3?-1:1)*polarity));if(i){while(phases[i]-phases[i-1]>pi)phases[i]-=2*pi;while(phases[i]-phases[i-1]<-pi)phases[i]+=2*pi;}}
    float intercept=0,slope=0;for(int i=0;i<4;++i){intercept+=phases[i]/4;slope+=pilots[i]*phases[i]/980.f;}
    for(int k=-26;k<=26;++k)y[(k+64)%64]*=std::polar(1.f,-intercept-slope*k);
    return y;
}
// Mix before resampling. Windowed-sinc anti-alias filtering is needed when
// reducing a B210's 56 Msps to 20 Msps; linear interpolation is insufficient.
std::vector<C> baseband(const C* x,size_t n,double rate,double offset){
    std::vector<C> mixed(n);C dc{};for(size_t i=0;i<n;++i)dc+=x[i];dc/=float(n);
    for(size_t i=0;i<n;++i)mixed[i]=(x[i]-dc)*std::polar(1.f,float(-2*pi*offset*double(i)/rate));
    if(std::abs(rate-20e6)<1)return mixed;
    size_t count=size_t(double(n)*20e6/rate);std::vector<C> out(count);
    double cutoff=0.5*std::min(1.0,20e6/rate);int radius=int(std::ceil(24*std::max(1.0,rate/20e6)));
    for(size_t i=0;i<count;++i){double t=double(i)*rate/20e6;long center=long(t);double norm=0;C sum{};
        for(long j=center-radius;j<=center+radius;++j){if(j<0||size_t(j)>=n)continue;double d=t-j;double sinc=std::abs(d)<1e-12?2*cutoff:std::sin(2*pi*cutoff*d)/(pi*d);double w=sinc*(0.5+0.5*std::cos(pi*d/(radius+1)));sum+=mixed[size_t(j)]*float(w);norm+=w;}
        if(std::abs(norm)>1e-8)out[i]=sum/float(norm);
    }return out;
}
}
OfdmDecodeResult decode_ofdm_burst(const C* input,size_t n,double rate,double center,double channel){
    OfdmDecodeResult result;
    if(!input||n<400||!std::isfinite(rate)||rate<20e6||rate>64e6||!std::isfinite(center)||!std::isfinite(channel)||n>size_t(rate*0.006)) {result.status="Unsupported rate or capture length";return result;}
    for(size_t i=0;i<n;++i)if(!std::isfinite(input[i].real())||!std::isfinite(input[i].imag())){result.status="Non-finite IQ";return result;}
    auto x=baseband(input,n,rate,channel-center);
    size_t stf=0,run=0;C phase{};bool found=false;
    for(size_t i=0;i+64<x.size()&&i<2048;++i){C p{};double a=0,b=0;for(size_t j=0;j<48;++j){p+=std::conj(x[i+j])*x[i+j+16];a+=std::norm(x[i+j]);b+=std::norm(x[i+j+16]);}
        double corr=std::norm(p)/(a*b+1e-30);if(corr>.65){if(!run){stf=i;phase=p;}if(++run>=32){found=true;break;}}else run=0;
    }
    if(!found)return result;
    double omega=std::arg(phase)/16.;
    for(size_t i=0;i<x.size();++i)x[i]*=std::polar(1.f,float(-omega*i));
    static const auto ref=[](){std::array<C,64> r{};for(int i=0;i<64;++i)for(int k=-26;k<=26;++k)r[i]+=float(training[k+26])*std::polar(1.f,float(2*pi*k*i/64))/8.f;return r;}();
    double refpower=0;for(auto v:ref)refpower+=std::norm(v);
    size_t ltf=0;double best=0;
    for(size_t i=stf+64;i<stf+320&&i+128<x.size();++i){C a{},b{};double pa=0,pb=0;for(size_t j=0;j<64;++j){a+=x[i+j]*std::conj(ref[j]);b+=x[i+j+64]*std::conj(ref[j]);pa+=std::norm(x[i+j]);pb+=std::norm(x[i+j+64]);}
        double score=std::min(std::norm(a)/(refpower*pa+1e-30),std::norm(b)/(refpower*pb+1e-30));if(score>best){best=score;ltf=i;}}
    result.ltf_correlation=best;if(best<.18)return result;
    result.preamble_found=true;result.ltf_start=ltf;
    C fine{};for(size_t i=0;i<64;++i)fine+=std::conj(x[ltf+i])*x[ltf+i+64];
    double residual=std::arg(fine)/64.;for(size_t i=0;i<x.size();++i)x[i]*=std::polar(1.f,float(-residual*i));
    result.cfo_hz=(omega+residual)*20e6/(2*pi);
    struct Fft {kiss_fft_cfg cfg=kiss_fft_alloc(64,0,nullptr,nullptr);~Fft(){kiss_fft_free(cfg);}} plan;
    if(!plan.cfg){result.status="FFT allocation failed";return result;}
    auto a=fft(x,ltf,plan.cfg),b=fft(x,ltf+64,plan.cfg);std::array<C,64> h{};
    for(int k=-26;k<=26;++k)if(k)h[(k+64)%64]=(a[(k+64)%64]+b[(k+64)%64])/(2.f*training[k+26]);
    size_t sig=ltf+144;if(sig+64>x.size()){result.status="Incomplete L-SIG";return result;}
    auto equal=equalize(fft(x,sig,plan.cfg),h,0);
    auto bits=viterbi(demap(equal,h,1),24,true);
    int code=0,length=0,check=0;for(int i=0;i<18;++i)check^=bits[i];for(int i=0;i<4;++i)code|=bits[i]<<i;for(int i=0;i<12;++i)length|=bits[5+i]<<i;
    const Rate* mode=nullptr;for(const auto& r:rates)if(r.code==code)mode=&r;
    bool tail=true;for(int i=18;i<24;++i)if(bits[i])tail=false;
    if(check||bits[4]||!tail||!mode||length<4){result.status="Invalid L-SIG";return result;}
    result.header_valid=true;result.rate_mbps=mode->mbps;result.psdu_length=size_t(length);
    size_t symbols=(16+8*size_t(length)+6+mode->dbps-1)/mode->dbps;
    size_t end=sig+80*symbols+64;result.samples_required=size_t(std::ceil(double(end)*rate/20e6));
    if(end>x.size()){result.status="Incomplete OFDM payload";return result;}
    std::vector<float> coded;coded.reserve(symbols*48*mode->bpsc);
    for(size_t s=0;s<symbols;++s){auto soft=demap(equalize(fft(x,sig+80*(s+1),plan.cfg),h,s+1),h,mode->bpsc);coded.insert(coded.end(),soft.begin(),soft.end());}
    // Puncturing patterns apply to serialized A/B encoder outputs.
    std::vector<int> mask={1,1};if(mode->dbps*2==48*mode->bpsc*3/2)mask={1,1,1,0,0,1};
    if(mode->mbps==48)mask={1,1,1,0};
    size_t nbits=symbols*mode->dbps;std::vector<float> mother(2*nbits,0);size_t pos=0;
    for(size_t i=0;i<mother.size();++i)if(mask[i%mask.size()]){if(pos>=coded.size()){result.status="Invalid coded length";return result;}mother[i]=coded[pos++];}
    // The encoder terminates BEFORE padding; decode only through the six tail bits.
    size_t meaningful=16+8*size_t(length);auto data=viterbi(mother,meaningful+6,true);
    unsigned seed=0;
    for(unsigned trial=1;trial<128;++trial){unsigned state=trial;bool match=true;for(size_t i=0;i<7;++i){unsigned v=((state>>6)^(state>>3))&1;if(v!=data[i])match=false;state=((state<<1)|v)&127;}if(match){seed=trial;break;}}
    if(!seed){result.status="Invalid scrambler seed";return result;}
    unsigned state=seed;for(size_t i=0;i<meaningful;++i){unsigned v=((state>>6)^(state>>3))&1;data[i]^=v;state=((state<<1)|v)&127;}
    for(int i=0;i<16;++i)if(data[i]){result.status="Unsupported format or invalid SERVICE";return result;}
    result.mpdu.assign(length,0);for(size_t i=0;i<8*size_t(length);++i)result.mpdu[i/8]|=data[16+i]<<(i%8);
    size_t last=result.mpdu.size()-4;uint32_t fcs=0;for(int i=0;i<4;++i)fcs|=uint32_t(result.mpdu[last+i])<<(8*i);
    result.fcs_valid=fcs32(result.mpdu.data(),last)==fcs;
    if(!result.fcs_valid){result.status="FCS failed (or unsupported PHY format)";return result;}
    auto beacon=parse_beacon(result.mpdu.data(),result.mpdu.size());if(beacon&&beacon->fcs_valid)result.beacon=std::move(beacon);
    result.status=result.beacon?"Decoded OFDM beacon/probe response":"FCS-valid non-beacon frame";
    return result;
}
}
