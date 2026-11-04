// 802.11n/ac payload decoding (20 MHz, one spatial stream, BCC or LDPC,
// with and without STBC) and 802.11ax HE SU (20 MHz, one stream, 2x HE-LTF)
// against PPDUs from a small transmitter written from the standard (LDPC
// parameters and encoder: src/wifi_ldpc.cpp). The receiver itself was validated on live traffic: FCS-valid
// MPDUs from real 802.11n/ac devices (docs/WIFI_HT_VHT_PAYLOADS_2026-11-04.md).
// Those captures are client traffic, so they are not committed; these
// synthetic PPDUs keep every decode path (MCS 0-8, short GI, STBC, A-MPDU,
// the 802.11ac symbol count) from regressing.
#include "wifi_ofdm_rx.hpp"
#include "wifi_ldpc.hpp"
#include <kissfft/kiss_fft.h>
#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <random>
using namespace rfmon;
namespace {
using C=std::complex<float>;
constexpr double pi=3.14159265358979323846;
constexpr int lltf[53]={1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,0,1,-1,-1,1,1,-1,1,-1,1,-1,-1,-1,-1,-1,1,1,-1,-1,1,-1,1,-1,1,1,1,1};
struct Mcs { int bpsc,num,den; };
constexpr Mcs mcs_table[12]={{1,1,2},{2,1,2},{2,3,4},{4,1,2},{4,3,4},{6,2,3},{6,3,4},{6,5,6},{8,3,4},{8,5,6},{10,3,4},{10,5,6}};
using Bits=std::vector<uint8_t>;

int parity(unsigned v){return __builtin_parity(v);}
// K=7 (133,171); newest bit in bit 0, so the taps read bit-reversed.
Bits encode(const Bits& in){Bits out;unsigned s=0;for(auto b:in){unsigned r=(s<<1)|b;out.push_back(uint8_t(parity(r&0155)));out.push_back(uint8_t(parity(r&0117)));s=r&63;}return out;}
Bits puncture(const Bits& in,int num,int den){
    std::vector<int> mask={1,1};
    if(num==2&&den==3)mask={1,1,1,0};
    if(num==3&&den==4)mask={1,1,1,0,0,1};
    if(num==5&&den==6)mask={1,1,1,0,0,1,1,0,0,1};
    Bits out;for(size_t i=0;i<in.size();++i)if(mask[i%mask.size()])out.push_back(in[i]);return out;
}
Bits interleave(const Bits& in,int bpsc,int columns){
    const int n=int(in.size()),s=std::max(bpsc/2,1);Bits out(in.size());
    for(int k=0;k<n;++k){int i=(n/columns)*(k%columns)+k/columns;int j=s*(i/s)+(i+n-(columns*i)/n)%s;out[size_t(j)]=in[size_t(k)];}
    return out;
}
unsigned crc8(const Bits& b,size_t n){unsigned c=0xFF;for(size_t i=0;i<n;++i){unsigned fb=((c>>7)&1)^b[i];c=(c<<1)&0xFF;if(fb)c^=0x07;}return (~c)&0xFF;}
void put(Bits& b,unsigned v,int n){for(int i=0;i<n;++i)b.push_back(uint8_t((v>>i)&1));}
void put_crc(Bits& b){const unsigned c=crc8(b,b.size());for(int i=7;i>=0;--i)b.push_back(uint8_t((c>>i)&1));}
int polarity(size_t n){unsigned st=127,bit=0;for(size_t i=0;i<=n%127;++i){bit=((st>>6)^(st>>3))&1;st=((st<<1)|bit)&127;}return bit?-1:1;}
float level(unsigned bits,int width){
    float m=1;
    if(width==2)m=(bits&2)?1:3;
    if(width==3){const int l[]={7,1,5,3};m=float(l[(bits>>1)&3]);}
    if(width==4){const int l[]={15,1,9,7,13,3,11,5};m=float(l[(bits>>1)&7]);}
    if(width==5){  // 1024-QAM: binary-reflected Gray code, b0 the most significant
        unsigned g=0;for(int j=0;j<5;++j)g|=((bits>>j)&1u)<<(4-j);
        unsigned i=g;for(unsigned t=g>>1;t;t>>=1)i^=t;return float(2*int(i)-31);
    }
    return (bits&1)?m:-m;
}
C map(const uint8_t* b,int bpsc){
    if(bpsc==1)return {b[0]?1.f:-1.f,0};
    const int w=bpsc/2;unsigned i=0,q=0;for(int k=0;k<w;++k){i|=unsigned(b[k])<<k;q|=unsigned(b[w+k])<<k;}
    const float scale=std::sqrt(bpsc==2?2.f:bpsc==4?10.f:bpsc==6?42.f:bpsc==8?170.f:682.f);
    return {level(i,w)/scale,level(q,w)/scale};
}
using Freq=std::array<C,64>;
C& at(Freq& f,int k){return f[size_t((k+64)%64)];}
C at(const Freq& f,int k){return f[size_t((k+64)%64)];}
std::vector<C> ofdm(const Freq& f,int gi){
    static kiss_fft_cfg cfg=kiss_fft_alloc(64,1,nullptr,nullptr);
    kiss_fft_cpx a[64],b[64];for(int i=0;i<64;++i){a[i].r=f[size_t(i)].real();a[i].i=f[size_t(i)].imag();}kiss_fft(cfg,a,b);
    std::vector<C> t;for(int i=64-gi;i<64;++i)t.push_back({b[i].r/8,b[i].i/8});for(int i=0;i<64;++i)t.push_back({b[i].r/8,b[i].i/8});
    return t;
}
void append(std::vector<C>& x,const std::vector<C>& s){x.insert(x.end(),s.begin(),s.end());}
// Legacy-format BPSK symbol (L-SIG, HT-SIG, VHT-SIG-A): 48 subcarriers;
// `quadrature` puts bit 1 on +j (QBPSK).
Freq legacy_symbol(const Bits& coded,size_t pilot_index,bool quadrature){
    const Bits b=interleave(coded,1,16);Freq f{};size_t i=0;
    for(int k=-26;k<=26;++k){if(!k||std::abs(k)==7||std::abs(k)==21)continue;const float v=b[i++]?1.f:-1.f;at(f,k)=quadrature?C(0,v):C(v,0);}
    const int p=polarity(pilot_index);at(f,-21)=float(p);at(f,-7)=float(p);at(f,7)=float(p);at(f,21)=float(-p);
    return f;
}
bool ht_data(int k){return k&&std::abs(k)!=7&&std::abs(k)!=21;}
float htltf(int k){return k<-26?1.f:k>26?-1.f:float(lltf[k+26]);}

struct Ppdu {
    bool vht=false,stbc=false,sgi=false,aggregate=false,ldpc=false,wide_flag=false;
    int mcs=0;
    std::vector<Bits> mpdus;  // MPDU bytes, FCS included
};
Bits fcs_frame(size_t n,uint8_t tag){
    // QoS data, To DS, locally administered test addresses, then FCS.
    Bits m={0x88,0x01,0x30,0x00, 0x02,0,0,0,0,0x01, 0x02,0,0,0,0,tag, 0x02,0,0,0,0,0x01, 0x10,0x00, 0x00,0x00};
    for(size_t i=m.size();i+4<n;++i)m.push_back(uint8_t(i*31+tag));
    const uint32_t f=wifi::fcs32(m.data(),m.size());for(int i=0;i<4;++i)m.push_back(uint8_t(f>>(8*i)));
    return m;
}
uint8_t reverse8(unsigned v){uint8_t r=0;for(int i=0;i<8;++i)r|=uint8_t(((v>>i)&1)<<(7-i));return r;}
void delimiter(Bits& out,size_t length,bool vht,bool eof){
    Bits d;d.push_back(uint8_t(eof|(vht?((length>>12)&3)<<2:0)|((length&15)<<4)));d.push_back(uint8_t((length>>4)&0xFF));
    Bits bits;for(int i=0;i<16;++i)bits.push_back(uint8_t((d[size_t(i/8)]>>(i%8))&1));
    d.push_back(reverse8(crc8(bits,16)));d.push_back(0x4E);out.insert(out.end(),d.begin(),d.end());
}

// Builds the PPDU, sends it through a two-tap channel with CFO and noise.
std::vector<C> legacy_preamble(){
    std::vector<C> x(300);
    Freq f{};const int s[53]={0,0,1,0,0,0,-1,0,0,0,1,0,0,0,-1,0,0,0,-1,0,0,0,1,0,0,0,0,0,0,0,-1,0,0,0,-1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0};
    for(int k=-26;k<=26;++k)at(f,k)=float(s[k+26])*C(1,1)*std::sqrt(13.f/6);
    auto t=ofdm(f,0);for(int i=0;i<160;++i)x.push_back(t[size_t(i%64)]);
    Freq l{};for(int k=-26;k<=26;++k)at(l,k)=float(lltf[k+26]);
    auto lt=ofdm(l,0);for(int i=32;i<64;++i)x.push_back(lt[size_t(i)]);append(x,lt);append(x,lt);
    return x;
}
// Two-tap channel, 23 kHz CFO, white noise.
std::vector<C> impair(std::vector<C> x,double snr_db,uint32_t seed){
    x.resize(x.size()+300);
    std::vector<C> y(x.size());
    for(size_t i=0;i<x.size();++i)y[i]=x[i]*std::polar(0.9f,0.4f)+(i>=3?x[i-3]*std::polar(0.25f,-1.3f):C{});
    double power=0;for(auto v:y)power+=std::norm(v);power/=double(y.size());
    std::mt19937 rng(seed);std::normal_distribution<float> noise(0,float(std::sqrt(power/2*std::pow(10,-snr_db/10))));
    for(size_t i=0;i<y.size();++i)y[i]=y[i]*std::polar(1.f,float(2*pi*23e3*double(i)/20e6))+C(noise(rng),noise(rng));
    return y;
}
std::vector<C> transmit(const Ppdu& p,double snr_db,uint32_t seed,wifi::LdpcPlan* used=nullptr){
    const Mcs m=mcs_table[p.mcs];
    const int cbps=52*m.bpsc,dbps=cbps*m.num/m.den,nsts=p.stbc?2:1,nltf=nsts;
    const size_t mstbc=p.stbc?2:1;
    Bits psdu;
    for(size_t i=0;i<p.mpdus.size();++i){
        if(!p.vht&&!p.aggregate){psdu=p.mpdus[i];break;}
        delimiter(psdu,p.mpdus[i].size(),p.vht,p.vht&&p.mpdus.size()==1);
        psdu.insert(psdu.end(),p.mpdus[i].begin(),p.mpdus[i].end());
        if(p.vht||i+1<p.mpdus.size())while(psdu.size()%4)psdu.push_back(0);
    }
    const size_t apep=psdu.size(),tail=p.ldpc?0:6;
    // BCC: symbols for SERVICE, PSDU and tail. LDPC: the encoding parameters
    // decide (802.11ac: payload fills the initial symbols, maybe one extra).
    size_t nsym=mstbc*((8*apep+16+tail+mstbc*size_t(dbps)-1)/(mstbc*size_t(dbps)));
    std::optional<wifi::LdpcPlan> plan;
    if(p.ldpc){
        const long pld=p.vht?long(nsym)*dbps:16+8*long(apep),per=long(dbps)*long(mstbc);
        plan=wifi::ldpc_plan(pld,long(cbps)*long(mstbc)*((pld+per-1)/per),m.num,m.den,cbps,int(mstbc),std::nullopt);
        nsym=size_t(plan->coded_bits/cbps);
        if(used)*used=*plan;
    }
    const size_t payload=p.ldpc?size_t(plan->payload_bits):nsym*size_t(dbps);
    if(p.vht){  // EOF padding fills the last symbol
        const size_t full=(payload-16-tail)/8;
        while(psdu.size()+4<=full)delimiter(psdu,0,true,true);
        while(psdu.size()<full)psdu.push_back(0);
    }
    // Data bits: SERVICE, PSDU, then tail and pad (802.11n) or pad and tail
    // (802.11ac); LDPC has no tail.
    Bits data(16,0);for(auto byte:psdu)put(data,byte,8);
    const size_t tail_at=p.vht?payload-tail:data.size();
    data.resize(payload,0);
    unsigned st=0x5D;for(auto& b:data){unsigned v=((st>>6)^(st>>3))&1;b^=uint8_t(v);st=((st<<1)|v)&127;}
    for(size_t i=0;i<tail;++i)data[tail_at+i]=0;
    const Bits coded=p.ldpc?wifi::ldpc_encode(data,*plan):puncture(encode(data),m.num,m.den);
    // Header fields.
    Bits sig;
    if(!p.vht){
        put(sig,unsigned(p.mcs),7);put(sig,p.wide_flag,1);put(sig,unsigned(apep),16);
        put(sig,1,1);put(sig,1,1);put(sig,1,1);put(sig,p.aggregate,1);put(sig,p.stbc,2);put(sig,p.ldpc,1);put(sig,p.sgi,1);put(sig,0,2);
    }else{
        put(sig,p.wide_flag?1:0,2);put(sig,1,1);put(sig,p.stbc,1);put(sig,63,6);put(sig,unsigned(nsts-1),3);put(sig,0,9);put(sig,1,1);put(sig,1,1);
        put(sig,p.sgi,1);put(sig,p.sgi&&nsym%10==9,1);put(sig,p.ldpc,1);put(sig,plan&&plan->extra_symbol,1);put(sig,unsigned(p.mcs),4);put(sig,0,1);put(sig,1,1);
    }
    put_crc(sig);put(sig,0,6);
    const Bits sig_coded=encode(sig);
    // L-LENGTH from the PPDU duration after the L-SIG.
    const size_t data_us=p.sgi?4*((9*nsym+9)/10):4*nsym;
    const size_t txtime=20+8+4+4*size_t(nltf)+(p.vht?4:0)+data_us;
    const unsigned llength=unsigned((txtime-20+3)/4*3-3);
    Bits lsig;put(lsig,11,4);put(lsig,0,1);put(lsig,llength,12);int par=0;for(auto b:lsig)par^=b;put(lsig,unsigned(par),1);put(lsig,0,6);

    std::vector<C> x=legacy_preamble();
    append(x,ofdm(legacy_symbol(encode(lsig),0,false),16));
    for(int s=0;s<2;++s){
        const Bits half(sig_coded.begin()+48*s,sig_coded.begin()+48*(s+1));
        append(x,ofdm(legacy_symbol(half,size_t(1+s),p.vht?s==1:true),16));
    }
    // From here on two space-time streams with STBC: the second through a
    // different gain and the -400 ns cyclic shift.
    const C g1=std::polar(0.7f,1.1f);
    auto combine=[&](const Freq& s0,const Freq& s1){Freq f{};for(int k=-28;k<=28;++k)at(f,k)=at(s0,k)+(nsts==2?g1*at(s1,k)*std::polar(1.f,float(2*pi*k*8/64)):C{});return f;};
    {Freq f{};for(int k=-24;k<=24;k+=4)if(k)at(f,k)=C(1,1);append(x,ofdm(f,16));}  // HT/VHT-STF (not used by the receiver)
    const int P[2][2]={{1,-1},{1,1}};
    for(int n=0;n<nltf;++n){Freq a{},b{};for(int k=-28;k<=28;++k)if(k){at(a,k)=float(P[0][n])*htltf(k);at(b,k)=float(P[1][n])*htltf(k);}append(x,ofdm(combine(a,b),16));}
    if(p.vht){Bits sigb;put(sigb,unsigned((apep+3)/4),17);put(sigb,7,3);put(sigb,0,6);const Bits b=interleave(encode(sigb),1,13);
        Freq f{};size_t i=0;for(int k=-28;k<=28;++k)if(ht_data(k))at(f,k)=b[i++]?1.f:-1.f;
        const int pol=polarity(3);at(f,-21)=float(pol);at(f,-7)=float(pol);at(f,7)=float(pol);at(f,21)=float(-pol);
        append(x,ofdm(combine(f,f),16));}
    static const int one[4]={1,1,1,-1},two[2][4]={{1,1,-1,-1},{1,-1,-1,1}};
    std::vector<std::array<C,52>> symbols(nsym);
    // BCC: interleaver. LDPC: subcarrier order (802.11n) or the tone mapper,
    // point k on data subcarrier 4 (k mod 13) + floor(k / 13) (802.11ac).
    for(size_t n=0;n<nsym;++n){const Bits chunk(coded.begin()+long(n)*cbps,coded.begin()+long(n+1)*cbps);
        if(!p.ldpc){const Bits b=interleave(chunk,m.bpsc,13);for(size_t i=0;i<52;++i)symbols[n][i]=map(&b[i*size_t(m.bpsc)],m.bpsc);}
        else for(size_t k=0;k<52;++k)symbols[n][p.vht?4*(k%13)+k/13:k]=map(&chunk[k*size_t(m.bpsc)],m.bpsc);}
    for(size_t n=0;n<nsym;++n){
        Freq s[2]{};size_t i=0;
        for(int k=-28;k<=28;++k){
            if(!ht_data(k))continue;
            if(nsts==1)at(s[0],k)=symbols[n][i];
            else if(n%2==0){at(s[0],k)=symbols[n][i];at(s[1],k)=-std::conj(symbols[n+1][i]);}
            else{at(s[0],k)=symbols[n][i];at(s[1],k)=std::conj(symbols[n-1][i]);}
            ++i;
        }
        const int pol=polarity(n+(p.vht?4:3));const int pk[4]={-21,-7,7,21};
        for(int t=0;t<nsts;++t)for(int j=0;j<4;++j)at(s[t],pk[j])=float(pol*(p.vht||nsts==1?one[(n+size_t(j))%4]:two[t][(n+size_t(j))%4]));
        append(x,ofdm(combine(s[0],s[1]),p.sgi?8:16));
    }
    return impair(x,snr_db,seed);
}

// --- 802.11ax HE SU: 20 MHz, one stream, 2x HE-LTF, BCC or LDPC.
// Copy of the receiver's 2x HE-LTF (src/wifi_ofdm_rx.cpp).
constexpr int8_t heltf2x[123]={
    -1,-1,-1,1,1,-1,1,-1,-1,-1,-1,1,-1,1,-1,-1,1,1,-1,1,1,1,1,1,-1,1,-1,1,-1,-1,1,
    1,-1,1,-1,-1,-1,-1,1,-1,1,1,1,-1,-1,1,-1,-1,-1,-1,-1,1,-1,-1,-1,1,1,1,-1,-1,1,0,
    1,-1,1,1,-1,1,1,-1,1,1,-1,-1,1,-1,1,1,1,1,-1,1,-1,1,1,-1,-1,1,-1,-1,-1,-1,-1,
    1,-1,1,1,-1,-1,1,1,-1,1,-1,-1,-1,-1,1,-1,1,1,1,-1,-1,1,-1,-1,-1,-1,-1,1,-1,1};

struct HePpdu { int mcs=0; bool ldpc=false; int gi=16; int pe_samples=0; std::vector<Bits> mpdus; bool dcm=false,stbc=false; int gi_ltf_field=-1; };
std::vector<C> ofdm256(const std::array<C,256>& f,int gi){
    static kiss_fft_cfg cfg=kiss_fft_alloc(256,1,nullptr,nullptr);
    kiss_fft_cpx a[256],b[256];for(int i=0;i<256;++i){a[i].r=f[size_t(i)].real();a[i].i=f[size_t(i)].imag();}kiss_fft(cfg,a,b);
    std::vector<C> t;for(int i=256-gi;i<256;++i)t.push_back({b[i].r/16,b[i].i/16});for(int i=0;i<256;++i)t.push_back({b[i].r/16,b[i].i/16});
    return t;
}
bool he_data(int k){const int a=std::abs(k);return a>=2&&a<=122&&a!=22&&a!=48&&a!=90&&a!=116;}
// Builds the PPDU (padding per the a-factor rules of 27.3.12) and returns it
// through the same channel as transmit(); `info` reports a, N_SYM and the
// LDPC extra segment for the checks.
struct HeInfo { int a=0; long nsym=0; bool extra=false,pe_disambiguity=false; };
std::vector<C> transmit_he(const HePpdu& p,double snr_db,uint32_t seed,HeInfo* info=nullptr){
    const Mcs m=mcs_table[p.mcs];
    const long cbps=234L*m.bpsc,dbps=cbps*m.num/m.den,cbps_short=60L*m.bpsc,dbps_short=cbps_short*m.num/m.den;
    auto cbps_last=[&](int a){return a<4?a*cbps_short:cbps;};
    auto dbps_last=[&](int a){return a<4?a*dbps_short:dbps;};
    Bits psdu;
    for(const auto& mpdu:p.mpdus){delimiter(psdu,mpdu.size(),true,p.mpdus.size()==1);psdu.insert(psdu.end(),mpdu.begin(),mpdu.end());while(psdu.size()%4)psdu.push_back(0);}
    const long apep=long(psdu.size()),tail=p.ldpc?0:6,bits=8*apep+16+tail;
    long nsym=(bits+dbps-1)/dbps;const long excess=bits%dbps;
    int a=excess==0?4:int(std::min<long>((excess+dbps_short-1)/dbps_short,4));
    long payload=(nsym-1)*dbps+dbps_last(a);
    std::optional<wifi::LdpcPlan> plan;bool extra=false;
    if(p.ldpc){
        const long avbits_init=(nsym-1)*cbps+cbps_last(a);
        extra=wifi::ldpc_plan(payload,avbits_init,m.num,m.den,int(cbps),1,std::nullopt).extra_symbol;
        long nsym_f=nsym;int a_f=a;
        if(extra){if(a==4){nsym_f=nsym+1;a_f=1;}else a_f=a+1;}
        plan=wifi::ldpc_plan_he(payload,avbits_init,(nsym_f-1)*cbps+cbps_last(a_f),m.num,m.den);
        nsym=nsym_f;a=a_f;
    }
    // EOF padding, then pre-FEC padding bits; BCC tail at the very end.
    const long full=(payload-16-tail)/8;
    while(long(psdu.size())+4<=full)delimiter(psdu,0,true,true);
    while(long(psdu.size())<full)psdu.push_back(0);
    Bits data(16,0);for(auto byte:psdu)put(data,byte,8);data.resize(size_t(payload),0);
    unsigned st=0x2B;for(auto& b:data){unsigned v=((st>>6)^(st>>3))&1;b^=uint8_t(v);st=((st<<1)|v)&127;}
    for(long i=0;i<tail;++i)data[size_t(payload-1-i)]=0;
    Bits coded=p.ldpc?wifi::ldpc_encode(data,*plan):puncture(encode(data),m.num,m.den);
    // Post-FEC padding fills the last symbol.
    std::mt19937 pad_rng(seed);
    while(long(coded.size())<nsym*cbps)coded.push_back(uint8_t(pad_rng()&1));
    // Durations (20 MHz samples) and L-LENGTH (m = 2 for HE SU).
    const long ltf=128+p.gi,sym=256+p.gi;
    const long after=80*3+80+ltf+nsym*sym+p.pe_samples;
    const long txtime_us=20+(after+79)/80*4;
    const long pe_disambiguity=(20*(txtime_us-20)-80*3-80-ltf)/sym-nsym;
    if(info)*info={a,nsym,extra,pe_disambiguity==1};
    const unsigned llength=unsigned((txtime_us-20)/4*3-3-2);
    Bits lsig;put(lsig,11,4);put(lsig,0,1);put(lsig,llength,12);int par=0;for(auto b:lsig)par^=b;put(lsig,unsigned(par),1);put(lsig,0,6);
    // HE-SIG-A (SU), 52 bits with CRC-4 (c7..c4 of the HT-SIG CRC).
    Bits sig;
    put(sig,1,1);put(sig,0,1);put(sig,1,1);put(sig,unsigned(p.mcs),4);put(sig,p.dcm,1);put(sig,33,6);put(sig,1,1);put(sig,0,4);put(sig,0,2);
    put(sig,unsigned(p.gi_ltf_field>=0?p.gi_ltf_field:p.gi==32?2:1),2);put(sig,p.stbc?1:0,3);
    put(sig,127,7);put(sig,p.ldpc,1);put(sig,extra,1);put(sig,p.stbc,1);put(sig,0,1);put(sig,unsigned(a%4),2);put(sig,unsigned(pe_disambiguity==1),1);put(sig,1,1);put(sig,0,1);
    {const unsigned c=crc8(sig,sig.size());for(int i=7;i>=4;--i)sig.push_back(uint8_t((c>>i)&1));}
    put(sig,0,6);
    const Bits sig_coded=encode(sig);
    std::vector<C> x=legacy_preamble();
    const Freq lsym=legacy_symbol(encode(lsig),0,false);
    append(x,ofdm(lsym,16));append(x,ofdm(lsym,16));  // L-SIG, RL-SIG
    for(int s=0;s<2;++s){
        const Bits b=interleave(Bits(sig_coded.begin()+52*s,sig_coded.begin()+52*(s+1)),1,13);Freq f{};size_t i=0;
        for(int k=-28;k<=28;++k)if(k&&std::abs(k)!=7&&std::abs(k)!=21)at(f,k)=b[i++]?1.f:-1.f;
        const int pol=polarity(size_t(2+s));at(f,-21)=float(pol);at(f,-7)=float(pol);at(f,7)=float(pol);at(f,21)=float(-pol);
        append(x,ofdm(f,16));
    }
    {Freq f{};for(int k=-24;k<=24;k+=4)if(k)at(f,k)=C(1,1);append(x,ofdm(f,16));}  // HE-STF (not used by the receiver)
    {std::array<C,256> f{};for(int k=-122;k<=122;k+=2)f[size_t((k+256)%256)]=std::sqrt(2.f)*float(heltf2x[(k+122)/2]);
     const auto t=ofdm256(f,0);for(long i=128-p.gi;i<128;++i)x.push_back(t[size_t(i)]);x.insert(x.end(),t.begin(),t.begin()+128);}
    for(long n=0;n<nsym;++n){
        const Bits chunk(coded.begin()+n*cbps,coded.begin()+(n+1)*cbps);std::array<C,256> f{};
        if(!p.ldpc){const Bits b=interleave(chunk,m.bpsc,26);size_t i=0;for(int k=-122;k<=122;++k)if(he_data(k)){f[size_t((k+256)%256)]=map(&b[i*size_t(m.bpsc)],m.bpsc);++i;}}
        else{std::vector<int> tones;for(int k=-122;k<=122;++k)if(he_data(k))tones.push_back(k);
             for(size_t k=0;k<234;++k)f[size_t((tones[9*(k%26)+k/26]+256)%256)]=map(&chunk[k*size_t(m.bpsc)],m.bpsc);}
        // Pilots: BPSK with an arbitrary per-symbol pattern (the receiver does not assume values).
        const int pk[8]={-116,-90,-48,-22,22,48,90,116};
        for(int j=0;j<8;++j)f[size_t((pk[j]+256)%256)]=float(polarity(size_t(n+j)));
        append(x,ofdm256(f,p.gi));
    }
    x.resize(x.size()+size_t(p.pe_samples));  // packet extension (no energy needed here)
    return impair(x,snr_db,seed);
}
}

