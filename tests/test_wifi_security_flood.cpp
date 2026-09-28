// Detector behavior and failure modes, shared offline/live state; no radio.
#include "security/wifi_security_state.hpp"
#include "security/wifi_security_monitor.hpp"
#include "wifi_frame.hpp"
#include <fstream>
#include <iostream>
using namespace rfmon;
using namespace rfmon::wifi_security;
int failures=0;
std::vector<nlohmann::json>* recording=nullptr;
void check(bool ok,const char* label) { std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n'; if(!ok)++failures; }
StateLimits limits() {
    StateLimits l; l.baseline.window_analysed_s=1;
    l.flood.min_baseline_s=3; l.flood.min_baseline_windows=3;
    l.flood.window_s=1; l.flood.floor_rate=4; l.flood.min_units=4;
    l.flood.release_quiet_s=1; l.incidents.close_after_quiet_analysed_s=2;
    return l;
}
CaptureRecord capture(uint64_t seq,int channel=6) {
    CaptureRecord c; c.run_id="flood-test"; c.capture_seq=seq; c.radio_session=1;
    c.band="wifi_2g4"; c.channel=channel; c.sample_rate_hz=20e6;
    c.samples_received=c.analysed_samples=10000000; c.processed=true;
    c.clock=ClockDomain::UsrpDevice; c.device_time_ns=int64_t(seq)*600000000;
    c.host_before_ns=1700000000000000000LL+c.device_time_ns; c.gain_db=20;
    c.antenna="RX2"; c.device="test"; return c;
}
std::vector<uint8_t> frame(int seq,bool retry=false,bool disassoc=false) {
    // Independent known management header fixture; vary sequence then refresh FCS.
    std::vector<uint8_t> b;
    from_hex("c0003a01ffffffffffff00112233445500112233445530120700713d26fa",b);
    b[0]=disassoc?0xa0:0xc0; b[1]=retry?8:0; b[22]=(seq<<4)&255; b[23]=(seq>>4)&255;
    auto fcs=wifi::fcs32(b.data(),b.size()-4);
    for(int i=0;i<4;++i)b[b.size()-4+i]=uint8_t(fcs>>(8*i)); return b;
}
void feed(SecurityState& s, CaptureRecord c,int count=0,bool retries=false) {
    for(int n=0;n<count;++n) {
        auto e=make_frame_event(c,1000+n*1000,400,"OFDM",6,false,
            frame(retries?7:int(c.capture_seq)*20+n,retries,n%2&&!retries),true);
        if(recording)recording->push_back(to_json(e));
        s.ingest(e);
    }
    c.events_submitted=count; if(recording)recording->push_back(to_json(c)); s.ingest(c);
}
void warm(SecurityState& s) { for(int i=1;i<=6;++i)feed(s,capture(i)); }
int main() {
    {
        SecurityState s(limits()); warm(s);
        feed(s,capture(7),4);feed(s,capture(8),4);
        check(s.snapshot().incidents_open==1,"sustained mixed deauth/disassoc creates incident");
        auto i=s.snapshot().incidents.at(0);
        check(i.numerator==8&&i.denominator_analysed_s==1&&i.confidence==IncidentConfidence::Medium,"exact units, usable exposure, conservative confidence");
        check(!i.first_evidence.empty()&&!i.timeline.empty()&&i.measures.at("threshold")==4,"evidence and threshold timeline");
        check(s.snapshot().baselines.at(0).learned_analysed_s==3&&!s.snapshot().baselines.at(0).holders.empty(),"triggering window excluded before learning");
        feed(s,capture(9),4);feed(s,capture(10),4);
        check(s.snapshot().incidents_total==1&&s.snapshot().incidents.at(0).observations==2,"coalesces sustained flood");
        auto bad=capture(11);bad.overflows.push_back({10,std::nullopt});feed(s,bad);
        bad=capture(12);bad.events_rejected_by_queue=20;feed(s,bad);
        check(s.snapshot().incidents_open==1,"overflow and queue loss cannot establish quiet");
        for(int n=13;n<=16;++n)feed(s,capture(n));
        check(s.snapshot().incidents_open==0&&s.snapshot().baselines.at(0).holders.empty(),"clean recovery closes and releases hold");
        check(snapshot_json(s.snapshot()).at("incidents").size()==1,"offline JSON includes incidents");
    }
    {
        SecurityState s(limits());
        for(int n=1;n<=8;++n)feed(s,capture(n),4);
        check(s.snapshot().incidents_total==0,"cold start cannot claim a baseline-supported flood");
        check(s.snapshot().baselines.at(0).learned_analysed_s==0,"cold-start candidate cannot poison baseline");
    }
    {
        SecurityState s(limits());warm(s);feed(s,capture(7),20,true);feed(s,capture(8),20,true);
        check(s.snapshot().incidents_total==0,"legitimate retry duplicates do not inflate units");
        feed(s,capture(9),10);feed(s,capture(10));
        check(s.snapshot().incidents_total==0,"single-capture reconnect burst lacks sustained evidence");
    }
    {
        SecurityState s(limits());warm(s);feed(s,capture(7),4);
        auto c=capture(100);feed(s,c,4);feed(s,capture(101));
        check(s.snapshot().incidents_total==0,"scan gap breaks sustained window");
        c=capture(102);c.gain_db=35;feed(s,c,10);c=capture(103);c.gain_db=35;feed(s,c,10);
        check(s.snapshot().incidents_total==0,"new gain requires own baseline");
    }
    {
        SecurityState s(limits());warm(s);feed(s,capture(7),4);
        s.ingest(LossNotice{1,0,0,0});feed(s,capture(8),4);feed(s,capture(9));
        check(s.snapshot().incidents_total==0,"loss notice invalidates partial detector window");
        auto c=capture(10);c.events_rejected_by_queue=50;feed(s,c);
        auto before=s.snapshot().baselines.at(0).learned_analysed_s;
        c=capture(11);c.burst_cap_reached=true;feed(s,c);
        check(s.snapshot().baselines.at(0).learned_analysed_s==before,"queue loss and burst cap excluded from clean reference");
    }
    {
        auto l=limits();l.flood.max_frames_per_capture=2;SecurityState s(l);warm(s);
        feed(s,capture(7),20);feed(s,capture(8),20);
        check(s.snapshot().incidents_total==0&&s.snapshot().flood_excluded_captures==2,"bounded detector overload is explicit and unusable");
    }
    {
        BaselineLimits l;l.window_analysed_s=1;BaselineStore b(l);auto c=capture(1);auto key=BaselineStore::key_for(c);
        b.note_capture(c);b.set_hold(key,"a",true);b.set_hold(key,"b",true);b.set_frozen(key,true);
        b.set_hold(key,"a",false);b.set_frozen(key,false);b.note_capture(capture(2));
        check(b.summary(key)->windows_included==0&&b.holders(key)->count("b"),"independent rule holds latch partial window");
        b.reset(key);check(b.holders(key)->count("b"),"reset preserves active rule hold");
        BaselineStore restored(l);restored.import_json(b.export_json());
        check(restored.holders(key)&&restored.holders(key)->count("b"),"holds survive persistence");
        restored.set_hold(key,"b",false);restored.note_capture(capture(3));restored.note_capture(capture(4));
        check(restored.summary(key)->windows_included==1,"last holder release permits clean learning");
    }
    {
        IncidentStore store;IncidentObservation o;o.rule="test";o.evidence=IncidentEvidence{};
        o.evidence->mpdu.resize(10000);o.evidence->note.assign(5000,'x');
        auto id=store.observe(o);for(int n=0;n<20;++n){store.observe(o);store.append_point(id,{});}
        IncidentLimits l;l.max_first_evidence=1;l.max_evidence=1;l.max_timeline=2;l.max_evidence_bytes=64;l.max_text_bytes=100;
        IncidentStore restored(l);restored.import_json(store.export_json());auto i=restored.incidents().front();
        check(i.first_evidence.size()==1&&i.evidence.size()==1&&i.timeline.size()==2,"restored evidence and timeline honor current capacities");
        check(i.first_evidence[0].mpdu.size()==64&&i.first_evidence[0].note.size()==100,"restored byte limits enforced");
        restored.note_analysed("",0,-10);check(restored.incidents().front().quiet_analysed_s==0,"invalid quiet duration ignored");
    }
    {
        SecurityState first(limits());warm(first);feed(first,capture(7),4);feed(first,capture(8),4);
        SecurityState restored(limits());restored.import_persistent(first.export_persistent());
        auto c=capture(9);c.radio_session=2;feed(restored,c);c=capture(10);c.radio_session=2;feed(restored,c);
        check(restored.snapshot().incidents_open==1,"restart does not inherit quiet or partial detection continuity");
        for(int n=11;n<=14;++n){c=capture(n);c.radio_session=2;feed(restored,c);}
        check(restored.snapshot().incidents_open==0&&restored.snapshot().baselines.at(0).holders.empty(),"restored hold recovers on usable quiet");
    }
    {
        std::vector<nlohmann::json> log; recording=&log;
        SecurityState direct(limits());warm(direct);feed(direct,capture(7),4);feed(direct,capture(8),4);
        recording=nullptr;
        SecurityState replay(limits());MonitorConfig cfg;cfg.limits=limits();WifiSecurityMonitor live(cfg);live.start();
        for(const auto& row:log) {
            auto j=nlohmann::json::parse(row.dump());
            if(j.at("kind")=="frame") { auto e=frame_from_json(j);replay.ingest(e);live.submit(e); }
            else { auto c=capture_from_json(j);replay.ingest(c);live.submit(c); }
        }
        live.stop();
        check(snapshot_json(direct.snapshot())==snapshot_json(replay.snapshot()),"offline serialization preserves decisions and evidence");
        check(snapshot_json(direct.snapshot())==snapshot_json(*live.snapshot()),"threaded live consumer and offline state agree");
    }
    {
        SecurityState s(limits());warm(s);feed(s,capture(7),4);feed(s,capture(8),4);
        auto c=capture(9);c.device_time_ns=1;feed(s,c);
        check(s.snapshot().incidents.at(0).quiet_analysed_s==0,"out-of-order capture cannot advance quiet");
        c=capture(10);c.events_submitted=10;s.ingest(c);
        check(s.snapshot().incidents.at(0).quiet_analysed_s==0,"missing frame events cannot advance quiet");
    }
    {
        BaselineLimits l;l.max_baselines=1;BaselineStore b(l);
        check(b.set_hold("one","rule",true)&&!b.set_hold("two","rule",true),"rule hold keys are bounded");
        b.set_hold("one","rule",false);
        check(b.set_hold("two","rule",true),"released hold capacity is reusable");
    }
    {
        SecurityState s(limits());warm(s);
        for(int seq=7;seq<=8;++seq) {
            auto c=capture(seq);
            for(int n=0;n<4;++n) {
                std::vector<uint8_t> b;
                from_hex("c0403a0166778899aabb001122334455001122334455f000ab00002001000000dead999999999999999947f6a45e",b);
                b[22]=uint8_t((seq*4+n)<<4);b[23]=uint8_t((seq*4+n)>>4);
                auto crc=wifi::fcs32(b.data(),b.size()-4);
                for(int k=0;k<4;++k)b[b.size()-4+k]=uint8_t(crc>>(8*k));
                s.ingest(make_frame_event(c,1000+n*1000,400,"OFDM",6,false,b,true));
            }
            c.events_submitted=4;s.ingest(c);
        }
        check(s.snapshot().incidents_open==1&&s.snapshot().incidents.at(0).confidence==IncidentConfidence::Medium,
              "encrypted management headers count without claiming PMF authenticity");
    }
    {
        BaselineLimits l;l.window_analysed_s=.5;BaselineStore b(l);
        for(int seq=1;seq<=2;++seq) {
            auto c=capture(seq);
            for(int n=0;n<10;++n) {
                auto bytes=frame(n,false,seq==1?n==0:n!=0);
                auto e=make_frame_event(c,n*1000,400,"OFDM",6,false,bytes,true);
                b.note_frame(e,parse_mac_frame(bytes.data(),bytes.size(),true));
            }
            b.note_capture(c);
        }
        auto r=b.combined_rate(BaselineStore::key_for(capture(1)),{BaselineMetric::Deauth,BaselineMetric::Disassoc});
        check(r.p95==20,"combined baseline sums window counts before computing percentile");
    }
    {
        auto l=limits();SecurityState s(l);warm(s);
        for(int seq=7;seq<=8;++seq) {
            auto c=capture(seq);
            for(int n=0;n<4;++n) {
                auto b=frame(seq*4+n);b[4]=uint8_t(n*2); // four distinct claimed targets
                auto crc=wifi::fcs32(b.data(),b.size()-4);
                for(int k=0;k<4;++k)b[b.size()-4+k]=uint8_t(crc>>(8*k));
                s.ingest(make_frame_event(c,n*1000,400,"OFDM",6,false,b,true));
            }
            c.events_submitted=4;s.ingest(c);
        }
        check(s.snapshot().incidents_total==0,"different targets are not combined into a target flood");
    }
    {
        SecurityState s;
        for(int n=1;n<=60;++n)feed(s,capture(n));
        for(int n=61;n<=64;++n)feed(s,capture(n),10);
        check(s.snapshot().incidents_open==1&&s.snapshot().incidents.at(0).numerator==40&&
              s.snapshot().incidents.at(0).denominator_analysed_s==2,
              "production defaults detect sustained activity after 30 usable baseline seconds");
    }
    std::cout<<failures<<" failures\n";return failures?1:0;
}
