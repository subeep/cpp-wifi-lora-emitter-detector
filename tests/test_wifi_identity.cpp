// Byte-level IE fixtures plus persistent identity integration. No radio or real data writes.
#include "wifi_frame.hpp"
#include "wifi_master.hpp"
#include "wifi_vendor.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace rfmon;
using Bytes = std::vector<uint8_t>;
int failures = 0, checks = 0;
void check(bool ok, const char* name) {
    ++checks; if (!ok) ++failures;
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
}
void ie(Bytes& body, uint8_t tag, const Bytes& data) {
    body.push_back(tag); body.push_back(uint8_t(data.size())); body.insert(body.end(), data.begin(), data.end());
}
Bytes frame(const Bytes& ies, uint16_t caps=0x11, bool probe=false) {
    Bytes f; f.reserve(36 + ies.size() + 4); f.resize(36, 0); f[0] = probe ? 0x50 : 0x80;
    const uint8_t mac[] = {0x3c,0x52,0xa1,0x0b,0xbf,0xd7};
    std::copy(std::begin(mac),std::end(mac),f.begin()+16);
    f[32]=100; f[34]=uint8_t(caps); f[35]=uint8_t(caps>>8);
    f.insert(f.end(),ies.begin(),ies.end());
    auto crc=wifi::fcs32(f.data(),f.size());
    for (int i=0;i<4;++i) f.push_back(uint8_t(crc>>(8*i)));
    return f;
}
wifi::BeaconInfo decode(const Bytes& ies, uint16_t caps=0x11, bool probe=false) {
    auto f=frame(ies,caps,probe);
    auto b=wifi::parse_beacon(f.data(),f.size());
    if (!b) throw std::runtime_error("Fixture decode failed");
    return *b;
}
// Version 1, CCMP group, one CCMP pairwise suite, one PSK AKM, PMF capable.
const Bytes rsn={1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,128,0};
void wps_attr(Bytes& w, uint16_t tag, const std::string& s) {
    w.push_back(uint8_t(tag>>8)); w.push_back(uint8_t(tag));
    w.push_back(uint8_t(s.size()>>8)); w.push_back(uint8_t(s.size()));
    w.insert(w.end(),s.begin(),s.end());
}
int main() {
    namespace fs=std::filesystem;
    Bytes ies;
    ie(ies,0,{'L','a','b','"','\\',0,0xff}); ie(ies,3,{9}); ie(ies,48,rsn);
    ie(ies,45,Bytes(26)); ie(ies,191,Bytes(12));
    Bytes wps; wps_attr(wps,0x1021,"Example Corp"); wps_attr(wps,0x1023,"Model A");
    wps_attr(wps,0x1024,"42"); wps_attr(wps,0x1011,"Lab AP");
    // Split in the middle of an attribute, independently of its TLV boundaries.
    Bytes w1={0,0x50,0xf2,4}, w2=w1;
    w1.insert(w1.end(),wps.begin(),wps.begin()+7); w2.insert(w2.end(),wps.begin()+7,wps.end());
    ie(ies,221,w1); ie(ies,221,w2);
    auto b=decode(ies,0x11,true);
    check(b.fcs_valid && b.ies_complete,"valid rich probe response");
    check(b.frame_source=="Probe response" && b.channel==9 && b.channel_source=="DS Parameter Set","frame and channel provenance");
    check(b.security.find("PSK")!=std::string::npos && b.ciphers.find("CCMP")!=std::string::npos && b.pmf=="Capable","RSN PSK CCMP PMF");
    check(b.standards=="HT (802.11n), VHT (802.11ac)","HT VHT advertised capabilities");
    check(b.wps_manufacturer=="Example Corp" && b.wps_model_name=="Model A" && b.wps_model_number=="42" && b.wps_device_name=="Lab AP","fragmented WPS hints");
    check(wifi::display_text(b.ssid).find("\\x00\\xFF")!=std::string::npos,"arbitrary SSID octets display safely");
    auto open=decode({},1); check(open.security.find("Open")!=std::string::npos,"open without privacy");
    check(decode({}).security=="Privacy set (legacy/unknown)","privacy alone does not assert WEP/WPA");
    Bytes transition=rsn; transition[12]=2; transition.insert(transition.begin()+18,{0,15,172,8});
    Bytes t; ie(t,48,transition); auto tb=decode(t);
    check(tb.security.find("PSK")!=std::string::npos && tb.security.find("SAE")!=std::string::npos,"PSK SAE transition advertisement");
    Bytes owe=rsn; owe[17]=18; owe[18]=192; t.clear(); ie(t,48,owe); tb=decode(t);
    check(tb.security.find("OWE")!=std::string::npos && tb.pmf=="Required","OWE and required PMF");
    Bytes enterprise=rsn; enterprise[17]=1; t.clear(); ie(t,48,enterprise);
    check(decode(t).security.find("802.1X")!=std::string::npos,"enterprise AKM");
    Bytes unknown=rsn; unknown[15]=0x12; t.clear(); ie(t,48,unknown);
    check(decode(t).security.find("Unknown")!=std::string::npos,"unknown AKM retained honestly");
    Bytes wpa={0,0x50,0xf2,1,1,0,0,0x50,0xf2,2,1,0,0,0x50,0xf2,2,1,0,0,0x50,0xf2,2};
    t.clear(); ie(t,221,wpa); ie(t,48,rsn);
    check(decode(t).security.find("WPA:")!=std::string::npos && decode(t).security.find("RSN:")!=std::string::npos,"WPA RSN mixed advertisement");
    Bytes ht(22); ht[0]=11; t.clear(); ie(t,61,ht);
    check(decode(t).channel==11 && decode(t).channel_source=="HT Operation","HT channel fallback");
    ie(t,3,{6}); check(decode(t).channel==6,"DS channel takes precedence");
    bool malformed_ok=true;
    // Every prefix before the mandatory AKM list is incomplete; no out-of-bounds reads.
    for (size_t n=0;n<18;++n) {
        t.clear(); ie(t,48,Bytes(rsn.begin(),rsn.begin()+n)); auto bad=decode(t);
        malformed_ok &= !bad.ies_complete && bad.security.find("malformed")!=std::string::npos;
    }
    check(malformed_ok,"all mandatory RSN truncations rejected");
    t.clear(); Bytes count=rsn; count[6]=255; count[7]=255; ie(t,48,count);
    check(!decode(t).ies_complete,"oversized suite count bounded");
    check(!decode({0,5,'a'},1).ies_complete && decode({0,5,'a'},1).security.find("Unknown")!=std::string::npos,"truncated IE cannot imply open");
    check(!decode({0},1).ies_complete,"trailing IE tag detected");
    t.clear(); ie(t,0,Bytes(33,'x')); check(!decode(t).ssid_present && !decode(t).ies_complete,"oversized SSID rejected");
    t.clear(); ie(t,221,{0,0x50,0xf2,4,0x10,0x23,0,9,'x'});
    check(!decode(t).ies_complete && decode(t).wps_model_name.empty(),"truncated WPS attribute not trusted");
    t.clear(); ie(t,45,Bytes(1)); ie(t,191,Bytes(11));
    check(decode(t).standards.empty() && !decode(t).ies_complete,"short capability IEs not advertised as standards");

    check(wifi::lookup_vendor("3C:52:A1:0B:BF:D7").name!="Unknown","real IEEE vendor lookup and MAC normalization");
    check(wifi::lookup_vendor("02:52:a1:00:00:01").name=="Locally administered","local MAC has no invented vendor");
    check(wifi::lookup_vendor("01:00:5e:00:00:01").name=="Not applicable","group MAC has no invented vendor");
    check(wifi::lookup_vendor("c8:5c:e2:70:00:00").name=="SYNERGY SYSTEMS AND SOLUTIONS", "MA-M /28 overrides parent /24");
    check(wifi::lookup_vendor("8c:1f:64:af:a0:00").name=="DATA ELECTRONIC DEVICES, INC", "MA-S /36 overrides parent /24");
    check(!wifi::canonical_mac("../unsafe") && wifi::lookup_vendor("bogus").name=="Unknown","invalid MAC rejected");

    auto dir=fs::temp_directory_path()/"rf-wifi-identity-tests";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        wifi_master::WifiMasterList list(dir.string());
        auto bad=b; bad.fcs_valid=false;
        check(list.record_identity(bad,2412e6,50).empty() && list.snapshot().empty(),"invalid FCS never persisted");
        auto key=list.record_identity(b,2412e6,100);
        wifi_fingerprint::WifiFingerprint fp; fp.gated_out=true;
        list.record_reading(key,"DSSS",2412e6,22e6,fp,101);
        auto row=list.snapshot().at(0);
        check(row.reading_count==0 && row.identity_count==1 && row.identity->channel==9 && row.monitored_channel_hz==2412e6,"identity survives rejected fingerprint and preserves both channels");
    }
    {
        wifi_master::WifiMasterList list(dir.string()); auto row=list.snapshot().at(0);
        check(row.identity && row.reading_count==0 && row.first_seen_ts==100 && row.last_seen_ts==100,"identity-only restart");
        check(row.identity->ssid==b.ssid && row.identity->wps_model_name=="Model A","exact octets and WPS persist");
        Bytes hidden; ie(hidden,0,{}); ie(hidden,3,{11}); auto hb=decode(hidden);
        list.record_identity(hb,2462e6,200); row=list.snapshot().at(0);
        check(row.identity->ssid==b.ssid && row.ssid_seen_ts==100 && row.ssid_source=="Probe response" && row.identity_ts==200,"hidden beacon retains learned SSID with original time");
        check(row.identity->wps_model_name=="Model A" && row.wps_seen_ts==100 && !row.identity->wps_present,"omitted WPS keeps last-known hints with timestamp");
        check(row.identity->channel==11 && row.monitored_channel_hz==2462e6,"channel updates independently of name");
        auto renamed=b; renamed.ssid="New name";
        list.record_identity(renamed,2437e6,300); list.record_identity(b,2437e6,250);
        check(list.snapshot().at(0).identity->ssid=="New name","new names update and stale observations do not overwrite");
        wifi_fingerprint::WifiFingerprint fp; fp.irr_db=-30; fp.dc_dbc=-40;
        list.record_reading(b.bssid,"DSSS",2437e6,22e6,fp,400);
        for (int i=0;i<150;++i) list.record_identity(renamed,2437e6,500+i);
        check(list.snapshot().size()==1 && list.snapshot().at(0).reading_count==1,"identity and RF observations join same BSSID");
    }
    {
        wifi_master::WifiMasterList list(dir.string()); auto row=list.snapshot().at(0);
        check(row.identity->ssid=="New name" && row.identity_count==153 && row.reading_count==1 && row.last_seen_ts==649 && row.first_seen_ts==100,"identity compaction preserves counts metadata and chronology");
        wifi_fingerprint::WifiFingerprint fp; fp.irr_db=-30; fp.dc_dbc=-40;
        for (int i=0;i<1105;++i) list.record_reading(b.bssid,"DSSS",2437e6,22e6,fp,700+i);
    }
    {
        wifi_master::WifiMasterList list(dir.string()); auto row=list.snapshot().at(0);
        check(row.identity && row.identity->ssid=="New name" && row.identity_count==153 && row.reading_count==1000,"RF compaction preserves identity");
    }
    // A manually written pre-change file (not produced by the current serializer).
    auto legacy=dir/"legacy"; fs::create_directories(legacy);
    {
        std::ofstream out(legacy/"00:11:22:33:44:55.ndjson");
        out << R"({"meta":1,"key":"00:11:22:33:44:55","key_is_mac":true,"first_seen_ts":7})" << '\n';
        out << R"({"ts":10,"channel_hz":2412000000,"bandwidth_hz":20000000,"phy":"OFDM","cfo_ppm":5,"irr_db":-30,"iq_eps":0.01,"iq_phi_deg":1,"dc_dbc":-40,"dc_ang_deg":3,"snr_db":20,"evm_pct":10,"sync_corr":0.9})" << '\n';
        out << "{broken trailing record";
    }
    {
        wifi_master::WifiMasterList list(legacy.string()); auto rows=list.snapshot();
        check(rows.size()==1 && !rows[0].identity && rows[0].reading_count==1 && rows[0].first_seen_ts==7,"legacy file loads with malformed tail");
        check(!list.storage_error().empty(),"malformed record surfaced");
        auto migrated=b; migrated.bssid="00:11:22:33:44:55"; list.record_identity(migrated,2437e6,20);
    }
    {
        wifi_master::WifiMasterList list(legacy.string()); auto row=list.snapshot().at(0);
        check(row.identity && row.first_seen_ts==7 && row.reading_count==1,"legacy record enriched without losing history");
    }
    auto ofdm_dir=dir/"ofdm";
    {
        wifi_master::WifiMasterList list(ofdm_dir.string());
        for(int i=0;i<110;++i) list.record_identity(b,5180e6,100+i,"OFDM");
        wifi_fingerprint::WifiFingerprint fp;
        list.record_reading(b.bssid,"DSSS",2412e6,22e6,fp,300);
    }
    {
        wifi_master::WifiMasterList list(ofdm_dir.string());auto row=list.snapshot().at(0);
        check(row.identity_phy=="OFDM" && row.last_phy=="DSSS" && row.identity_count==110,
              "OFDM identity provenance survives compaction and newer DSSS reading");
    }
    // Schema-2 identity records predating OFDM omitted phy and mean DSSS.
    auto path=ofdm_dir/(b.bssid+".ndjson");std::vector<nlohmann::json> old_lines;
    {std::ifstream in(path);std::string line;while(std::getline(in,line)){auto j=nlohmann::json::parse(line);if(j.value("type","")=="identity")j.erase("phy");old_lines.push_back(j);}}
    {std::ofstream out(path);for(const auto& j:old_lines)out<<j.dump()<<'\n';}
    {
        wifi_master::WifiMasterList list(ofdm_dir.string());
        check(list.snapshot().at(0).identity_phy=="DSSS","old identity records default to DSSS");
    }
    auto unwritable=dir/"not-a-directory"; { std::ofstream out(unwritable); out << 'x'; }
    wifi_master::WifiMasterList unavailable(unwritable.string());
    unavailable.record_identity(b,2412e6,100);
    check(!unavailable.storage_error().empty(),"storage failure surfaced without losing in-memory identity");
    fs::remove_all(dir);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
