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
// Separate transmissions, with optional repeated bytes or distributed clients.
void transmissions(SecurityState& s,CaptureRecord c,int count,bool repeat=false,
                   bool retry=false,int targets=1,size_t spacing=1000000,bool reverse=false,
                   bool client_origin=false,bool split_bssid=false) {
    for(int k=0;k<count;++k) {
        int n=reverse?count-1-k:k;
        auto bytes=frame(repeat?7:int(c.capture_seq)*20+n,retry);
        if(targets>1) bytes[4]=uint8_t((n%targets+1)*2);
        if(client_origin) {
            for(int j=0;j<6;++j)bytes[4+j]=bytes[16+j];
            bytes[10]=uint8_t((n%targets+1)*2);
        }
        if(split_bssid) { bytes[15]=uint8_t(n%targets);bytes[21]=uint8_t(n%targets); }
        auto crc=wifi::fcs32(bytes.data(),bytes.size()-4);
        for(int j=0;j<4;++j)bytes[bytes.size()-4+j]=uint8_t(crc>>(8*j));
        auto e=make_frame_event(c,1000+size_t(n)*spacing,400,"OFDM",6,false,bytes,true);
        if(recording)recording->push_back(to_json(e));
        s.ingest(e);
    }
    c.events_submitted=count;if(recording)recording->push_back(to_json(c));s.ingest(c);
}
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
        check(s.snapshot().incidents_total==1&&s.snapshot().incidents.at(0).observations==3,"rolling observations coalesce sustained flood");
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
        auto snap=s.snapshot();
        check(snap.incidents_total==1&&snap.incidents[0].target.empty()&&
              snap.incidents[0].path=="ap_wide_distinct_management_frames"&&snap.incidents[0].measures.at("targets")==4,
              "distributed targets create one AP-wide incident with explicit scope");
    }
    {
        SecurityState s;
        for(int n=1;n<=60;++n)feed(s,capture(n));
        for(int n=61;n<=64;++n)feed(s,capture(n),10);
        check(s.snapshot().incidents_open==1&&s.snapshot().incidents.at(0).numerator==40&&
              s.snapshot().incidents.at(0).denominator_analysed_s==2,
              "production defaults detect sustained activity after 30 usable baseline seconds");
    }
    {
        SecurityState s(limits());warm(s);feed(s,capture(7),4);
        s.note_input_rejected();feed(s,capture(8),4);feed(s,capture(9));
        check(s.snapshot().incidents_total==0&&s.snapshot().input_lines_rejected==1,
              "malformed recording input breaks sustained flood evidence");
    }
    {
        SecurityState s(limits());warm(s);
        transmissions(s,capture(7),4,true);transmissions(s,capture(8),4,true);
        auto snap=s.snapshot();
        check(snap.incidents_total==1&&snap.incidents[0].path=="repeated_management_frames"&&
              snap.incidents[0].numerator==8&&snap.incidents[0].measures.at("distinct_units")==1,
              "separated identical non-retry copies trigger repeated-content path");
        check(snap.baselines[0].learned_analysed_s==3&&snap.incidents[0].rule_version=="2",
              "repeated-content candidates cannot train the baseline and carry rule version");
    }
    {
        SecurityState s(limits());warm(s);
        transmissions(s,capture(7),30,true,false,1,1000);
        transmissions(s,capture(8),30,true,false,1,1000);
        check(s.snapshot().incidents_total==0,"short identical copies without Retry still collapse under guard");
        transmissions(s,capture(9),4,true,true);transmissions(s,capture(10),4,true,true);
        check(s.snapshot().incidents_total==0,"long Retry-marked repeats do not claim independent attempts");
    }
    {
        SecurityState s(limits());warm(s);
        feed(s,capture(7));feed(s,capture(8),4);feed(s,capture(9),4);
        check(s.snapshot().incidents_total==1&&s.snapshot().incidents[0].timeline[0].capture_seq_first==8,
              "flood across former fixed-window boundary is detected");
        feed(s,capture(10));
        check(s.snapshot().incidents[0].observations==1&&s.snapshot().incidents[0].quiet_analysed_s==.5,
              "rolling historic activity alone cannot reset quiet or emit a new observation");
    }
    {
        // Shift the same event campaign through each capture phase of a 2 s window.
        for(int phase=0;phase<4;++phase) {
            auto l=limits();l.flood.window_s=2;SecurityState s(l);warm(s);
            int seq=7;for(int n=0;n<phase;++n)feed(s,capture(seq++));
            for(int n=0;n<4;++n)feed(s,capture(seq++),4);
            check(s.snapshot().incidents_total==1,"rolling detection survives capture-window phase shift");
        }
    }
    {
        auto l=limits();l.flood.min_units=5;SecurityState s(l);warm(s);
        transmissions(s,capture(7),4,true,false,4);transmissions(s,capture(8),4,true,false,4);
        auto snap=s.snapshot();
        check(snap.incidents_total==1&&snap.incidents[0].target.empty()&&
              snap.incidents[0].path=="ap_wide_repeated_management_frames"&&snap.incidents[0].numerator==8,
              "repeated per-client contents aggregate into AP-wide path");
        SecurityState separate(limits());warm(separate);
        transmissions(separate,capture(7),4,false,false,4,1000000,false,false,true);
        transmissions(separate,capture(8),4,false,false,4,1000000,false,false,true);
        check(separate.snapshot().incidents_total==0,"different claimed BSSIDs never combine into AP-wide flood");
    }
    {
        SecurityState s(limits());warm(s);
        transmissions(s,capture(7),4,false,false,4,1000000,false,true);
        transmissions(s,capture(8),4,false,false,4,1000000,false,true);
        check(s.snapshot().incidents_total==1&&s.snapshot().incidents[0].target.empty()&&
              s.snapshot().incidents[0].measures.at("targets")==4,
              "client-originated disconnects aggregate affected clients at claimed AP");
        SecurityState burst(limits());warm(burst);
        transmissions(burst,capture(7),8,false,false,4);feed(burst,capture(8));
        check(burst.snapshot().incidents_total==0,"single-capture multi-client reconnect burst stays negative");
    }
    {
        SecurityState s(limits());warm(s);
        for(int seq=7;seq<=8;++seq) {
            auto c=capture(seq);c.clock=ClockDomain::HostOnly;
            transmissions(s,c,4,true);
        }
        check(s.snapshot().incidents_total==0,"host-only timing cannot establish separated repeated-content units");
        SecurityState ordered(limits());warm(ordered);
        transmissions(ordered,capture(7),4,true,false,1,1000000,true);
        transmissions(ordered,capture(8),4,true,false,1,1000000,true);
        check(ordered.snapshot().incidents_total==1&&ordered.snapshot().incidents[0].numerator==8,
              "sample ordering makes reversed event delivery deterministic");
    }
    {
        for(int kind=0;kind<5;++kind) {
            SecurityState s(limits());warm(s);transmissions(s,capture(7),4,true);
            auto c=capture(8);
            if(kind==0)s.ingest(LossNotice{1,0,0,0});
            if(kind==1)s.note_input_rejected();
            if(kind==2)c.burst_cap_reached=true;
            if(kind==3)c.overflows.push_back({10,std::nullopt});
            if(kind==4)c.radio_session=2;
            transmissions(s,c,4,true);feed(s,capture(9));
            check(s.snapshot().incidents_total==0,"repeated-content evidence cannot bridge loss, bad input, caps, overflow or session reset");
        }
    }
    {
        auto l=limits();l.flood.max_units=3;SecurityState s(l);warm(s);
        transmissions(s,capture(7),4,true);
        check(s.snapshot().flood_excluded_captures==1&&s.snapshot().incidents_total==0,
              "repeated raw observations obey bounded window capacity");
        l=limits();l.flood.max_window_captures=1;SecurityState captures(l);warm(captures);
        check(captures.snapshot().flood_excluded_captures>0,"tiny-capture window capacity excludes incomplete comparisons");
        l=limits();l.flood.max_groups=1;SecurityState groups(l);warm(groups);
        transmissions(groups,capture(7),4);
        check(groups.snapshot().flood_excluded_captures==1,"AP and target group capacity fails conservatively");
    }
    {
        auto l=limits();l.flood.min_units=5;
        std::vector<nlohmann::json> log;recording=&log;
        SecurityState direct(l);warm(direct);
        transmissions(direct,capture(7),4,true);transmissions(direct,capture(8),4,true);
        for(int seq=9;seq<=14;++seq)feed(direct,capture(seq));
        transmissions(direct,capture(15),4,true,false,4);transmissions(direct,capture(16),4,true,false,4);
        recording=nullptr;
        SecurityState replay(l);MonitorConfig cfg;cfg.limits=l;
        WifiSecurityMonitor live(cfg);live.start();
        bool queued=true;
        for(const auto& j:log) {
            if(j.at("kind")=="frame") {auto e=frame_from_json(j);replay.ingest(e);queued=live.submit(e)&&queued;}
            else {auto c=capture_from_json(j);replay.ingest(c);queued=live.submit(c)&&queued;}
        }
        check(queued,"expanded campaign queues without loss");
        live.stop();
        check(direct.snapshot().incidents_total==2&&direct.snapshot().incidents[0].path=="repeated_management_frames"&&
              direct.snapshot().incidents[1].path=="ap_wide_repeated_management_frames",
              "expanded campaign contains target and AP-wide repeated-content positives");
        check(snapshot_json(direct.snapshot())==snapshot_json(replay.snapshot()),"expanded paths preserve offline serialization decisions and evidence");
        check(snapshot_json(direct.snapshot())==snapshot_json(*live.snapshot()),"expanded paths produce identical threaded and offline incidents");
    }
    {
        SecurityState s(limits());
        for(int seq=1;seq<=6;++seq)feed(s,capture(seq),1);
        feed(s,capture(7),3);feed(s,capture(8),3);
        check(s.snapshot().incidents_total==0,"learned channel management rate raises threshold above provisional floor");
        check(!s.snapshot().baselines[0].holders.empty(),"sub-threshold candidates still protect reference learning");
    }
    {
        IncidentStore store;IncidentObservation o;o.rule="management_disconnect_flood";
        o.band="wifi_2g4";o.channel=6;o.claimed_bssid="00:11:22:33:44:55";o.target="";
        o.baseline_key="gain20";auto first=store.observe(o);
        o.baseline_key="gain30";auto second=store.observe(o);
        check(first!=second&&store.open_count()==2,"receiver profiles never coalesce AP-wide incidents");
        store.note_analysed(o.band,o.channel,1,nullptr,"gain20");
        check(store.incidents()[0].quiet_analysed_s==1&&store.incidents()[1].quiet_analysed_s==0,
              "quiet exposure stays within matching receiver profile");
    }
    {
        SecurityState s(limits());warm(s);
        for(int seq=7;seq<=8;++seq) {
            auto c=capture(seq);
            for(int n=0;n<4;++n) {
                auto e=make_frame_event(c,1000+size_t(n)*1000000,400,"OFDM",6,false,frame(7),true);
                e.device_time_ns=*e.device_time_ns+100000000;
                s.ingest(e);
            }
            c.events_submitted=4;s.ingest(c);
        }
        check(s.snapshot().incidents_total==0,"disagreeing event/device anchor cannot prove repeat separation");
        auto c=capture(9);auto e=make_frame_event(c,c.samples_received+1,400,"OFDM",6,false,frame(7),true);
        s.ingest(e);c.events_submitted=1;s.ingest(c);
        check(s.snapshot().flood_excluded_captures==1,"out-of-capture evidence is explicitly excluded");
    }
    {
        auto l=limits();l.flood.repeat_guard_s=0;bool rejected=false;
        try {SecurityState invalid(l);}catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"invalid repeated-content guard is rejected");
    }
    {
        SecurityState s(limits());warm(s);
        auto a=capture(7);a.samples_received=a.analysed_samples=30000000;
        auto b=capture(8);b.samples_received=b.analysed_samples=30000000;
        b.device_time_ns=6000000000;b.host_before_ns=1700000006000000000LL;
        transmissions(s,a,8);transmissions(s,b,8);
        check(s.snapshot().incidents_total==1&&s.snapshot().incidents[0].numerator==16&&
              s.snapshot().incidents[0].denominator_analysed_s==3,
              "long captures retain multiple-capture evidence and their full exposure denominator");
    }
    {
        const char* paths[]={"distinct_management_frames","repeated_management_frames",
                             "ap_wide_distinct_management_frames","ap_wide_repeated_management_frames"};
        for(int mode=0;mode<4;++mode) {
            auto l=limits();l.flood.min_units=5;
            std::vector<nlohmann::json> log;recording=&log;
            SecurityState direct(l);warm(direct);
            transmissions(direct,capture(7),4,mode%2,false,mode>=2?4:1);
            transmissions(direct,capture(8),4,mode%2,false,mode>=2?4:1);
            recording=nullptr;MonitorConfig cfg;cfg.limits=l;
            WifiSecurityMonitor live(cfg);live.start();SecurityState replay(l);bool queued=true;
            for(const auto& j:log) {
                if(j.at("kind")=="frame") {auto e=frame_from_json(j);replay.ingest(e);queued=live.submit(e)&&queued;}
                else {auto c=capture_from_json(j);replay.ingest(c);queued=live.submit(c)&&queued;}
            }
            live.stop();
            check(queued&&direct.snapshot().incidents_total==1&&direct.snapshot().incidents[0].path==paths[mode]&&
                  snapshot_json(direct.snapshot())==snapshot_json(replay.snapshot())&&
                  snapshot_json(direct.snapshot())==snapshot_json(*live.snapshot()),
                  "every detection path matches full threaded, direct and serialized offline evidence");
        }
    }
    std::cout<<failures<<" failures\n";return failures?1:0;
}
