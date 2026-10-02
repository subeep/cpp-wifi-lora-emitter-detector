#include "security/lora_security.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace rfmon;
using namespace rfmon::lora_security;
void check(bool b,const char* msg){if(!b)throw std::runtime_error(msg);std::cout<<"PASS "<<msg<<'\n';}
LoraCapture capture(){LoraCapture c;c.run_id="unit-fixture";c.capture_seq=1;c.radio_session=1;c.sample_rate_hz=500000;
 c.requested_center_hz=866900000;c.device_args="fixture";c.antenna="none";
 CaptureTiming t;t.device_time_valid=true;t.device_time_ns=5000000000;t.requested_samples=100000;c.timing=t;return c;}
LoraPacketRow row(){LoraPacketRow r;r.decoder="LoRa explicit PHY";r.sf=7;r.cr=1;r.bandwidth_khz=125;
 r.decoder_sample_start=200;r.decoder_sample_end=2000;r.start_uncertainty_samples=128;
 r.header_valid=r.payload_complete=true;r.crc=LoraPacketRow::Crc::Valid;r.payload_hex="01 02 03";return r;}
int main(){char tmp[]="/tmp/rfmon-lora-security-XXXXXX";if(!mkdtemp(tmp))return 1;
 try{
    auto c=capture();auto r=row();auto b=make_batch(c,100000,{r,r});
    check(b.events.size()==1&&b.capture.deduplicated==1,"Identical decoder hypotheses collapse within one physical sample span");
    check(b.events.front().sample_start==800&&b.events.front().device_time_ns==5001600000&&b.events.front().start_uncertainty_samples==512,
        "Decimated sample mapping preserves coarse packet-start uncertainty");
    auto again=r;again.decoder_sample_start=3000;again.decoder_sample_end=4800;
    b=make_batch(c,100000,{r,again});check(b.events.size()==2,"Same bytes received at different samples remain separate transmissions");
    State direct,offline;direct.ingest(b);offline.ingest(from_json(nlohmann::json::parse(to_json(b).dump())));
    check(snapshot_json(direct.snapshot())==snapshot_json(offline.snapshot()),"Serialized replay equals live-state ingestion");
    direct.ingest(b);check(direct.snapshot().captures==1&&direct.snapshot().duplicates==1,"Repeated capture ingestion cannot inflate exposure");
    auto badcrc=r;badcrc.crc=LoraPacketRow::Crc::Failed;auto absent=r;absent.crc=LoraPacketRow::Crc::Absent;
    auto partial=r;partial.payload_complete=false;partial.crc=LoraPacketRow::Crc::NotChecked;partial.payload_hex.clear();
    c.capture_seq=2;c.timing->device_time_ns+=300000000;auto failures=make_batch(c,100000,{badcrc,absent,partial});direct.ingest(failures);
    check(direct.snapshot().eligible==2&&direct.snapshot().crc_failed==1&&direct.snapshot().crc_absent==1&&direct.snapshot().partial==1,
        "Failed/absent CRC and partial bytes remain evidence without protocol eligibility");
    for(int variant=0;variant<5;++variant){auto degraded=c;degraded.capture_seq=3+variant;
        auto rr=r;if(variant==0){degraded.overflow=true;degraded.timing->overflows={{500,false,0}};}
        if(variant==1)degraded.timing->timed_out=true;if(variant==2)rr.analysis_limited=true;
        if(variant==3)degraded.timing->exception=true;
        auto batch=make_batch(degraded,100000,{rr},variant==4);
        State s;s.ingest(batch);check(s.snapshot().excluded==1&&s.snapshot().usable_s==0,"Loss/cap/laboratory data cannot supply usable security exposure");}
    State silent;auto empty=make_batch(c,0,{});silent.ingest(empty);
    check(silent.snapshot().excluded==1&&silent.snapshot().events==0,"Failed empty capture is retained, never treated as quiet coverage");
    State queueState;queueState.note_loss(2);check(queueState.snapshot().input_loss==2,"Capture queue losses exposed explicitly");
    auto record=std::string(tmp)+"/events.ndjson";Monitor monitor(record);monitor.start();monitor.submit(b);monitor.submit(failures);monitor.stop();
    State expected;expected.ingest(b);expected.ingest(failures);
    check(snapshot_json(expected.snapshot())==snapshot_json(*monitor.snapshot()),"Threaded monitor drains to identical offline evidence");
    State recorded;std::ifstream in(record);std::string line;while(std::getline(in,line))recorded.ingest(from_json(nlohmann::json::parse(line)));
    check(snapshot_json(recorded.snapshot())==snapshot_json(expected.snapshot()),"Persisted capture-batch stream replays exactly");
    auto malformed=to_json(b);malformed["events"][0]["device_time_ns"]=123;
    bool rejected=false;try{from_json(malformed);}catch(const std::exception&){rejected=true;}
    check(rejected,"Forged event receive time rejected against sample provenance");
    malformed=to_json(b);malformed["events"][0]["payload"]={256};rejected=false;try{from_json(malformed);}catch(const std::exception&){rejected=true;}
    check(rejected,"Out-of-range payload byte rejected instead of silently truncated");
    for(uint64_t n=3;n<400;++n){c.capture_seq=n;c.timing->device_time_ns+=300000000;expected.ingest(make_batch(c,100000,{r}));}
    check(expected.snapshot().recent.size()==256,"Retained event memory remains bounded across long runs");
    Monitor small({},1);check(!small.submit(b),"Inactive consumer rejects capture explicitly");small.start();small.stop();
    check(small.snapshot()->input_loss==1,"Rejected queue input survives consumer startup");
    Monitor rotating(std::string(tmp)+"/rotating.ndjson",32,8192);rotating.start();
    for(uint64_t n=10;n<30;++n){c.capture_seq=n;c.timing->device_time_ns+=300000000;rotating.submit(make_batch(c,100000,{r}));}rotating.stop();
    check(std::filesystem::file_size(std::string(tmp)+"/rotating.ndjson")<=4096&&std::filesystem::exists(std::string(tmp)+"/rotating.ndjson.1"),"Recording rotation bounds disk use");
    {
        State clocks;auto base=capture();clocks.ingest(make_batch(base,100000,{r}));
        base.capture_seq=2;clocks.ingest(make_batch(base,100000,{r}));
        check(clocks.snapshot().time_regressions==1&&clocks.snapshot().excluded==1,
            "Overlapping device timestamps cannot inflate usable exposure");
        base.capture_seq=3;base.radio_session=2;clocks.ingest(make_batch(base,100000,{r}));
        check(clocks.snapshot().excluded==1,"A declared new radio session permits clock reset");
        base.capture_seq=4;base.requested_center_hz=867100000;base.timing->device_time_ns+=300000000;
        clocks.ingest(make_batch(base,100000,{r}));
        check(clocks.snapshot().coverage.size()==2,"Exposure denominators remain separate by receiver profile");
    }
    // Real previously recorded SX1262 IQ, not an internal waveform generator.
    for(const auto& entry:std::filesystem::directory_iterator(std::string(PROJECT_ROOT_DIR)+"/tests/fixtures/lora_sx1262"))if(entry.is_directory()) {
        auto hw=load_lora_capture(entry.path().string());hw.run_id="hardware-fixture:"+entry.path().filename().string();hw.capture_seq=1;
        auto rows=analyze_lora_capture(hw.iq,hw.sample_rate_hz,hw.requested_center_hz);
        auto fixture=make_batch(hw,hw.iq.size(),rows);State s;s.ingest(fixture);
        check(s.snapshot().eligible>=1&&s.snapshot().device_timed==0,"Real SX1262 IQ produces integrity-eligible events without invented legacy hardware timing");
    }
    check(snapshot_json(expected.snapshot()).at("attack_rules_enabled")==0,"Foundation never implies a LoRa attack verdict");
    std::filesystem::remove_all(tmp);return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" ("<<tmp<<")\n";return 1;}}