int main(){
    int failures=0,checks=0;
    auto check=[&](bool ok,const std::string& name){++checks;if(!ok)++failures;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';};
    auto decode=[](const std::vector<C>& y){return wifi::decode_ofdm_burst(y.data(),y.size(),20e6,2412e6,2412e6);};
    auto same=[](const wifi::OfdmDecodeResult& r,const std::vector<Bits>& want){
        if(r.mpdus.size()!=want.size())return false;
        for(size_t i=0;i<want.size();++i)if(r.mpdus[i]!=want[i])return false;
        return r.fcs_valid&&r.mpdu==want.front();
    };
    const double rate_lgi[10]={6.5,13,19.5,26,39,52,58.5,65,78,0};
    uint32_t seed=1;
    // Every MCS, both formats, long GI, one MPDU (802.11ac always aggregates).
    for(int vht=0;vht<2;++vht)for(int mcs=0;mcs<=(vht?8:7);++mcs){
        Ppdu p;p.vht=vht;p.mcs=mcs;p.mpdus={fcs_frame(150+size_t(mcs)*7,uint8_t(mcs))};
        auto r=decode(transmit(p,32,seed++));
        const std::string name=std::string(vht?"802.11ac":"802.11n")+" MCS "+std::to_string(mcs);
        check(same(r,p.mpdus)&&r.sig&&r.sig->streams==1&&r.sig->space_time_streams==1&&std::abs(r.data_rate_mbps-rate_lgi[mcs])<0.01&&
              r.rate_mbps==int(std::lround(rate_lgi[mcs]))&&r.status.find(vht?"A-MPDU 1/1 MPDUs FCS-valid":"MPDU FCS-valid")!=std::string::npos,name+": "+r.status);
    }
    // Whole-frame RF features on verified frames: the channel adds a 23 kHz
    // carrier offset and no sampling-clock offset.
    {
        bool ok=true;std::string last;
        for(int vht=0;vht<2;++vht)for(int ldpc=0;ldpc<2;++ldpc)for(int mcs:{0,4,7}){
            Ppdu p;p.vht=vht;p.ldpc=ldpc;p.mcs=mcs;p.aggregate=true;p.mpdus={fcs_frame(600,uint8_t(mcs))};
            auto r=decode(transmit(p,32,seed++));
            const bool good=r.fcs_valid&&r.rf&&std::abs(r.rf->cfo_hz-23e3)<30&&std::abs(r.rf->cfo_early_hz-23e3)<60&&std::abs(r.rf->sfo_ppm)<3&&r.rf->snr_db>20;
            if(!good){ok=false;last=r.rf?("CFO "+std::to_string(r.rf->cfo_hz)+" Hz, SFO "+std::to_string(r.rf->sfo_ppm)+" ppm, SNR "+std::to_string(r.rf->snr_db)):"no features";}
        }
        check(ok,"whole-frame features: 23 kHz CFO recovered on 802.11n/ac BCC and LDPC "+last);
        Ppdu stbc;stbc.vht=true;stbc.stbc=true;stbc.mcs=4;stbc.aggregate=true;stbc.mpdus={fcs_frame(300,1)};
        auto r=decode(transmit(stbc,32,seed++));
        check(r.fcs_valid&&!r.rf,"whole-frame features: not claimed for STBC (not implemented)");
        Ppdu bad;bad.aggregate=true;bad.mcs=3;bad.mpdus={fcs_frame(200,7),fcs_frame(200,8)};bad.mpdus[1][40]^=0xFF;
        r=decode(transmit(bad,32,seed++));
        check(r.fcs_valid&&!r.rf,"whole-frame features: not fitted when an A-MPDU subframe failed its FCS");
    }
    // Short GI, A-MPDU of three.
    {Ppdu p;p.mcs=5;p.sgi=true;p.aggregate=true;p.mpdus={fcs_frame(300,1),fcs_frame(97,2),fcs_frame(1500,3)};
     auto r=decode(transmit(p,32,seed++));
     check(same(r,p.mpdus)&&r.ampdu_subframes==3&&std::abs(r.data_rate_mbps-52/0.9)<0.01&&r.status.find("A-MPDU 3/3")!=std::string::npos,"802.11n short GI A-MPDU: "+r.status);}
    // STBC (two space-time streams, Alamouti), with and without short GI.
    for(int vht=0;vht<2;++vht)for(int sgi=0;sgi<2;++sgi){
        Ppdu p;p.vht=vht;p.stbc=true;p.sgi=sgi;p.aggregate=true;p.mcs=vht?8:(sgi?7:2);p.mpdus={fcs_frame(220,4),fcs_frame(333,5)};
        auto r=decode(transmit(p,34,seed++));
        check(same(r,p.mpdus)&&r.sig&&r.sig->stbc&&r.sig->streams==1&&r.sig->space_time_streams==2,
              std::string(vht?"802.11ac":"802.11n")+" STBC"+(sgi?" short GI":"")+": "+r.status);
    }
    // 802.11ac short GI with the symbol-count disambiguation bit (N_SYM mod 10 == 9).
    {
        bool found=false;
        for(size_t n=100;n<400&&!found;++n){
            Ppdu p;p.vht=true;p.sgi=true;p.mcs=4;p.mpdus={fcs_frame(n,6)};
            const size_t apep=(n+4+3)/4*4,nsym=(8*apep+22+155)/156;  // MCS 4: 156 data bits per symbol
            if(nsym%10!=9)continue;
            found=true;auto r=decode(transmit(p,32,seed++));
            check(same(r,p.mpdus)&&r.sig&&r.sig->nsym_disambiguation,"802.11ac short GI, N_SYM "+std::to_string(nsym)+": "+r.status);
        }
        check(found,"disambiguation case constructed");
    }
    // A damaged subframe is skipped; the others are kept.
    {Ppdu p;p.aggregate=true;p.mcs=3;p.mpdus={fcs_frame(200,7),fcs_frame(200,8),fcs_frame(200,9)};p.mpdus[1][40]^=0xFF;
     auto r=decode(transmit(p,32,seed++));
     check(r.fcs_valid&&r.mpdus.size()==2&&r.mpdus[0]==p.mpdus[0]&&r.mpdus[1]==p.mpdus[2]&&r.ampdu_subframes==3&&r.status.find("A-MPDU 2/3")!=std::string::npos,"damaged subframe skipped: "+r.status);}
    // LDPC: every MCS in both formats (802.11ac MCS 9 at 20 MHz with 346 data
    // bits per symbol, as devices send it); three payload sizes per MCS cover
    // shortening, puncturing, repetition, extra symbols and several codewords.
    {
        int punctured=0,repeated=0,extra=0,multi=0;
        for(int vht=0;vht<2;++vht)for(int mcs=0;mcs<=(vht?9:7);++mcs){
            bool ok=true;std::string last;
            for(size_t n:{40,333,1700}){
                Ppdu p;p.vht=vht;p.ldpc=true;p.mcs=mcs;p.mpdus={fcs_frame(n,uint8_t(n))};wifi::LdpcPlan plan;
                auto r=decode(transmit(p,32,seed++,&plan));
                const double rate=(mcs==9?346:52*mcs_table[mcs].bpsc*mcs_table[mcs].num/mcs_table[mcs].den)/4.0;
                ok=ok&&same(r,p.mpdus)&&std::abs(r.data_rate_mbps-rate)<0.01;last=r.status;
                punctured+=plan.punctured>0;repeated+=plan.repeated>0;extra+=plan.extra_symbol;multi+=plan.codewords>1;
            }
            check(ok,std::string(vht?"802.11ac":"802.11n")+" LDPC MCS "+std::to_string(mcs)+": "+last);
        }
        check(punctured&&repeated&&extra&&multi,"LDPC cases include puncturing ("+std::to_string(punctured)+"), repetition ("+std::to_string(repeated)+
              "), extra symbols ("+std::to_string(extra)+") and several codewords ("+std::to_string(multi)+")");
    }
    // LDPC with STBC and short GI (802.11ac MCS 9 STBC short GI is common live).
    {const struct {bool vht,stbc,sgi;int mcs;} cases[]={{false,true,false,3},{false,false,true,7},{true,true,true,9},{true,false,true,7}};
     for(const auto& c:cases){Ppdu p;p.vht=c.vht;p.stbc=c.stbc;p.sgi=c.sgi;p.ldpc=true;p.aggregate=true;p.mcs=c.mcs;p.mpdus={fcs_frame(500,1),fcs_frame(64,2)};
         auto r=decode(transmit(p,34,seed++));
         check(same(r,p.mpdus)&&r.sig&&r.sig->ldpc,std::string(c.vht?"802.11ac":"802.11n")+" LDPC"+(c.stbc?" STBC":"")+(c.sgi?" short GI":"")+": "+r.status);}}
    // The LDPC codec on its own: all 12 codes through noise; heavy noise is reported.
    {
        std::mt19937 rng(3);std::normal_distribution<float> n(0,.5f);int bad=0,flagged=0;
        const int rates[4][2]={{1,2},{2,3},{3,4},{5,6}};
        for(int i=0;i<120;++i){
            const auto rr=rates[i%4];const long pld=16+8*long(1+i*37%2500),cbps=52L*(1+i%6),dbps=cbps*rr[0]/rr[1];
            const auto plan=wifi::ldpc_plan(pld,cbps*((pld+dbps-1)/dbps),rr[0],rr[1],int(cbps),1,std::nullopt);
            Bits payload(static_cast<size_t>(pld));for(auto& b:payload)b=rng()&1;
            const auto coded=wifi::ldpc_encode(payload,plan);std::vector<float> llr(coded.size());
            for(size_t j=0;j<coded.size();++j)llr[j]=(coded[j]?1.f:-1.f)+n(rng);
            Bits out;if(coded.size()!=size_t(plan.coded_bits)||wifi::ldpc_decode(llr.data(),plan,out)||out!=payload)++bad;
            for(auto& v:llr)v=v*0.05f+n(rng);  // far below the threshold
            if(wifi::ldpc_decode(llr.data(),plan,out)>0)++flagged;
        }
        check(bad==0,"LDPC codec: 120 noisy round trips over all codes and rates");
        check(flagged==120,"LDPC codec: unmet parity reported at very low SNR ("+std::to_string(flagged)+"/120)");
    }
    // 802.11ax HE SU: every MCS (BCC up to MCS 9, LDPC up to 1024-QAM), both
    // guard intervals; payload sizes give a = 1..4, LDPC extra segments, and
    // a packet extension that sets the PE-disambiguity bit.
    {
        int a_seen[5]={0,0,0,0,0},extra=0,pe=0;
        for(int ldpc=0;ldpc<2;++ldpc)for(int mcs=0;mcs<=(ldpc?11:9);++mcs){
            bool ok=true;std::string last;
            for(size_t n:{60,451,1500,2900}){
                HePpdu p;p.ldpc=ldpc;p.mcs=mcs;p.gi=(n%2)?32:16;p.pe_samples=n==451?320:0;p.mpdus={fcs_frame(n,uint8_t(n+mcs))};HeInfo info;
                auto r=decode(transmit_he(p,mcs>=10?40:34,seed++,&info));
                ok=ok&&same(r,p.mpdus)&&r.sig&&r.sig->format=="802.11ax HE SU"&&r.sig->pre_fec_padding==info.a&&r.sig->pe_disambiguity==info.pe_disambiguity;
                last=r.status;++a_seen[info.a];extra+=info.extra;pe+=info.pe_disambiguity;
            }
            check(ok,std::string("802.11ax ")+(ldpc?"LDPC":"BCC")+" MCS "+std::to_string(mcs)+": "+last);
        }
        check(a_seen[1]&&a_seen[2]&&a_seen[3]&&a_seen[4]&&extra&&pe,"802.11ax cases include a = 1..4 ("+std::to_string(a_seen[1])+","+std::to_string(a_seen[2])+","+
              std::to_string(a_seen[3])+","+std::to_string(a_seen[4])+"), LDPC extra segments ("+std::to_string(extra)+") and PE disambiguity ("+std::to_string(pe)+")");
    }
    // 802.11ax out of scope: reported, never claimed.
    {const struct {const char* what;HePpdu p;const char* reason;} cases[]={
        {"DCM",[]{HePpdu p;p.mcs=1;p.dcm=true;return p;}(),"payload not decoded (DCM"},
        {"4x HE-LTF",[]{HePpdu p;p.mcs=1;p.gi_ltf_field=3;return p;}(),"payload not decoded (4x HE-LTF"}};
     for(const auto& c:cases){HePpdu p=c.p;p.mpdus={fcs_frame(100,1)};auto r=decode(transmit_he(p,34,seed++));
         check(!r.fcs_valid&&r.sig&&r.sig->crc_ok&&r.status.find(c.reason)!=std::string::npos,std::string("802.11ax ")+c.what+" not claimed: "+r.status);}}
    // Out of scope: the header is reported, the payload never claimed.
    {wifi::HighThroughputSig h;h.format="802.11ac VHT";h.bandwidth_mhz=20;h.streams=h.space_time_streams=1;h.group_id=63;h.mcs=9;
     const auto bcc=wifi::ht_payload_unsupported_reason(h);h.ldpc=true;
     check(bcc.find("MCS 9 with BCC")!=std::string::npos&&wifi::ht_payload_unsupported_reason(h).empty(),"802.11ac MCS 9 at 20 MHz: LDPC only ("+bcc+")");}
    {Ppdu p;p.mcs=4;p.wide_flag=true;p.mpdus={fcs_frame(120,1)};auto r=decode(transmit(p,32,seed++));
     check(!r.fcs_valid&&r.status.find("payload not decoded (40 MHz")!=std::string::npos,"40 MHz not claimed: "+r.status);}
    {Ppdu p;p.vht=true;p.mcs=8;p.mpdus={fcs_frame(120,1)};const auto y=transmit(p,32,seed++);
     // Truncated after the VHT-SIG-B: header decoded, payload not claimed.
     auto r=decode(std::vector<C>(y.begin(),y.begin()+300+320+80*5+200));
     check(!r.fcs_valid&&r.sig&&r.sig->crc_ok&&r.status.find("capture ends before the payload")!=std::string::npos&&r.samples_required>0,"truncated PPDU: "+r.status);}
    // Heavy noise: no FCS-valid MPDU is ever invented.
    {int claimed=0;for(int i=0;i<10;++i){Ppdu p;p.mcs=7;p.mpdus={fcs_frame(400,uint8_t(i))};auto r=decode(transmit(p,8,seed++));
         if(r.fcs_valid&&!same(r,p.mpdus))++claimed;}
     check(claimed==0,"noisy 64-QAM never yields a wrong FCS-valid MPDU");}
    std::cout<<checks-failures<<"/"<<checks<<" checks passed\n";
    return failures?1:0;
}
