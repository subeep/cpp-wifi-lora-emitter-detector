// Real received beacons + software vectors. Never opens a radio or live data store.
#include "wifi_ofdm_rx.hpp"
#include "wifi_phy.hpp"
#include "wifi_master.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <cstring>
#include <unistd.h>
using namespace rfmon;
namespace fs=std::filesystem;
int main(int argc,char**argv) {
    int failures=0,checks=0;
    auto check=[&](bool ok,const std::string& name) {++checks;if(!ok)++failures;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';};
    auto dir=fs::temp_directory_path()/("wifi-ofdm-test-"+std::to_string(getpid()));
    fs::remove_all(dir);
    try {
        for(int arg=1;arg<argc;++arg)for(auto& entry:fs::directory_iterator(argv[arg])) {
            if(entry.path().extension()!=".json")continue;
            nlohmann::json j;std::ifstream(entry.path())>>j;
            std::ifstream raw(entry.path().parent_path()/j.at("iq_file").get<std::string>(),std::ios::binary);
            size_t n=j.at("samples");std::vector<std::complex<float>> iq(n);
            for(auto& v:iq){float f[2];for(int c=0;c<2;++c){uint32_t u=0;for(int b=0;b<4;++b){int byte=raw.get();if(byte<0)throw std::runtime_error("short fixture");u|=uint32_t(byte)<<(8*b);}std::memcpy(&f[c],&u,4);}v={f[0],f[1]};}
            double rate=j.at("sample_rate_hz"),center=j.at("capture_center_hz"),channel=j.at("channel_hz");
            auto r=wifi::decode_ofdm_burst(iq.data(),n,rate,center,channel);
            std::string name=entry.path().stem();bool expected=j.at("expected_fcs");
            check(r.fcs_valid==expected,name+": "+r.status);
            if(j.contains("expected_format")){
                // Live 802.11n/ac/ax header fixtures: CRC-valid header, payload not decoded.
                const bool ok=r.sig&&r.sig->crc_ok&&r.sig->format==j.at("expected_format")&&r.sig->mcs==j.at("expected_mcs")&&
                    r.sig->bandwidth_mhz==j.at("expected_bandwidth_mhz")&&r.sig->streams==j.at("expected_streams")&&
                    r.sig->bss_color==j.at("expected_bss_color")&&r.sig->group_id==j.at("expected_group_id")&&
                    r.sig->partial_aid==j.at("expected_partial_aid")&&r.sig->ht_length==j.at("expected_ht_length");
                check(ok,name+" header fields: "+(r.sig?r.sig->summary:std::string("no header")));
                check(!r.fcs_valid&&!r.beacon&&r.status.find("payload not decoded")!=std::string::npos,name+" payload not claimed");
                auto damaged=iq;for(size_t i=0;i<damaged.size();++i)if(i%7==0)damaged[i]*=-1.f;
                auto d=wifi::decode_ofdm_burst(damaged.data(),n,rate,center,channel);
                check(!(d.sig&&d.sig->crc_ok&&d.sig->summary!=r.sig->summary),name+" corrupted IQ never yields a different CRC-valid header");
                continue;
            }
            if(!expected){check(!r.beacon,name+" rejected identity");continue;}
            std::string hex;for(auto b:r.mpdu){hex+="0123456789abcdef"[b>>4];hex+="0123456789abcdef"[b&15];}
            check(hex==j.at("expected_mpdu_hex")&&r.rate_mbps==j.at("expected_rate_mbps"),name+" exact MPDU and rate");
            check(r.beacon&&r.beacon->fcs_valid,name+" verified beacon parser");
            // The production scanner first classifies and checks occupied bandwidth.
            auto cls=wifi::classify_modulation(iq,rate,center,channel,channel<3e9);
            check(cls.mod==wifi::ModClass::OFDM && wifi::estimate_occupied_bandwidth_hz(iq.data(),n,rate)>=wifi::MIN_WIFI_BANDWIDTH_HZ,name+" scanner eligibility");
            if(r.beacon){
                {wifi_master::WifiMasterList list(dir.string());check(!list.record_identity(*r.beacon,channel,100,"OFDM").empty(),name+" identity saved without RF acceptance");}
                {wifi_master::WifiMasterList list(dir.string());auto rows=list.snapshot();auto it=std::find_if(rows.begin(),rows.end(),[&](auto& v){return v.device_key==r.beacon->bssid;});check(it!=rows.end()&&it->identity_phy=="OFDM"&&it->last_phy=="OFDM"&&it->reading_count==0,name+" OFDM identity restart");}
            }
            auto short_r=wifi::decode_ofdm_burst(iq.data(),std::min(n,size_t(rate*18e-6)),rate,center,channel);
            check(!short_r.fcs_valid&&!short_r.beacon,name+" truncated rejection");
        }
        // 802.11ac partial AID of a frame to an AP: BSSID bits 39..47.
        check(wifi::vht_partial_aid("98:ba:5f:37:57:4b")==150&&wifi::vht_partial_aid("00:00:00:00:80:00")==1&&wifi::vht_partial_aid("00:00:00:00:00:80")==256&&
              wifi::vht_partial_aid("00:00:00:00:00:01")==2&&wifi::vht_partial_aid("bad")==-1,"VHT partial AID from BSSID");
        {
            // Beacon with an HE Operation element (BSS colour 12) and a disabled-colour variant.
            auto beacon=[](uint8_t color_info){
                std::vector<uint8_t> m={0x80,0x00,0,0, 0xff,0xff,0xff,0xff,0xff,0xff, 2,0,0,0,0,1, 2,0,0,0,0,1, 0x10,0};
                for(int i=0;i<8;++i)m.push_back(0);
                m.insert(m.end(),{0x64,0x00,0x11,0x04, 0,4,'T','E','S','T', 255,7,36,0x00,0x00,0x00,color_info,0xfc,0xff});
                const uint32_t f=wifi::fcs32(m.data(),m.size());for(int i=0;i<4;++i)m.push_back(uint8_t(f>>(8*i)));
                return wifi::parse_beacon(m.data(),m.size());
            };
            auto on=beacon(0x0c),off=beacon(0x8c);
            check(on&&on->fcs_valid&&on->bss_color==12&&!on->bss_color_disabled&&off&&off->bss_color==12&&off->bss_color_disabled,
                  "HE Operation BSS colour parsed from beacon");
        }
        std::mt19937 rng(42);std::normal_distribution<float> noise(0,.1);std::vector<std::complex<float>> iq(20000);
        for(auto&v:iq)v={noise(rng),noise(rng)};
        check(!wifi::decode_ofdm_burst(iq.data(),iq.size(),20e6,5180e6,5180e6).fcs_valid,"noise rejection");
        iq[500]={NAN,0};check(wifi::decode_ofdm_burst(iq.data(),iq.size(),20e6,0,0).status=="Non-finite IQ","NaN rejection");
        check(!wifi::decode_ofdm_burst(nullptr,400,20e6,0,0).fcs_valid,"null rejection");
        check(!wifi::decode_ofdm_burst(iq.data(),iq.size(),10e6,0,0).fcs_valid,"unsupported rate rejection");
        check(checks>20,"fixtures supplied");
    }catch(const std::exception& e){++failures;std::cerr<<e.what()<<'\n';}
    fs::remove_all(dir);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
