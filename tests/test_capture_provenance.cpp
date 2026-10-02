#include "capture_timing_json.hpp"
#include "lora_capture.hpp"
#include "security/wifi_iq_input.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace rfmon;
namespace fs=std::filesystem;
void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);std::cout<<"PASS "<<msg<<'\n';}
template<class F>void rejected(F f){bool fail=false;try{f();}catch(const std::exception&){fail=true;}check(fail,"Malformed evidence rejected");}
int main(){char tmp[]="/tmp/rfmon-capture-provenance-XXXXXX";if(!mkdtemp(tmp))return 1;
 try{
    CaptureTiming timing;timing.device_time_valid=true;timing.device_time_ns=5000000000;
    timing.host_before_ns=1700000000000000000;timing.host_after_ns=timing.host_before_ns+200000;
    timing.requested_samples=8;timing.gain_db=20;timing.actual_rf_hz=2412e6;timing.retuned=false;
    timing.overflows={{3,true,6000000000},{6,false,0}};
    auto restored=capture_timing_from_json(nlohmann::json::parse(capture_timing_json(timing).dump()),8);
    check(capture_sample_time(restored,1e6,8,2)==5000002000,"Pre-overflow hardware anchor");
    check(capture_sample_time(restored,1e6,8,3)==6000000000,"Overflow boundary uses resume anchor");
    check(!capture_sample_time(restored,1e6,8,6)&&!capture_sample_time(restored,1e6,8,8),"Unknown gap/out-of-range sample has no time");
    timing.overflows.clear();std::vector<std::complex<float>> iq(8,{1,-2});
    nlohmann::json j={{"run_id","shared-device-session"},{"radio_session",3},{"capture_seq",1},
        {"iq_file","first.cf32"},{"sample_rate_hz",20e6},{"channel_hz",2412e6},{"capture_center_hz",2412e6}};
    fs::path manifest=fs::path(tmp)/"first.json";save_wifi_iq_capture(manifest.string(),j,iq,timing);
    auto saved=load_wifi_iq_capture(manifest.string());auto rec=wifi_security::wifi_iq_record(saved,"first.json",99);
    check(saved.iq==iq&&rec.capture_seq==1&&rec.radio_session==3&&rec.clock==wifi_security::ClockDomain::UsrpDevice,
        "Wi-Fi schema 2 preserves sample bytes and ingest identity");
    check(rec.device_time_ns==timing.device_time_ns&&rec.host_after_ns==timing.host_after_ns&&rec.gain_db==20&&!rec.retuned,
        "Wi-Fi replay preserves hardware/host/gain/retune provenance");
    j["iq_file"]="second.cf32";j["capture_seq"]=2;timing.device_time_ns+=1000000000;
    save_wifi_iq_capture((fs::path(tmp)/"second.json").string(),j,iq,timing);
    auto second=wifi_security::wifi_iq_record(load_wifi_iq_capture((fs::path(tmp)/"second.json").string()),"second.json",1);
    check(rec.run_id==second.run_id&&second.capture_seq==2&&second.device_time_ns-rec.device_time_ns==1000000000,
        "Separate IQ files retain one comparable radio session");
    auto legacy=saved.metadata;legacy["schema"]=1;legacy.erase("timing");legacy.erase("run_id");legacy.erase("capture_seq");
    legacy["host_start_ns"]=1700000000000000000;
    auto write=[&](const nlohmann::json& value){std::ofstream(manifest)<<value.dump();};
    write(legacy);auto old=load_wifi_iq_capture(manifest.string());auto oldrec=wifi_security::wifi_iq_record(old,"first.json",1);
    check(!old.timing&&oldrec.clock==wifi_security::ClockDomain::Unknown&&!wifi_security::device_time_of_sample(oldrec,1),
        "Legacy Wi-Fi capture readable without fabricated device time");
    auto bad=saved.metadata;bad["samples"]=-1;write(bad);rejected([&]{load_wifi_iq_capture(manifest.string());});
    bad=saved.metadata;bad["samples"]=uint64_t(-1);write(bad);rejected([&]{load_wifi_iq_capture(manifest.string());});
    bad=saved.metadata;bad["iq_file"]="../outside.cf32";write(bad);rejected([&]{load_wifi_iq_capture(manifest.string());});
    bad=saved.metadata;bad["timing"]["overflows"]={{{"at_sample",9},{"resume_device_ns",nullptr}}};write(bad);rejected([&]{load_wifi_iq_capture(manifest.string());});
    bad=saved.metadata;bad["timing"]["device_time_ns"]=std::numeric_limits<int64_t>::max();write(bad);
    rejected([&]{load_wifi_iq_capture(manifest.string());});
    write(saved.metadata);{std::fstream f(fs::path(tmp)/"first.cf32",std::ios::in|std::ios::out|std::ios::binary);f.put('x');}
    rejected([&]{load_wifi_iq_capture(manifest.string());});
    LoraCapture l;l.iq=iq;l.sample_rate_hz=l.requested_sample_rate_hz=500000;
    l.requested_center_hz=866900000;l.requested_duration_s=.001;l.run_id="LoRa-live";l.capture_seq=9;l.radio_session=2;
    l.timing=timing;l.host_start_unix_s=10;
    auto dir=save_lora_capture(tmp,l);auto lr=load_lora_capture(dir);
    check(lr.timing&&lr.timing->device_time_ns==timing.device_time_ns&&lr.capture_seq==9&&lr.radio_session==2,"LoRa schema 2 preserves radio session/timing");
    auto cropped=crop_lora_capture(lr,2,6);
    check(cropped.iq.size()==4&&cropped.timing->device_time_ns==timing.device_time_ns+4000&&cropped.host_start_unix_s==10,
        "Contiguous crop shifts sample anchors but never invents a legacy host sample timestamp");
    check(cropped.run_id!=lr.run_id,"Crop has distinct ingestion identity");
    lr.overflow=true;lr.timing->overflows={{3,false,0}};cropped=crop_lora_capture(lr,4,6);
    check(cropped.overflow&&!cropped.timing->device_time_valid,"Discontinuous crop cannot restore precision");
    auto lp=fs::path(dir)/"manifest.json";nlohmann::json lm;std::ifstream(lp)>>lm;lm["version"]=1;
    lm.erase("timing");lm.erase("run_id");lm.erase("capture_seq");lm.erase("radio_session");std::ofstream(lp)<<lm.dump();
    check(!load_lora_capture(dir).timing,"Legacy LoRa v1 remains readable");
    fs::remove_all(tmp);return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" ("<<tmp<<")\n";return 1;}}
