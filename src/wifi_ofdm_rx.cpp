#include "wifi_ofdm_rx.hpp"
#include "wifi_ldpc.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
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
// State s (6 bits) and input b reach state ((s<<1)|b)&63, so each state has
// two predecessors; one decision bit per state and step keeps a 5 ms HT
// PSDU's trellis at 8 bytes per bit. Costs are renormalised every step.
std::vector<uint8_t> viterbi(const std::vector<float>& llr,size_t bits,bool terminated) {
    static const auto sign=[]{std::array<std::array<float,2>,128> o{};for(unsigned r=0;r<128;++r){o[r][0]=parity(r&0155)?1.f:-1.f;o[r][1]=parity(r&0117)?1.f:-1.f;}return o;}();
    std::array<float,64> cost,next; cost.fill(1e30f);cost[0]=0;
    std::vector<uint64_t> decision(bits);
    for(size_t t=0;t<bits;++t){
        const float a=llr[2*t],b=llr[2*t+1];uint64_t d=0;float low=1e30f;
        for(unsigned ns=0;ns<64;++ns){
            const unsigned s0=ns>>1,s1=s0|32;  // registers ns and ns|64
            const float m0=cost[s0]-sign[ns][0]*a-sign[ns][1]*b,m1=cost[s1]-sign[ns|64][0]*a-sign[ns|64][1]*b;
            if(m1<m0){next[ns]=m1;d|=uint64_t(1)<<ns;}else next[ns]=m0;
            low=std::min(low,next[ns]);
        }
        decision[t]=d;
        for(unsigned s=0;s<64;++s)cost[s]=next[s]-low;
    }
    unsigned state=terminated?0:unsigned(std::min_element(cost.begin(),cost.end())-cost.begin());
    std::vector<uint8_t> out(bits);
    for(size_t t=bits;t-->0;){out[t]=state&1;state=(state>>1)|unsigned((decision[t]>>state)&1)<<5;}
    return out;
}
// Legacy interleaver has 16 columns; HT/VHT/HE 20 MHz (52 data subcarriers) 13.
std::vector<float> deinterleave(const std::vector<float>& in,int bpsc,int columns=16){
    int count=int(in.size()),s=std::max(bpsc/2,1);std::vector<float> out(count);
    for(int k=0;k<count;++k){int i=(count/columns)*(k%columns)+k/columns;int j=s*(i/s)+(i+count-(columns*i)/count)%s;out[k]=in[j];}
    return out;
}
std::array<C,64> fft(const std::vector<C>& x,size_t start,kiss_fft_cfg cfg){
    kiss_fft_cpx a[64],b[64];for(int i=0;i<64;++i){a[i].r=x[start+i].real();a[i].i=x[start+i].imag();}kiss_fft(cfg,a,b);
    std::array<C,64> out;for(int i=0;i<64;++i)out[i]={b[i].r,b[i].i};return out;
}
// IEEE Gray labels, I then Q: the axis bits b0 b1 ... (b0 in bit 0 here),
// read MSB first, are the binary-reflected Gray code of the level index
// from the most negative level up (BPSK to 1024-QAM).
float axis(unsigned bits,int width){
    unsigned g=0;for(int j=0;j<width;++j)g|=((bits>>j)&1u)<<(width-1-j);
    unsigned i=g;for(unsigned t=g>>1;t;t>>=1)i^=t;
    return float(2*int(i)-((1<<width)-1));
}
// Max-log soft bits of one axis (Gray labels are separable in I and Q).
void axis_llr(float v,int width,float scale,float weight,float* out){
    static const auto levels=[]{std::array<std::array<float,32>,6> l{};for(int w=1;w<=5;++w)for(unsigned u=0;u<(1u<<w);++u)l[size_t(w)][u]=axis(u,w);return l;}();
    float d[32];const unsigned count=1u<<width;
    for(unsigned u=0;u<count;++u){const float e=v-levels[size_t(width)][u]/scale;d[u]=e*e;}
    for(int bit=0;bit<width;++bit){
        float d0=1e30f,d1=1e30f;
        for(unsigned u=0;u<count;++u){if((u>>bit)&1)d1=std::min(d1,d[u]);else d0=std::min(d0,d[u]);}
        out[bit]=(d0-d1)*weight;
    }
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
// --- HT / VHT / HE headers (validated on live captures: every CRC
// convention below was the only one passing on real 802.11n/ac/ax frames).
// 802.11 CRC-8 (x^8+x^2+x+1), register preset to ones, output complemented
// and sent c7 first. HE-SIG-A carries only c7..c4.
unsigned crc8(const std::vector<uint8_t>& bits,size_t n){
    unsigned c=0xFF;
    for(size_t i=0;i<n;++i){unsigned fb=((c>>7)&1)^bits[i];c=(c<<1)&0xFF;if(fb)c^=0x07;}
    return (~c)&0xFF;
}
bool crc_ok(const std::vector<uint8_t>& bits,size_t n,size_t at,int width){
    const unsigned c=crc8(bits,n);
    for(int i=0;i<width;++i)if(bits[at+i]!=((c>>(7-i))&1))return false;
    return true;
}
int field(const std::vector<uint8_t>& b,size_t lo,int n){int v=0;for(int i=0;i<n;++i)v|=b[lo+i]<<i;return v;}
// Soft bits of one equalised BPSK (real axis) or QBPSK (imaginary axis,
// bit 1 = +j) symbol, deinterleaved; 48 legacy or 52 HE data subcarriers.
// Subcarriers without a channel estimate (HE +/-27, +/-28) are erasures.
std::vector<float> sig_soft(const std::array<C,64>& y,const std::array<C,64>& h,bool quadrature,bool wide){
    std::vector<float> llr;
    const int edge=wide?28:26;
    for(int k=-edge;k<=edge;++k){
        if(k==0||std::abs(k)==7||std::abs(k)==21)continue;
        const int bin=(k+64)%64;
        llr.push_back(4*(quadrature?y[bin].imag():y[bin].real())*std::norm(h[bin]));
    }
    return deinterleave(llr,1,wide?13:16);
}
// Equalise with a fixed common phase / slope (taken from the L-SIG pilots):
// HT-SIG and VHT-SIG-A2 are QBPSK, so their own pilots are not used.
std::array<C,64> equalize_fixed(const std::array<C,64>& raw,const std::array<C,64>& h,float intercept,float slope){
    std::array<C,64> y{};
    for(int k=-28;k<=28;++k){const int bin=(k+64)%64;if(std::norm(h[bin])>1e-18f)y[bin]=raw[bin]/h[bin]*std::polar(1.f,-intercept-slope*k);}
    return y;
}
void pilot_fit(const std::array<C,64>& raw,const std::array<C,64>& h,float& intercept,float& slope){
    constexpr int pilots[]={-21,-7,7,21};float phases[4];int polarity=pilot_polarity(0);
    for(int i=0;i<4;++i){const int bin=(pilots[i]+64)%64;C y=std::norm(h[bin])>1e-18f?raw[bin]/h[bin]:C{};
        phases[i]=std::arg(y*float((i==3?-1:1)*polarity));
        if(i){while(phases[i]-phases[i-1]>pi)phases[i]-=2*pi;while(phases[i]-phases[i-1]<-pi)phases[i]+=2*pi;}}
    intercept=0;slope=0;for(int i=0;i<4;++i){intercept+=phases[i]/4;slope+=pilots[i]*phases[i]/980.f;}
}
std::string join_summary(const std::vector<std::string>& parts){std::string s;for(const auto& p:parts)if(!p.empty())s+=(s.empty()?"":" | ")+p;return s;}

// Classifies the symbols after a 6 Mbps L-SIG and decodes the header.
// Returns nullopt for legacy (or undecidable) PPDUs.
std::optional<HighThroughputSig> decode_sig(const std::vector<C>& x,size_t sig,const std::array<C,64>& h,int lsig_length,kiss_fft_cfg cfg){
    if(sig+80*3+64>x.size())return std::nullopt;
    float intercept,slope;pilot_fit(fft(x,sig,cfg),h,intercept,slope);
    std::array<std::array<C,64>,4> e;for(size_t n=0;n<4;++n)e[n]=equalize_fixed(fft(x,sig+80*n,cfg),h,intercept,slope);
    double repeat=0,mag1=0,q1=0,p1=0,q2=0,p2=0;
    for(int k=-26;k<=26;++k){
        if(k==0||std::abs(k)==7||std::abs(k)==21)continue;
        const int bin=(k+64)%64;
        repeat+=(e[0][bin].real()>0?1:-1)*e[1][bin].real();mag1+=std::abs(e[1][bin]);
        q1+=e[1][bin].imag()*e[1][bin].imag();p1+=std::norm(e[1][bin]);
        q2+=e[2][bin].imag()*e[2][bin].imag();p2+=std::norm(e[2][bin]);
    }
    if(!(mag1>0&&p1>0&&p2>0))return std::nullopt;
    repeat/=mag1;q1/=p1;q2/=p2;
    HighThroughputSig s;
    auto bits_of=[&](std::initializer_list<std::pair<int,bool>> syms,bool wide){
        std::vector<float> llr;for(auto [n,quad]:syms){auto v=sig_soft(e[size_t(n)],h,quad,wide);llr.insert(llr.end(),v.begin(),v.end());}
        return viterbi(llr,llr.size()/2,true);
    };
    if(repeat>0.7&&lsig_length%3!=0){
        // RL-SIG repeats L-SIG: 802.11ax. HE-SIG-A is two BPSK symbols on 52 subcarriers.
        const auto b=bits_of({{2,false},{3,false}},true);
        s.crc_ok=crc_ok(b,42,42,4);
        s.txop=field(b,26,7);
        if(lsig_length%3==1&&field(b,0,1)==1){
            s.format="802.11ax HE SU";s.uplink=field(b,2,1);s.mcs=field(b,3,4);s.dcm=field(b,7,1);s.bss_color=field(b,8,6);
            s.spatial_reuse=field(b,15,4);s.bandwidth_mhz=20<<field(b,19,2);s.gi_ltf=field(b,21,2);s.space_time_streams=field(b,23,3)+1;
            s.ldpc=field(b,33,1);s.ldpc_extra_symbol=field(b,34,1);s.stbc=field(b,35,1);s.streams=s.stbc?s.space_time_streams/2:s.space_time_streams;
            const int a=field(b,37,2);s.pre_fec_padding=a?a:4;s.pe_disambiguity=field(b,39,1);s.doppler=field(b,41,1);
        }else if(lsig_length%3==1){
            s.format="802.11ax HE TB";s.uplink=1;s.bss_color=field(b,1,6);s.bandwidth_mhz=20<<field(b,24,2);
        }else{
            s.format="802.11ax HE MU";s.uplink=field(b,0,1);s.bss_color=field(b,5,6);s.spatial_reuse=field(b,11,4);
            const int bw=field(b,15,3);s.bandwidth_mhz=bw<4?20<<bw:(bw<6?80:160);
        }
    }else if(q1>0.7){
        // 802.11n: HT-SIG1 and HT-SIG2 both QBPSK.
        const auto b=bits_of({{1,true},{2,true}},false);
        s.format="802.11n HT";s.crc_ok=crc_ok(b,34,34,8);
        s.mcs=field(b,0,7);s.bandwidth_mhz=field(b,7,1)?40:20;s.ht_length=field(b,8,16);s.aggregation=field(b,27,1);
        s.stbc=field(b,28,2)!=0;s.ldpc=field(b,30,1);s.short_gi=field(b,31,1);s.extension_ltfs=field(b,32,2);
        s.streams=s.mcs<32?s.mcs/8+1:0;s.space_time_streams=s.streams?s.streams+field(b,28,2):0;
    }else if(q1<0.3&&q2>0.7){
        // 802.11ac: VHT-SIG-A1 BPSK, VHT-SIG-A2 QBPSK.
        const auto b=bits_of({{1,false},{2,true}},false);
        s.format="802.11ac VHT";s.crc_ok=crc_ok(b,34,34,8);
        s.bandwidth_mhz=20<<field(b,0,2);s.stbc=field(b,3,1);s.group_id=field(b,4,6);
        if(s.group_id==0||s.group_id==63){s.space_time_streams=field(b,10,3)+1;s.streams=s.stbc?s.space_time_streams/2:s.space_time_streams;
            s.partial_aid=field(b,13,9);s.uplink=s.group_id==0;}
        s.short_gi=field(b,24,1);s.nsym_disambiguation=field(b,25,1);s.ldpc=field(b,26,1);s.ldpc_extra_symbol=field(b,27,1);s.mcs=s.group_id==0||s.group_id==63?field(b,28,4):-1;
    }else return std::nullopt;
    char buf[64];
    std::vector<std::string> parts={s.format};
    if(s.mcs>=0){std::snprintf(buf,sizeof buf,"MCS %d",s.mcs);parts.push_back(buf);}
    if(s.bandwidth_mhz){std::snprintf(buf,sizeof buf,"%d MHz",s.bandwidth_mhz);parts.push_back(buf);}
    if(s.streams){std::snprintf(buf,sizeof buf,"%d stream%s",s.streams,s.streams>1?"s":"");parts.push_back(buf);}
    if(s.ldpc)parts.push_back("LDPC");
    if(s.stbc)parts.push_back("STBC");
    if(s.short_gi)parts.push_back("short GI");
    if(s.ht_length>=0){std::snprintf(buf,sizeof buf,"%d bytes%s",s.ht_length,s.aggregation?" (A-MPDU)":"");parts.push_back(buf);}
    if(s.group_id>=0){std::snprintf(buf,sizeof buf,"group %d%s",s.group_id,s.group_id==0?" (to AP)":s.group_id==63?" (from AP / SU)":" (MU)");parts.push_back(buf);}
    if(s.partial_aid>=0&&s.group_id==0){std::snprintf(buf,sizeof buf,"partial AID %d",s.partial_aid);parts.push_back(buf);}
    if(s.bss_color>=0){std::snprintf(buf,sizeof buf,"BSS colour %d",s.bss_color);parts.push_back(buf);}
    if(s.format.rfind("802.11ax",0)==0&&s.uplink>=0)parts.push_back(s.uplink?"uplink":"downlink");
    parts.push_back(s.crc_ok?"header CRC OK":"header CRC FAILED");
    s.summary=join_summary(parts);
    return s;
}

// --- 802.11n / 802.11ac payloads: 20 MHz, one spatial stream, BCC. STBC
// (two space-time streams, Alamouti) is combined with one receive antenna.
struct HtMcs { int bpsc,num,den; };
constexpr HtMcs ht_mcs[12]={{1,1,2},{2,1,2},{2,3,4},{4,1,2},{4,3,4},{6,2,3},{6,3,4},{6,5,6},{8,3,4},{8,5,6},{10,3,4},{10,5,6}};
// HT-LTF / VHT-LTF on subcarriers -28..28: the L-LTF with +1,+1 / -1,-1 edges.
float htltf(int k){return k<-26?1.f:k>26?-1.f:float(training[k+26]);}
bool ht_data_subcarrier(int k){return k&&std::abs(k)!=7&&std::abs(k)!=21;}
// Puncturing of the rate-1/2 mother code, serialized A/B (rate 5/6: A 11010, B 10101).
std::vector<int> puncture_mask(int num,int den){
    if(num==2&&den==3)return {1,1,1,0};
    if(num==3&&den==4)return {1,1,1,0,0,1};
    if(num==5&&den==6)return {1,1,1,0,0,1,1,0,0,1};
    return {1,1};
}
// Pilot of space-time stream `sts` on pilot j (subcarriers -21,-7,7,21) in
// data symbol n, before polarity. 802.11n uses per-stream patterns for two
// streams; 802.11ac at 20 MHz the single-stream pattern on every stream.
float ht_pilot(bool vht,int n_sts,int sts,int j,size_t n){
    static const int one[4]={1,1,1,-1},two[2][4]={{1,1,-1,-1},{1,-1,-1,1}};
    const size_t i=(n+size_t(j))%4;
    return float(vht||n_sts==1?one[i]:two[sts][i]);
}
// Average energy of the integer-level constellation: BPSK 1, M-QAM 2(M-1)/3.
float qam_scale(int bpsc){return bpsc==1?1.f:std::sqrt(2.f*float((1<<bpsc)-1)/3.f);}
// Coded-bit order on the 52 data subcarriers: BCC interleaver (13 columns);
// 802.11n LDPC in subcarrier order; 802.11ac LDPC tone mapper (distance 4):
// constellation point k sits on data subcarrier 4 (k mod 13) + floor(k / 13).
enum class BitOrder { Bcc, Plain, ToneMap };
void demap_ht(const std::array<C,64>& d,const std::array<float,64>& weight,int bpsc,std::vector<float>& coded,BitOrder order){
    const int width=std::max(1,bpsc/2);
    const float scale=qam_scale(bpsc);
    std::vector<float> llr(size_t(52*bpsc));size_t o=0;
    for(int k=-28;k<=28;++k){
        if(!ht_data_subcarrier(k))continue;
        const int bin=(k+64)%64;
        axis_llr(d[bin].real(),width,scale,weight[bin],&llr[o]);o+=size_t(width);
        if(bpsc>1){axis_llr(d[bin].imag(),width,scale,weight[bin],&llr[o]);o+=size_t(width);}
    }
    if(order==BitOrder::Bcc){const auto v=deinterleave(llr,bpsc,13);coded.insert(coded.end(),v.begin(),v.end());return;}
    if(order==BitOrder::Plain){coded.insert(coded.end(),llr.begin(),llr.end());return;}
    for(size_t k=0;k<52;++k){const size_t t=4*(k%13)+k/13;coded.insert(coded.end(),llr.begin()+long(t*size_t(bpsc)),llr.begin()+long((t+1)*size_t(bpsc)));}
}
uint8_t reverse8(unsigned v){uint8_t r=0;for(int i=0;i<8;++i)r|=uint8_t(((v>>i)&1)<<(7-i));return r;}
// A-MPDU subframe delimiter: EOF, reserved, length (12 bits, plus 2 high
// bits for VHT), CRC-8 of those 16 bits (the HT-SIG CRC, c7 first), 0x4E.
bool ampdu_delimiter(const uint8_t* d,bool vht,size_t& length){
    if(d[3]!=0x4E)return false;
    std::vector<uint8_t> bits(16);for(int i=0;i<16;++i)bits[size_t(i)]=(d[i/8]>>(i%8))&1;
    if(reverse8(crc8(bits,16))!=d[2])return false;
    length=size_t(d[0]>>4)|size_t(d[1])<<4;
    if(vht)length|=size_t((d[0]>>2)&3)<<12;
    return true;
}
bool fcs_ok(const uint8_t* p,size_t n){
    if(n<14)return false;
    uint32_t fcs=0;for(int i=0;i<4;++i)fcs|=uint32_t(p[n-4+size_t(i)])<<(8*i);
    return fcs32(p,n-4)==fcs;
}
// Payload of a CRC-valid HT/VHT header within ht_payload_unsupported_reason().
// Timing in 20 MHz samples relative to the L-SIG FFT window: HT-SIG / VHT-SIG-A
// (2 symbols), HT/VHT-STF (1), N_LTF long training symbols, VHT-SIG-B (VHT
// only), data. FFT windows back off into the guard interval; the channel
// estimate uses the same offset, so the delay folds into it.
void decode_ht_payload(const std::vector<C>& x,size_t sig,const HighThroughputSig& s,int lsig_length,kiss_fft_cfg cfg,double rate,OfdmDecodeResult& result){
    const bool vht=s.format=="802.11ac VHT";
    const HtMcs m=ht_mcs[s.mcs];
    const int cbps=52*m.bpsc,dbps=cbps*m.num/m.den,nsts=s.space_time_streams;
    const size_t stbc=nsts==2?2:1,nltf=size_t(nsts),backoff=3;
    const size_t ltf0=sig+80*4-backoff,first=ltf0+80*nltf+(vht?80:0);
    size_t nsym=0;
    std::optional<LdpcPlan> ldpc;
    if(vht){
        // L-LENGTH carries the PPDU duration; the data symbols are what is left
        // after VHT-SIG-A, VHT-STF, the VHT-LTFs and VHT-SIG-B. With LDPC the
        // payload fills the symbols before any extra one (VHT-SIG-A B3).
        const long data_us=long(lsig_length+3)/3*4-16-4*long(nltf);
        if(data_us>0)nsym=s.short_gi?size_t(data_us*10/36)-(s.nsym_disambiguation?1:0):size_t(data_us/4);
        const size_t extra=s.ldpc&&s.ldpc_extra_symbol?stbc:0;
        if(s.ldpc&&nsym>extra){
            const long init=long(nsym-extra);
            // MCS 9 at 20 MHz with one stream has 346 2/3 data bits per symbol and is
            // not defined; devices send it with LDPC using 346 (measured: 272 of 279
            // such PPDUs pass FCS that way, none with the other roundings).
            ldpc=ldpc_plan(init*dbps,init*cbps,m.num,m.den,cbps,int(stbc),s.ldpc_extra_symbol);
            if(ldpc->coded_bits!=long(nsym)*cbps){result.status=s.summary+"; payload decode failed (LDPC symbol count)";return;}
        }
    }else if(s.ldpc){
        const long pld=16+8L*s.ht_length,per=long(dbps)*long(stbc);
        ldpc=ldpc_plan(pld,long(cbps)*long(stbc)*((pld+per-1)/per),m.num,m.den,cbps,int(stbc),std::nullopt);
        nsym=size_t(ldpc->coded_bits/cbps);
    }else{
        const size_t bits=16+8*size_t(s.ht_length)+6;
        nsym=stbc*((bits+stbc*size_t(dbps)-1)/(stbc*size_t(dbps)));
    }
    const BitOrder order=!s.ldpc?BitOrder::Bcc:vht?BitOrder::ToneMap:BitOrder::Plain;
    auto window=[&](size_t i){return s.short_gi?first-8+72*i:first+80*i;};
    const size_t end=window(nsym)+(s.short_gi?8:16);
    result.samples_required=size_t(std::ceil(double(end)*rate/20e6));
    if(nsym==0||nsym%stbc){result.status=s.summary+"; payload not decoded (no data symbols)";return;}
    if(end>x.size()){result.status=s.summary+"; payload not decoded (capture ends before the payload)";return;}
    std::array<std::array<C,64>,2> h{};
    {
        const auto y0=fft(x,ltf0,cfg);const auto y1=nsts==2?fft(x,ltf0+80,cfg):y0;
        // Two LTFs carry the streams with P = [1 -1; 1 1].
        for(int k=-28;k<=28;++k){
            if(!k)continue;
            const int bin=(k+64)%64;const float l=htltf(k);
            if(nsts==1)h[0][bin]=y0[bin]/l;
            else{h[0][bin]=(y0[bin]-y1[bin])/(2*l);h[1][bin]=(y0[bin]+y1[bin])/(2*l);}
        }
    }
    constexpr int pilots[4]={-21,-7,7,21};
    const size_t z=vht?4:3;  // pilot polarity index of data symbol 0 (L-SIG is 0)
    float slope=0;
    std::vector<float> coded;coded.reserve(nsym*size_t(cbps));
    std::array<C,64> even{};
    for(size_t n=0;n<nsym;++n){
        auto y=fft(x,window(n),cfg);
        // Common phase from the pilots; the slope (sampling-clock drift) is
        // tracked slowly because four pilots per symbol are too noisy for it.
        const float polarity=float(pilot_polarity(n+z));
        float ph[4];C sum{};
        for(int j=0;j<4;++j){
            const int bin=(pilots[j]+64)%64;C e{};
            for(int t=0;t<nsts;++t)e+=h[size_t(t)][bin]*ht_pilot(vht,nsts,t,j,n);
            const C r=y[bin]*std::conj(e*polarity)*std::polar(1.f,-slope*float(pilots[j]));
            ph[j]=std::arg(r);sum+=r;
        }
        const float common=std::arg(sum);float residual=0;
        for(int j=0;j<4;++j)residual+=float(pilots[j])*std::remainder(ph[j]-common,float(2*pi))/980.f;
        slope+=0.1f*residual;
        for(int k=-28;k<=28;++k)y[(k+64)%64]*=std::polar(1.f,-common-slope*float(k));
        std::array<C,64> d{};std::array<float,64> w{};
        if(stbc==1){
            for(int k=-28;k<=28;++k){const int bin=(k+64)%64;w[bin]=std::norm(h[0][bin]);if(w[bin]>1e-18f)d[bin]=y[bin]/h[0][bin];}
            demap_ht(d,w,m.bpsc,coded,order);
        }else if(n%2==0){
            even=y;
        }else{
            // Alamouti: stream 1 sends (d0, d1), stream 2 (-d1*, d0*).
            std::array<C,64> d1{};
            for(int k=-28;k<=28;++k){
                const int bin=(k+64)%64;const C a=h[0][bin],b=h[1][bin];const float g=std::norm(a)+std::norm(b);
                w[bin]=g;if(g<=1e-18f)continue;
                d[bin]=(std::conj(a)*even[bin]+b*std::conj(y[bin]))/g;
                d1[bin]=(std::conj(a)*y[bin]-b*std::conj(even[bin]))/g;
            }
            demap_ht(d,w,m.bpsc,coded,order);demap_ht(d1,w,m.bpsc,coded,order);
        }
    }
    std::vector<uint8_t> data;size_t decoded=0;int ldpc_failed=0;
    if(ldpc){
        // No tail bits: SERVICE, PSDU and (802.11ac) padding fill N_pld.
        ldpc_failed=ldpc_decode(coded.data(),*ldpc,data);
        decoded=size_t(ldpc->payload_bits);
        if(data.size()!=decoded){result.status=s.summary+"; payload decode failed (LDPC)";return;}
    }else{
        const auto mask=puncture_mask(m.num,m.den);
        const size_t nbits=nsym*size_t(dbps);std::vector<float> mother(2*nbits,0);size_t pos=0;
        for(size_t i=0;i<mother.size();++i)if(mask[i%mask.size()]){if(pos>=coded.size()){result.status=s.summary+"; payload decode failed (coded length)";return;}mother[i]=coded[pos++];}
        // 802.11n terminates the code right after the PSDU; 802.11ac after the PHY padding.
        decoded=vht?nbits:16+8*size_t(s.ht_length)+6;
        data=viterbi(mother,decoded,true);
    }
    unsigned seed=0;
    for(unsigned trial=1;trial<128;++trial){unsigned state=trial;bool match=true;for(size_t i=0;i<7;++i){unsigned v=((state>>6)^(state>>3))&1;if(v!=data[i])match=false;state=((state<<1)|v)&127;}if(match){seed=trial;break;}}
    if(!seed){result.status=s.summary+"; payload decode failed (scrambler seed)";return;}
    unsigned state=seed;for(size_t i=0;i<decoded;++i){unsigned v=((state>>6)^(state>>3))&1;data[i]^=v;state=((state<<1)|v)&127;}
    // SERVICE is zero, except 802.11ac bits 8..15 (CRC of VHT-SIG-B).
    for(size_t i=0;i<(vht?8u:16u);++i)if(data[i]){result.status=s.summary+"; payload decode failed (SERVICE)";return;}
    const size_t bytes=vht?(decoded-16-(ldpc?0:6))/8:size_t(s.ht_length);
    std::vector<uint8_t> psdu(bytes,0);for(size_t i=0;i<8*bytes;++i)psdu[i/8]|=uint8_t(data[16+i]<<(i%8));
    result.psdu_length=bytes;
    result.data_rate_mbps=double(dbps)/(s.short_gi?3.6:4.0);
    if(!vht&&!s.aggregation){
        if(fcs_ok(psdu.data(),psdu.size()))result.mpdus.push_back(psdu);
    }else{
        // Walk the subframes; after a damaged delimiter, resynchronise on the next valid one.
        for(size_t at=0;at+4<=psdu.size();){
            size_t length=0;
            if(!ampdu_delimiter(&psdu[at],vht,length)||length==0){at+=4;continue;}  // damaged, or EOF padding
            if(at+4+length>psdu.size())break;
            ++result.ampdu_subframes;
            if(fcs_ok(&psdu[at+4],length))result.mpdus.emplace_back(psdu.begin()+long(at+4),psdu.begin()+long(at+4+length));
            at+=4+((length+3)&~size_t(3));
        }
    }
    char buf[96];
    if(result.mpdus.empty()){
        std::snprintf(buf,sizeof buf,"; payload decode failed (%s",(!vht&&!s.aggregation)?"FCS":result.ampdu_subframes?"no FCS-valid MPDU":"no A-MPDU delimiter");
        std::string why=buf;
        if(ldpc_failed){std::snprintf(buf,sizeof buf,"; LDPC parity unmet in %d/%d codewords",ldpc_failed,ldpc->codewords);why+=buf;}
        result.status=s.summary+why+")";return;
    }
    result.fcs_valid=true;result.mpdu=result.mpdus.front();result.rate_mbps=int(std::lround(result.data_rate_mbps));
    for(const auto& mpdu:result.mpdus){auto beacon=parse_beacon(mpdu.data(),mpdu.size());if(beacon&&beacon->fcs_valid){result.beacon=std::move(beacon);break;}}
    if(!vht&&!s.aggregation)std::snprintf(buf,sizeof buf,"; payload decoded (%.1f Mbps), MPDU FCS-valid",result.data_rate_mbps);
    else std::snprintf(buf,sizeof buf,"; payload decoded (%.1f Mbps), A-MPDU %zu/%d MPDUs FCS-valid",result.data_rate_mbps,result.mpdus.size(),result.ampdu_subframes);
    result.status=s.summary+buf;
}

// --- 802.11ax HE SU payloads: 20 MHz (242-tone RU), one spatial stream, no
// STBC or DCM, BCC or LDPC, 2x HE-LTF. 256-point data symbols (78.125 kHz).
// HE-LTF (2x) on tones -122..122 step 2, index (k + 122) / 2; 0 at DC.
// Derived from live HE SU frames: neighbouring-tone sign votes (a real
// channel is smooth over 156 kHz) chained across the band, and the remaining
// choices (overall sign, two low-confidence links) settled by FCS: only this
// variant decodes any payload (docs/WIFI_HE_PAYLOADS_2026-11-04.md).
constexpr int8_t heltf2x[123]={
    -1,-1,-1,1,1,-1,1,-1,-1,-1,-1,1,-1,1,-1,-1,1,1,-1,1,1,1,1,1,-1,1,-1,1,-1,-1,1,
    1,-1,1,-1,-1,-1,-1,1,-1,1,1,1,-1,-1,1,-1,-1,-1,-1,-1,1,-1,-1,-1,1,1,1,-1,-1,1,0,
    1,-1,1,1,-1,1,1,-1,1,1,-1,-1,1,-1,1,1,1,1,-1,1,-1,1,1,-1,-1,1,-1,-1,-1,-1,-1,
    1,-1,1,1,-1,-1,1,1,-1,1,-1,-1,-1,-1,1,-1,1,1,1,-1,-1,1,-1,-1,-1,-1,-1,1,-1,1};
constexpr int he_pilots[8]={-116,-90,-48,-22,22,48,90,116};
bool he_data_tone(int k){
    if(std::abs(k)<2||std::abs(k)>122)return false;
    for(int p:he_pilots)if(k==p)return false;
    return true;
}
std::vector<C> fftn(const std::vector<C>& x,size_t start,size_t n,kiss_fft_cfg cfg){
    std::vector<kiss_fft_cpx> a(n),b(n);for(size_t i=0;i<n;++i){a[i].r=x[start+i].real();a[i].i=x[start+i].imag();}
    kiss_fft(cfg,a.data(),b.data());
    std::vector<C> out(n);for(size_t i=0;i<n;++i)out[i]={b[i].r,b[i].i};return out;
}
void decode_he_payload(const std::vector<C>& x,size_t sig,const HighThroughputSig& s,int lsig_length,double rate,OfdmDecodeResult& result){
    struct Plan { kiss_fft_cfg c128=kiss_fft_alloc(128,0,nullptr,nullptr),c256=kiss_fft_alloc(256,0,nullptr,nullptr);
                  ~Plan(){kiss_fft_free(c128);kiss_fft_free(c256);} } plan;
    const HtMcs m=ht_mcs[s.mcs];
    const int cbps=234*m.bpsc,dbps=cbps*m.num/m.den,cbps_short=60*m.bpsc,dbps_short=cbps_short*m.num/m.den;
    const size_t gi=s.gi_ltf==2?32:16,ltf=128,backoff=3;
    // L-SIG, RL-SIG, HE-SIG-A (2), HE-STF (4 us), one HE-LTF, then data.
    const size_t ltf_start=sig-16+80*5,data_start=ltf_start+gi+ltf;
    // PPDU duration from L-LENGTH (m = 2 for HE SU); times in 0.05 us samples.
    const long txtime_us=20+4*(long(lsig_length)+3+2)/3;
    const long after=20*(txtime_us-20)-80*3-80-long(gi+ltf);
    const long nsym=after/long(gi+256)-(s.pe_disambiguity?1:0);
    const int a=s.pre_fec_padding;
    auto cbps_last=[&](int f){return f<4?f*cbps_short:cbps;};
    auto dbps_last=[&](int f){return f<4?f*dbps_short:dbps;};
    auto window=[&](size_t n){return data_start+n*(gi+256)+gi-backoff;};
    const size_t end=nsym>0?window(size_t(nsym-1))+256:0;
    result.samples_required=size_t(std::ceil(double(end)*rate/20e6));
    if(nsym<=0){result.status=s.summary+"; payload not decoded (no data symbols)";return;}
    if(end>x.size()){result.status=s.summary+"; payload not decoded (capture ends before the payload)";return;}
    // Channel on the even tones from the 2x HE-LTF; odd tones interpolated.
    // The 2x HE-LTF puts twice the power on half the tones and is read with a
    // 128-point FFT: sqrt(2) relative to 256-point data symbols (measured:
    // equalised data power 2.00 without it on the cleanest frames).
    std::array<C,256> h{};
    {
        const auto r=fftn(x,ltf_start+gi-backoff,ltf,plan.c128);
        for(int k=-122;k<=122;k+=2){const int l=heltf2x[(k+122)/2];if(l)h[size_t((k+256)%256)]=r[size_t((k/2+128)%128)]*std::sqrt(2.f)/float(l);}
        // One LTF symbol is noisy: triangular smoothing over +/-3 even tones
        // (+/-469 kHz; on live frames 87 -> 98 FCS-valid PPDUs, wider is worse).
        // Its bias on frequency-selective channels limits 256/1024-QAM, so
        // those get +/-1 and none.
        const int span=m.bpsc<=6?3:m.bpsc==8?1:0;
        std::array<C,256> e=h;
        for(int k=-122;k<=122;k+=2){
            if(!k)continue;
            C sum{};float n=0;
            for(int d=-span;d<=span;++d){const int t=k+2*d;if(std::abs(t)<2||std::abs(t)>122)continue;const float wt=float(span+1-std::abs(d));sum+=wt*h[size_t((t+256)%256)];n+=wt;}
            e[size_t((k+256)%256)]=sum/n;
        }
        h=e;
        for(int k=-121;k<=121;k+=2)if(std::abs(k)>1)h[size_t((k+256)%256)]=0.5f*(h[size_t((k-1+256)%256)]+h[size_t((k+1+256)%256)]);
    }
    // Pilot values are not assumed: BPSK pilots squared give the common phase
    // modulo pi, and continuity from the HE-LTF (phase 0) resolves it.
    float phase=0,slope=0;
    std::vector<float> coded;coded.reserve(size_t(nsym)*size_t(cbps));
    const float scale=qam_scale(m.bpsc);const int width=std::max(1,m.bpsc/2);
    for(long n=0;n<nsym;++n){
        auto y=fftn(x,window(size_t(n)),256,plan.c256);
        C sq{};float ph[8];
        for(int j=0;j<8;++j){const size_t bin=size_t((he_pilots[j]+256)%256);if(std::norm(h[bin])<1e-18f){ph[j]=0;continue;}
            const C e=y[bin]/h[bin]*std::polar(1.f,-slope*float(he_pilots[j]));sq+=e*e*std::norm(h[bin]);ph[j]=std::arg(e);}
        float common=0.5f*std::arg(sq);
        common+=float(pi)*std::round((phase-common)/float(pi));
        float residual=0,norm=0;
        for(int j=0;j<8;++j){float d=std::remainder(ph[j]-common,float(pi));residual+=float(he_pilots[j])*d;norm+=float(he_pilots[j]*he_pilots[j]);}
        slope+=0.1f*residual/norm;phase=common;
        std::vector<float> llr(static_cast<size_t>(cbps));size_t o=0;
        for(int k=-122;k<=122;++k){
            if(!he_data_tone(k))continue;
            const size_t bin=size_t((k+256)%256);const float w=std::norm(h[bin]);
            const C d=w>1e-18f?y[bin]/h[bin]*std::polar(1.f,-common-slope*float(k)):C{};
            axis_llr(d.real(),width,scale,w,&llr[o]);o+=size_t(width);
            if(m.bpsc>1){axis_llr(d.imag(),width,scale,w,&llr[o]);o+=size_t(width);}
        }
        // BCC: 26-column interleaver. LDPC: tone mapper with distance 9,
        // constellation point k on data tone 9 (k mod 26) + floor(k / 26).
        std::vector<float> bits;
        if(!s.ldpc)bits=deinterleave(llr,m.bpsc,26);
        else for(size_t k=0;k<234;++k){const size_t t=9*(k%26)+k/26;bits.insert(bits.end(),llr.begin()+long(t*size_t(m.bpsc)),llr.begin()+long((t+1)*size_t(m.bpsc)));}
        // The last symbol carries only cbps_last(a) coded bits; the rest is post-FEC padding.
        coded.insert(coded.end(),bits.begin(),bits.begin()+(n+1<nsym?cbps:cbps_last(a)));
    }
    std::vector<uint8_t> data;size_t decoded=0;int ldpc_failed=0;std::optional<LdpcPlan> ldpc;
    if(s.ldpc){
        // An LDPC extra symbol segment raised a by one (from 4 to a new symbol at 1).
        long nsym_init=nsym;int a_init=a;
        if(s.ldpc_extra_symbol){if(a==1){a_init=4;nsym_init=nsym-1;}else a_init=a-1;}
        const long pld=(nsym_init-1)*dbps+dbps_last(a_init);
        ldpc=ldpc_plan_he(pld,(nsym_init-1)*cbps+cbps_last(a_init),(nsym-1)*cbps+cbps_last(a),m.num,m.den);
        if(ldpc->coded_bits!=long(coded.size())){result.status=s.summary+"; payload decode failed (LDPC length)";return;}
        ldpc_failed=ldpc_decode(coded.data(),*ldpc,data);decoded=size_t(pld);
        if(data.size()!=decoded){result.status=s.summary+"; payload decode failed (LDPC)";return;}
    }else{
        const auto mask=puncture_mask(m.num,m.den);
        decoded=size_t((nsym-1)*dbps+dbps_last(a));
        std::vector<float> mother(2*decoded,0);size_t pos=0;
        for(size_t i=0;i<mother.size();++i)if(mask[i%mask.size()]){if(pos>=coded.size()){result.status=s.summary+"; payload decode failed (coded length)";return;}mother[i]=coded[pos++];}
        data=viterbi(mother,decoded,true);
    }
    unsigned seed=0;
    for(unsigned trial=1;trial<128;++trial){unsigned state=trial;bool match=true;for(size_t i=0;i<7;++i){unsigned v=((state>>6)^(state>>3))&1;if(v!=data[i])match=false;state=((state<<1)|v)&127;}if(match){seed=trial;break;}}
    if(!seed){result.status=s.summary+"; payload decode failed (scrambler seed)";return;}
    unsigned state=seed;for(size_t i=0;i<decoded;++i){unsigned v=((state>>6)^(state>>3))&1;data[i]^=v;state=((state<<1)|v)&127;}
    for(size_t i=0;i<8;++i)if(data[i]){result.status=s.summary+"; payload decode failed (SERVICE)";return;}
    const size_t bytes=(decoded-16-(s.ldpc?0:6))/8;
    std::vector<uint8_t> psdu(bytes,0);for(size_t i=0;i<8*bytes;++i)psdu[i/8]|=uint8_t(data[16+i]<<(i%8));
    result.psdu_length=bytes;result.data_rate_mbps=double(dbps)/(double(gi+256)/20.0);
    for(size_t at=0;at+4<=psdu.size();){
        size_t length=0;
        if(!ampdu_delimiter(&psdu[at],true,length)||length==0){at+=4;continue;}
        if(at+4+length>psdu.size())break;
        ++result.ampdu_subframes;
        if(fcs_ok(&psdu[at+4],length))result.mpdus.emplace_back(psdu.begin()+long(at+4),psdu.begin()+long(at+4+length));
        at+=4+((length+3)&~size_t(3));
    }
    char buf[96];
    if(result.mpdus.empty()){
        std::snprintf(buf,sizeof buf,"; payload decode failed (%s",result.ampdu_subframes?"no FCS-valid MPDU":"no A-MPDU delimiter");
        std::string why=buf;
        if(ldpc_failed){std::snprintf(buf,sizeof buf,"; LDPC parity unmet in %d/%d codewords",ldpc_failed,ldpc->codewords);why+=buf;}
        result.status=s.summary+why+")";return;
    }
    result.fcs_valid=true;result.mpdu=result.mpdus.front();result.rate_mbps=int(std::lround(result.data_rate_mbps));
    for(const auto& mpdu:result.mpdus){auto beacon=parse_beacon(mpdu.data(),mpdu.size());if(beacon&&beacon->fcs_valid){result.beacon=std::move(beacon);break;}}
    std::snprintf(buf,sizeof buf,"; payload decoded (%.1f Mbps), A-MPDU %zu/%d MPDUs FCS-valid",result.data_rate_mbps,result.mpdus.size(),result.ampdu_subframes);
    result.status=s.summary+buf;
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
int vht_partial_aid(const std::string& bssid){
    unsigned b[6];
    if(std::sscanf(bssid.c_str(),"%2x:%2x:%2x:%2x:%2x:%2x",&b[0],&b[1],&b[2],&b[3],&b[4],&b[5])!=6)return -1;
    return int(((b[4]>>7)|(b[5]<<1))&0x1FF);
}
std::string ht_payload_unsupported_reason(const HighThroughputSig& s){
    const bool ht=s.format=="802.11n HT",vht=s.format=="802.11ac VHT",he=s.format=="802.11ax HE SU";
    if(he){
        if(s.bandwidth_mhz!=20)return std::to_string(s.bandwidth_mhz)+" MHz; 20 MHz only";
        if(s.streams!=1||s.stbc)return s.stbc?"STBC not supported for 802.11ax":std::to_string(s.streams)+" spatial streams; one only";
        if(s.dcm)return "DCM not supported";
        if(s.doppler)return "midambles (Doppler) not supported";
        if(s.gi_ltf!=1&&s.gi_ltf!=2)return s.gi_ltf==0?"1x HE-LTF not supported":"4x HE-LTF not supported";
        return "";
    }
    if(!ht&&!vht)return s.format.rfind("802.11ax",0)==0?"802.11ax MU / TB payloads not supported":"format not supported";
    if(s.bandwidth_mhz!=20)return std::to_string(s.bandwidth_mhz)+" MHz; 20 MHz only";
    if(s.streams!=1||s.space_time_streams>2)return std::to_string(s.streams)+" spatial streams; one only";
    if(vht&&s.group_id!=0&&s.group_id!=63)return "multi-user";
    if(ht&&s.extension_ltfs)return "extension LTFs";
    if(ht&&s.ht_length<=0)return "no data (sounding)";
    if(vht&&s.mcs>8&&!s.ldpc)return "MCS "+std::to_string(s.mcs)+" with BCC is not defined for one stream at 20 MHz";
    return "";
}
OfdmDecodeResult decode_ofdm_burst(const C* input,size_t n,double rate,double center,double channel){
    OfdmDecodeResult result;
    if(!input||n<400||!std::isfinite(rate)||rate<20e6||rate>64e6||!std::isfinite(center)||!std::isfinite(channel)||n>size_t(rate*0.006)) {result.status="Unsupported rate or capture length";return result;}
    for(size_t i=0;i<n;++i)if(!std::isfinite(input[i].real())||!std::isfinite(input[i].imag())){result.status="Non-finite IQ";return result;}
    auto x=baseband(input,n,rate,channel-center);
    size_t stf=0,run=0;C phase{};bool found=false;
    // A constant also repeats every 16 samples: after DC removal the quiet
    // lead-in of a very clean burst is one (the burst mean). Its 1-sample
    // correlation is near 1; an ideal L-STF's is 0.15 but real ones run
    // higher, so only windows above 0.8 are skipped (live captures: +8
    // FCS-valid PPDUs; a 0.5 cutoff lost a quarter of them).
    for(size_t i=0;i+64<x.size()&&i<2048;++i){C p{},q{};double a=0,b=0;for(size_t j=0;j<48;++j){p+=std::conj(x[i+j])*x[i+j+16];q+=std::conj(x[i+j])*x[i+j+1];a+=std::norm(x[i+j]);b+=std::norm(x[i+j+16]);}
        double corr=std::norm(p)/(a*b+1e-30);const bool constant=std::norm(q)>0.64*a*a;
        if(corr>.65&&!constant){if(!run){stf=i;phase=p;}if(++run>=32){found=true;break;}}else run=0;
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
    // HT/VHT/HE PPDUs carry a 6 Mbps L-SIG; their header follows it. A CRC-valid
    // header decides the attempt: its payload is decoded when in scope, and
    // otherwise reported as not decoded. A failed header falls through to the
    // legacy attempt as before.
    if(mode->mbps==6){
        result.sig=decode_sig(x,sig,h,length,plan.cfg);
        if(result.sig&&result.sig->crc_ok){
            const std::string reason=ht_payload_unsupported_reason(*result.sig);
            if(reason.empty()&&result.sig->format=="802.11ax HE SU")decode_he_payload(x,sig,*result.sig,length,rate,result);
            else if(reason.empty())decode_ht_payload(x,sig,*result.sig,length,plan.cfg,rate,result);
            else result.status=result.sig->summary+"; payload not decoded ("+reason+")";
            return result;
        }
    }
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
    if(!result.fcs_valid){result.status=result.sig?result.sig->summary+"; legacy decode failed":"FCS failed (or unsupported PHY format)";return result;}
    result.mpdus.push_back(result.mpdu);
    auto beacon=parse_beacon(result.mpdu.data(),result.mpdu.size());if(beacon&&beacon->fcs_valid)result.beacon=std::move(beacon);
    result.status=result.beacon?"Decoded OFDM beacon/probe response":"FCS-valid non-beacon frame";
    return result;
}
}
