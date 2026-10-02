#include "security/wifi_security_state.hpp"
#include "wifi_frame.hpp"
#include "security/wifi_security_monitor.hpp"
#include <iostream>
using namespace rfmon;
using namespace rfmon::wifi_security;
int failures = 0;
void check(bool ok, const char* message) { std::cout << (ok ? "PASS " : "FAIL ") << message << '\n'; failures += !ok; }
CaptureRecord cap(int n) {
    CaptureRecord c; c.run_id="beacon-test"; c.radio_session=1; c.capture_seq=n;
    c.band="wifi_2g4"; c.channel=6; c.gain_db=20; c.device="fixture"; c.antenna="RX2";
    c.sample_rate_hz=20e6; c.samples_received=c.analysed_samples=10000000;
    c.processed=true; c.clock=ClockDomain::UsrpDevice; c.device_time_ns=int64_t(n)*600000000;
    c.host_before_ns=1700000000000000000LL+c.device_time_ns; return c;
}
std::vector<uint8_t> beacon(uint64_t tsf, int seq=10, bool retry=false) {
    // Independent MAC/fixed-field fixture, with an SSID IE and trailing CRC.
    std::vector<uint8_t> b(43,0); b[0]=0x80; b[1]=retry?8:0;
    for(int n=4;n<10;++n)b[n]=255;
    for(int n=0;n<6;++n)b[10+n]=b[16+n]=uint8_t(0x20+n);
    b[22]=uint8_t(seq<<4);b[23]=uint8_t(seq>>4);
    for(int n=0;n<8;++n)b[24+n]=uint8_t(tsf>>(8*n));
    b[32]=100;b[34]=1; b[36]=0;b[37]=1;b[38]='X';
    auto crc=wifi::fcs32(b.data(),b.size()-4);
    for(int n=0;n<4;++n)b[b.size()-4+n]=uint8_t(crc>>(8*n));
    return b;
}
FrameEvent event(const CaptureRecord& c,uint64_t tsf,int seq=10,bool retry=false) {
    return make_frame_event(c,1000,1000,"OFDM",6,false,beacon(tsf,seq,retry),true);
}
void feed(SecurityState& s,CaptureRecord c,uint64_t tsf,int seq=10,bool retry=false) {
    s.ingest(event(c,tsf,seq,retry));c.events_submitted=1;s.ingest(c);
}
void start(SecurityState& s) { feed(s,cap(1),1000000);feed(s,cap(2),1600000,11); }
int main() {
    {
        SecurityState s;start(s);feed(s,cap(3),1000000);
        check(s.snapshot().incidents_total==1,"historical beacon after newer beacon alerts");
        const auto& i=s.snapshot().incidents.at(0);
        check(i.rule=="historical_beacon_replay"&&i.confidence==IncidentConfidence::Medium,"suspected replay classification");
        check(i.first_evidence.size()==3&&i.context.at("newer_tsf_us")=="1600000","original, intervening and repeat evidence retained");
        check(i.measures.at("replay_age_s")>1&&i.quiet_analysed_s==0,"actual receive age; triggering capture never quiet");
        feed(s,cap(4),1000000,10,true);
        feed(s,cap(5),1000000);
        check(s.snapshot().incidents_total==1,"repeated observations coalesce");
        SecurityState restored;restored.import_persistent(s.export_persistent());
        check(restored.snapshot().incidents.at(0).restored&&restored.snapshot().incidents.at(0).first_evidence.size()==s.snapshot().incidents.at(0).first_evidence.size(),"evidence persists with interrupted status");
        check(snapshot_json(s.snapshot()).at("beacon_replay").at("evaluated")==5,"snapshot exposes rule diagnostics");
    }
    {
        SecurityState s;for(int n=1;n<20;++n)feed(s,cap(n),uint64_t(n)*600000,4090+n);
        check(s.snapshot().incidents_total==0,"normal progression and sequence wrap benign");
    }
    {
        SecurityState s;feed(s,cap(1),1000000);feed(s,cap(2),1000000,10,true);
        check(s.snapshot().incidents_total==0,"repetition without intervening newer beacon is not replay verdict");
        feed(s,cap(3),1600000,11);feed(s,cap(4),1000000,10,true);
        check(s.snapshot().incidents_total==1,"Retry flag cannot conceal historical beacon");
    }
    {
        SecurityState s;start(s);auto c=cap(3);auto e=event(c,1000000);s.ingest(e);s.ingest(e);c.events_submitted=1;s.ingest(c);s.ingest(c);
        check(s.snapshot().incidents.at(0).observations==1&&s.snapshot().frames_duplicate==1,"duplicate ingestion is idempotent");
    }
    {
        SecurityState s;start(s);feed(s,cap(3),1,0);feed(s,cap(4),600001,1);feed(s,cap(5),1000000);
        check(s.snapshot().incidents_total==0,"unknown TSF regression reacquires after reboot-like reset");
    }
    for(int variant=0;variant<7;++variant) {
        SecurityState s;start(s);auto c=cap(3);
        if(variant==0)c.clock=ClockDomain::HostOnly;
        if(variant==1)c.events_rejected_by_queue=1;
        if(variant==2)c.overflows.push_back({0,std::nullopt});
        if(variant==3)c.burst_cap_reached=true;
        if(variant==4)c.timed_out=true;
        if(variant==5)c.processed=false;
        if(variant==6)c.device_time_ns=0;
        feed(s,c,1000000);
        check(s.snapshot().incidents_total==0&&s.snapshot().beacon_replay_excluded>0,"degraded or out-of-order input cannot establish replay");
    }
    for(int variant=0;variant<4;++variant) {
        SecurityState s;start(s);auto c=cap(3);
        if(variant==0)c.radio_session=2;
        if(variant==1)c.gain_db=30;
        if(variant==2)c.channel=11;
        if(variant==3)c.device_time_ns+=10000000000LL;
        feed(s,c,1000000);
        check(s.snapshot().incidents_total==0,"session/profile/channel/gap prevents historical comparison");
    }
    {
        SecurityState s;start(s);s.ingest(LossNotice{1,0,0,0});feed(s,cap(3),1000000);
        check(s.snapshot().incidents_total==0,"global loss clears replay history");
    }
    {
        SecurityState s;start(s);auto c=cap(3);s.ingest(event(c,1000000));c.events_submitted=2;s.ingest(c);
        check(s.snapshot().incidents_total==0&&s.snapshot().beacon_replay_excluded==1,"missing event invalidates capture before emission");
    }
    {
        StateLimits l;l.beacon_replay.history_per_identity=1;SecurityState s(l);start(s);feed(s,cap(3),1000000);
        check(s.snapshot().incidents_total==0&&s.snapshot().beacon_replay_forgotten>0,"bounded history eviction prevents unsupported match");
    }
    {
        StateLimits l;l.beacon_replay.retention_s=0.8;SecurityState s(l);start(s);feed(s,cap(3),1000000);
        check(s.snapshot().incidents_total==0,"history TTL expires original observation");
    }
    {
        StateLimits l;l.beacon_replay.enabled=false;SecurityState s(l);start(s);feed(s,cap(3),1000000);
        check(s.snapshot().incidents_total==0,"rule disabled");
    }
    {
        StateLimits l;l.beacon_replay.max_frames_per_capture=1;SecurityState s(l);start(s);auto c=cap(3);
        s.ingest(event(c,1000000));auto e=event(c,1000000);e.sample_start+=1000;e.device_time_ns=*e.device_time_ns+50000;s.ingest(e);
        c.events_submitted=2;s.ingest(c);check(s.snapshot().incidents_total==0,"local capacity overload rejects whole capture");
    }
    {
        SecurityState s;auto c=cap(1);
        for(int n=0;n<3;++n){auto e=event(c,n==1?1600000:1000000,n==1?11:10);e.sample_start+=n*10000;e.device_time_ns=*e.device_time_ns+n*500000;s.ingest(e);}
        c.events_submitted=3;s.ingest(c);check(s.snapshot().incidents_total==0,"near-time duplicate below minimum delay");
    }
    {
        SecurityState s;start(s);auto c=cap(3);auto e=event(c,1000000);e.mpdu[38]^=1;s.ingest(e);c.events_submitted=1;s.ingest(c);
        check(s.snapshot().incidents_total==0&&s.snapshot().frames_rejected_fcs==1,"invalid FCS never becomes replay evidence");
    }
    {
        StateLimits l;l.incidents.close_after_quiet_analysed_s=1;SecurityState s(l);start(s);feed(s,cap(3),1000000);
        auto bad=cap(4);bad.timed_out=true;feed(s,bad,2200000);
        check(s.snapshot().incidents_open==1,"degraded capture cannot close replay incident");
        feed(s,cap(5),2800000);feed(s,cap(6),3400000);
        check(s.snapshot().incidents_open==0,"clean observed recovery closes incident");
    }
    {
        SecurityState direct, offline; MonitorConfig cfg; WifiSecurityMonitor live(cfg); live.start();
        for(int n=1;n<=3;++n) {
            auto c=cap(n);auto e=event(c,n==2?1600000:1000000,n==2?11:10);c.events_submitted=1;
            direct.ingest(e);direct.ingest(c);
            auto decoded=frame_from_json(nlohmann::json::parse(to_json(e).dump()));
            auto coverage=capture_from_json(nlohmann::json::parse(to_json(c).dump()));
            offline.ingest(decoded);offline.ingest(coverage);live.submit(decoded);live.submit(coverage);
        }
        live.stop();
        check(snapshot_json(direct.snapshot())==snapshot_json(offline.snapshot()),"serialized replay equals direct state including evidence");
        check(snapshot_json(direct.snapshot())==snapshot_json(*live.snapshot()),"threaded monitor equals offline replay");
    }
    {
        SecurityState s;start(s);s.note_input_rejected();feed(s,cap(3),1000000);
        check(s.snapshot().incidents_total==0&&s.snapshot().input_lines_rejected==1,
              "malformed recording line breaks historical replay comparison");
    }
    for(int variant=0;variant<4;++variant) {
        SecurityState s;start(s);auto c=cap(3);auto e=event(c,1000000);
        if(variant==0)e.mpdu[38]='Y'; // different SSID, same TSF/sequence
        if(variant==1)e.mpdu[22]^=0x10; // different sequence, same TSF
        if(variant==2)e.phy="DSSS",e.rate_mbps=1;
        if(variant==3)e.mpdu[10]^=2; // different claimed transmitter
        auto crc=wifi::fcs32(e.mpdu.data(),e.mpdu.size()-4);
        for(int n=0;n<4;++n)e.mpdu[e.mpdu.size()-4+n]=uint8_t(crc>>(8*n));
        s.ingest(e);c.events_submitted=1;s.ingest(c);
        check(s.snapshot().incidents_total==0,"TSF/sequence alone or a different PHY/source cannot establish full-frame replay");
    }
    {
        SecurityState s;start(s);auto c=cap(3);auto earlier=event(c,1000000);auto later=event(c,2200000,12);
        later.sample_start+=1000;later.device_time_ns=*later.device_time_ns+50000;
        s.ingest(later);s.ingest(earlier);c.events_submitted=2;s.ingest(c);
        check(s.snapshot().incidents_total==0&&s.snapshot().beacon_replay_excluded==1,
              "out-of-order frames invalidate the entire capture before an incident can be emitted");
    }
    return failures?1:0;
}
