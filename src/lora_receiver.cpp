// Byte/symbol conventions cross-checked with LoRaPHY (MIT), see
// docs/LORA_RECEIVER_UPGRADE.md and third_party/loraphy_reference/LICENSE.
// Synchronization estimates timing from both chirp slopes; an upchirp
// multiplied by a downchirp reference is a tone, and vice versa.
#include "lora_receiver.hpp"
#include <kissfft/kiss_fft.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace rfmon::lora::receiver {
namespace {
double wrap(double x, int n) { x = std::fmod(x, n); return x < 0 ? x+n : x; }
double signed_bin(double x, int n) { x = wrap(x,n); return x >= n/2. ? x-n : x; }
int gray(int x) { return x ^ (x >> 1); }
int popcount(unsigned x) { int n=0; for (; x; x &= x-1) ++n; return n; }
uint8_t fec(unsigned x, int cr) {
    auto bit=[&](int i){return (x>>i)&1;};
    unsigned v=x;
    if(cr==1) v|=(bit(0)^bit(1)^bit(2)^bit(3))<<4;
    else {
        v|=(bit(0)^bit(1)^bit(2))<<4;
        v|=(bit(1)^bit(2)^bit(3))<<5;
        if(cr>=3) v|=(bit(0)^bit(1)^bit(3))<<6;
        if(cr==4) v|=(bit(0)^bit(2)^bit(3))<<7;
    }
    return uint8_t(v);
}
uint8_t unfec(uint8_t c,int cr,int& errors) {
    int dist=9, winner=0, ties=0;
    for(int x=0;x<16;++x) {
        int d=popcount((fec(x,cr)^c)&((1<<(cr+4))-1));
        if(d<dist) {dist=d;winner=x;ties=1;} else if(d==dist) ++ties;
    }
    if(dist) ++errors;
    // A parity-only code cannot uniquely correct a flipped bit. Keep observed
    // data on ties rather than biasing toward whichever nibble was tried first.
    return ties==1 ? uint8_t(winner) : uint8_t(c&15);
}
std::vector<uint8_t> deinterleave(const std::vector<int>& syms,int width) {
    std::vector<uint8_t> out(width,0);
    for(size_t k=0;k<syms.size();++k)
        for(int m=0;m<width;++m) out[(m+int(k))%width] |= ((syms[k]>>m)&1)<<k;
    return out;
}
uint8_t header_sum(int a,int b,int c) {
    const int rows[5][12]={{1,1,1,1,0,0,0,0,0,0,0,0},{1,0,0,0,1,1,1,0,0,0,0,1},
        {0,1,0,0,1,0,0,1,1,0,1,0},{0,0,1,0,0,1,0,1,0,1,1,1},{0,0,0,1,0,0,1,0,1,1,1,1}};
    int bits=(a<<8)|(b<<4)|c,out=0;
    for(auto& row:rows) {int parity=0;for(int i=0;i<12;++i)parity ^= ((bits>>(11-i))&1)*row[i];out=(out<<1)|parity;}
    return uint8_t(out);
}
uint16_t payload_crc(const std::vector<uint8_t>& bytes) {
    uint16_t c=0;
    for(size_t i=0;i+2<bytes.size();++i) {
        c ^= uint16_t(bytes[i])<<8;
        for(int j=0;j<8;++j)c=uint16_t((c<<1)^((c&0x8000)?0x1021:0));
    }
    if(!bytes.empty())c ^= bytes.back();
    if(bytes.size()>1)c ^= uint16_t(bytes[bytes.size()-2])<<8;
    return c;
}
struct Peak {double bin=0, ratio=0;};
class Analyzer {
    int n_;
    using Config=std::unique_ptr<kiss_fft_state,decltype(&free)>;
    Config coarse_, fine_;
    std::vector<kiss_fft_cpx> in_,out_;
    std::vector<std::complex<float>> up_;
    const std::vector<std::complex<float>>& iq_;
public:
    Analyzer(const std::vector<std::complex<float>>& iq,int n):n_(n),
        coarse_(kiss_fft_alloc(n,0,nullptr,nullptr),free),fine_(kiss_fft_alloc(n*8,0,nullptr,nullptr),free),
        in_(n*8),out_(n*8),up_(n),iq_(iq) {
        if(!coarse_||!fine_)throw std::bad_alloc();
        for(int j=0;j<n;++j)up_[j]=std::polar(1.f,float(2*M_PI*(double(j)*j/(2*n)-j/2.)));
    }
    Peak peak(long pos,bool down=false,bool fine=true) {
        if(pos<0 || size_t(pos)+n_>iq_.size())return {};
        const int factor=fine?8:1, count=n_*factor;
        double sum=0;
        for(int j=0;j<n_;++j) {
            auto v=iq_[pos+j]*(down?up_[j]:std::conj(up_[j]));
            in_[j]={v.real(),v.imag()};sum+=std::abs(v);
        }
        std::fill(in_.begin()+n_,in_.begin()+count,kiss_fft_cpx{0,0});
        kiss_fft(fine?fine_.get():coarse_.get(),in_.data(),out_.data());
        double best=0;int bin=0;
        for(int j=0;j<count;++j) {double mag=std::hypot(out_[j].r,out_[j].i);if(mag>best){best=mag;bin=j;}}
        return {signed_bin(double(bin)/factor,n_),best/(sum+1e-12)};
    }
};
}
Packet decode_symbols(const std::vector<double>& syms,int sf,bool ldro) {
    Packet p;p.sf=sf;p.ldro=ldro;
    if(sf<7 || sf>12) {p.detail="Explicit-header support is limited to SF7..12.";return p;}
    if(syms.size()<8){p.detail="Header truncated.";return p;}
    for(double x:syms)if(!std::isfinite(x)){p.detail="Non-finite symbol.";return p;}
    int n=1<<sf;
    std::vector<int> g;
    for(int k=0;k<8;++k)g.push_back(gray(int(std::llround(wrap(syms[k],n)))%n/4));
    auto cw=deinterleave(g,sf-2);
    std::vector<uint8_t> nib;
    for(auto c:cw)nib.push_back(unfec(c,4,p.fec_disagreements));
    p.declared_payload_len=nib[0]*16+nib[1];p.cr=nib[2]>>1;p.crc_on=nib[2]&1;
    if(p.cr<1 || p.cr>4 || header_sum(nib[0],nib[1],nib[2])!=((nib[3]&1)<<4|nib[4])) {
        p.detail="Explicit header checksum/fields rejected.";return p;
    }
    p.header_valid=true;
    std::vector<uint8_t> data(nib.begin()+5,nib.end());
    const int need=2*(p.declared_payload_len+(p.crc_on?2:0)), width=sf-(ldro?2:0), block=p.cr+4;
    int required=8+std::max(0,(need-int(data.size())+width-1)/width)*block;
    if(syms.size()<size_t(required)){p.detail="Header accepted; payload truncated.";return p;}
    for(int k=8;k<required;k+=block) {
        g.clear();
        for(int j=0;j<block;++j) {
            int bin=int(std::llround(wrap(syms[k+j],n)))%n;
            g.push_back(gray(ldro?bin/4:(bin+n-1)%n));
        }
        for(auto c:deinterleave(g,width))data.push_back(unfec(c,p.cr,p.fec_disagreements));
    }
    std::vector<uint8_t> bytes;
    for(int j=0;j<need;j+=2)bytes.push_back(data[j]|(data[j+1]<<4));
    uint8_t state=0xff;
    for(int j=0;j<p.declared_payload_len;++j) {
        bytes[j]^=state;
        int feedback=((state>>7)^(state>>5)^(state>>4)^(state>>3))&1;
        state=uint8_t((state<<1)|feedback);
    }
    p.payload.assign(bytes.begin(),bytes.begin()+p.declared_payload_len);
    p.payload_complete=true;p.end_sample=required; // symbol count until demodulator maps to samples
    if(p.crc_on)p.crc_valid=payload_crc(p.payload)==(bytes[p.declared_payload_len]|(bytes[p.declared_payload_len+1]<<8));
    p.detail=p.crc_on?(p.crc_valid?"Physical payload CRC valid; application protocol not interpreted.":"Physical payload CRC failed; bytes unverified."):
                         "Payload CRC absent; bytes are not integrity-verified.";
    return p;
}
std::vector<Packet> demodulate(const std::vector<std::complex<float>>& iq,int sf,double bw,const Options& opt) {
    std::vector<Packet> results;
    if(sf<7||sf>12||!std::isfinite(bw)||bw<=0)return results;
    if(opt.min_preamble_symbols<4||opt.max_preamble_symbols<opt.min_preamble_symbols||opt.max_preamble_symbols>4096||
       opt.max_packets<1||opt.max_packets>1024||!std::isfinite(opt.minimum_peak_ratio)||opt.minimum_peak_ratio<=0||opt.minimum_peak_ratio>1)
        throw std::invalid_argument("Invalid LoRa receiver options");
    int n=1<<sf;Analyzer a(iq,n);
    size_t windows=iq.size()/n;
    std::vector<Peak> peaks(windows);
    for(size_t i=0;i<windows;++i)peaks[i]=a.peak(long(i*n),false,false);
    for(size_t i=0;i+opt.min_preamble_symbols<windows && results.size()<size_t(opt.max_packets);++i) {
        bool run=true;
        for(int k=0;k<opt.min_preamble_symbols;++k) {
            if(peaks[i+k].ratio<opt.minimum_peak_ratio || (k && std::abs(signed_bin(peaks[i+k].bin-peaks[i+k-1].bin,n))>1.5)){run=false;break;}
        }
        if(!run)continue;
        long sfd=-1;
        for(size_t j=i+opt.min_preamble_symbols;j<std::min(windows,i+size_t(opt.max_preamble_symbols));++j) {
            auto d=a.peak(long(j*n),true,false);
            if(d.ratio>opt.minimum_peak_ratio && d.ratio>peaks[j].ratio){sfd=long(j*n);break;}
        }
        if(sfd<0)continue;
        double u=a.peak(sfd-4*n).bin, d=a.peak(sfd+n,true).bin;
        // Both slopes resolve timing/CFO up to an N/2 ambiguity. Select by
        // preamble + two full SFD chirp correlations, not by guessed payload text.
        long aligned=-1;double score=0;
        for(int branch:{-1,0,1}) {
            long pos=sfd+std::lround((d-u)/2+branch*n/2.);
            for(int symbol:{-1,0,1}) {
                long p=pos+symbol*n;
                auto pre=a.peak(p-3*n),d0=a.peak(p,true),d1=a.peak(p+n,true);
                double value=pre.ratio+d0.ratio+d1.ratio;
                if(pre.ratio>opt.minimum_peak_ratio && d0.ratio>opt.minimum_peak_ratio && d1.ratio>opt.minimum_peak_ratio && value>score){score=value;aligned=p;}
            }
        }
        if(aligned<0){i=size_t(sfd/n);continue;}
        double sy=0,sxy=0,sx=0,sxx=0;
        double origin=a.peak(aligned-3*n).bin;
        for(int j=-8;j<=-3;++j) {
            double y=origin+signed_bin(a.peak(aligned+j*n).bin-origin,n);
            sy+=y;sxy+=j*y;sx+=j;sxx+=j*j;
        }
        double slope=(6*sxy-sx*sy)/(6*sxx-sx*sx),intercept=(sy-slope*sx)/6;
        long data_start=aligned+9*n/4;
        // The largest explicit LoRa packet is bounded by its one-byte length.
        size_t available=data_start<0||size_t(data_start)>iq.size()?0:(iq.size()-data_start)/n;
        available=std::min(available,size_t(1100));
        std::vector<double> raw;raw.reserve(available);
        for(size_t k=0;k<available;++k) {
            auto peak = a.peak(data_start+long(k*n));
            if (peak.ratio < 1e-6) break; // missing/zero samples are erasures, not bin zero
            raw.push_back(wrap(peak.bin-intercept,n));
        }
        Packet best;bool have=false;
        // Payload LDRO is not signalled in the header. Evaluate both bounded
        // hypotheses; CRC resolves them when possible, otherwise retain ambiguity.
        bool preferred=(double(n)/bw)>0.016;
        for(bool ldro:{preferred,!preferred}) {
            std::vector<double> corrected;corrected.reserve(raw.size());double offset=0,last=1;
            for(size_t k=0;k<raw.size();++k) {
                double v=raw[k];
                if(k<8 || ldro) {
                    double delta=wrap(v-last,4);offset-=delta<2?delta:delta-4;last=v;
                    v=wrap(v+offset,n);
                } else v=wrap(v-slope*(k+2.25),n);
                corrected.push_back(v);
            }
            auto p=decode_symbols(corrected,sf,ldro);
            if(!p.header_valid)continue;
            p.ldro_ambiguous=!(p.crc_on&&p.crc_valid);
            if(!have || (p.crc_valid&&!best.crc_valid) || (p.payload_complete&&!best.payload_complete)) {best=std::move(p);have=true;}
        }
        if(!have){i=size_t(sfd/n);continue;}
        best.start_sample=long(i*n);best.data_start_sample=data_start;
        best.end_sample=best.payload_complete?data_start+best.end_sample*n:long(iq.size());
        best.cfo_bins=intercept;best.drift_bins_per_symbol=slope;
        for(int k=0;k<2;++k)best.sync_bins[k]=wrap(a.peak(aligned+(k-2)*n).bin-(intercept+slope*(k-2)),n);
        int hi=int(std::lround(best.sync_bins[0]/8)),lo=int(std::lround(best.sync_bins[1]/8));
        if(hi>=0&&hi<16&&lo>=0&&lo<16&&std::abs(best.sync_bins[0]-8*hi)<1.5&&std::abs(best.sync_bins[1]-8*lo)<1.5)
            best.sync_word=(hi<<4)|lo;
        results.push_back(best);
        i=std::max(i,size_t(std::max(0L,best.end_sample)/n)-1);
    }
    return results;
}
} // namespace rfmon::lora::receiver
