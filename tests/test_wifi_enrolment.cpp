// End-to-end enrolment consumer tests. Isolated stores; no SDR or transmissions.
#include "wifi_enrolment.hpp"
#include "wifi_frame.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <unistd.h>
#include <nlohmann/json.hpp>
using namespace rfmon;
using namespace wifi_master;
namespace fs=std::filesystem;
int checks=0,failed=0;
void check(bool b,const char* label){++checks;if(!b)++failed;std::cout<<(b?"PASS ":"FAIL ")<<label<<'\n';}
const std::string address="02:11:22:33:44:55";
std::vector<uint8_t> beacon(const std::string& name="lab",bool client=false){
    std::vector<uint8_t> b(client?24:36);b[0]=client?8:0x80;b[1]=client?1:0;
    const uint8_t mac[]={2,0x11,0x22,0x33,0x44,0x55};
    std::fill_n(b.begin()+4,6,0xff);std::copy(mac,mac+6,b.begin()+10);std::copy(mac,mac+6,b.begin()+16);
    if(!client){b[32]=100;b[34]=1;b.push_back(0);b.push_back(uint8_t(name.size()));b.insert(b.end(),name.begin(),name.end());b.insert(b.end(),{3,1,1});}
    auto crc=wifi::fcs32(b.data(),b.size());for(int k=0;k<4;++k)b.push_back(uint8_t(crc>>(8*k)));return b;
}
EnrolmentBatch batch(uint64_t seq,const std::string& name="lab",double cfo=1,bool client=false){
    EnrolmentBatch b;auto& c=b.capture;c.run_id="fixture";c.radio_session=1;c.capture_seq=seq;c.source="live";c.band="wifi_2g4";c.channel=1;c.channel_hz=2412e6;c.sample_rate_hz=20e6;c.samples_received=c.analysed_samples=20000000;c.processed=true;c.host_before_ns=int64_t(1000+seq*2)*1000000000LL;c.host_after_ns=c.host_before_ns+1000;c.device="Fixture receiver";c.antenna="RX2";c.gain_db=20;
    auto f=wifi_security::make_frame_event(c,100,20000,"OFDM",6,false,beacon(name,client),true);f.bandwidth_hz=20e6;
    wifi_fingerprint::WifiFingerprint fp;fp.cfo_ppm=cfo;fp.irr_db=-30;fp.dc_dbc=-40;fp.dc_ang_deg=179;fp.iq_eps=.01;fp.iq_phi_deg=1;fp.snr_db=30;fp.evm_pct=10;fp.sync_corr=.95;fp.n_samp=128;
    b.observations.push_back({f,fp});return b;
}
WifiMasterRow row(WifiEnrolment& e){e.flush();auto rows=e.snapshot();for(auto& r:rows)if(r.device_key==address)return r;throw std::runtime_error("missing row");}
void send(WifiEnrolment& e,const std::string& action){check(e.command({address,action,"Office","test operator note"}),"command admitted");e.flush();}
int main(){
    {
        WifiEnrolmentInfo info;
        check(!has_repeated_identity_evidence(info),"empty identity is candidate");
        info.policy="approved";
        check(!has_repeated_identity_evidence(info),"operator approval cannot fabricate received evidence");
        RfProfileSummary p;p.captures=100;p.first_ts=100;p.last_ts=101;info.profiles.push_back(p);
        check(!has_repeated_identity_evidence(info),"short observation flood cannot confirm identity");
        info.profiles[0].captures=2;info.profiles[0].last_ts=120;
        info.profiles.push_back(info.profiles[0]);
        check(!has_repeated_identity_evidence(info),"separate contexts cannot combine to meet evidence threshold");
        info.profiles[0].captures=3;
        check(has_repeated_identity_evidence(info),"three captures over ten seconds corroborate interface");
        info.archived_profiles=info.profiles;info.profiles.clear();
        check(has_repeated_identity_evidence(info),"historical corroboration survives context retirement");
    }
    const auto root=fs::temp_directory_path()/("rf-enrolment-"+std::to_string(getpid()));fs::create_directories(root);
    const auto live=(root/"live").string();
    {
        WifiEnrolment e(live);check(e.submit(batch(1)),"capture admitted");auto r=row(e);
        check(r.enrolment&&r.enrolment->policy=="unreviewed"&&!r.enrolment->imported,"new identity unreviewed");
        check(r.identity&&r.identity->ssid=="lab"&&r.reading_count==1,"digital identity and same-burst RF saved");
        check(r.enrolment->history.size()==1&&r.enrolment->history[0].kind=="Discovered","single discovery event");
        e.submit(batch(1));r=row(e);check(r.enrolment->observations==1,"duplicate capture idempotent");
        auto duplicate=batch(2);duplicate.observations.push_back(duplicate.observations.front());e.submit(std::move(duplicate));r=row(e);check(r.enrolment->observations==2,"duplicate burst idempotent");
        send(e,"acknowledge");check(row(e).enrolment->policy=="acknowledged","acknowledge is not approve");
        send(e,"approve");check(row(e).enrolment->policy=="approved","explicit approval saved");
        send(e,"note");check(row(e).enrolment->alias=="Office","operator label saved");
        send(e,"reference");check(row(e).enrolment->reference_version==0,"insufficient captures cannot create reference");
        for(uint64_t seq=3;seq<=10;++seq)e.submit(batch(seq));r=row(e);
        check(r.enrolment->profiles.size()==1&&r.enrolment->profiles[0].captures==10,"one RF sample per independent capture");
        send(e,"reference");r=row(e);check(r.enrolment->reference_version==1&&r.enrolment->profiles[0].has_reference,"explicit reference accepted");
        const auto ref=r.enrolment->profiles[0].reference;
        for(uint64_t seq=11;seq<=13;++seq)e.submit(batch(seq,"lab",10));r=row(e);
        check(r.enrolment->profiles[0].review,"persistent reference divergence latches review");
        check(r.enrolment->profiles[0].reference==ref,"divergence cannot replace reference");
        e.submit(batch(14,"renamed"));r=row(e);check(r.enrolment->digital_review&&r.identity->ssid=="renamed","name change retained and flagged");
        e.submit(batch(15,""));r=row(e);check(r.identity->ssid=="renamed","hidden SSID does not erase name");
        send(e,"review_digital");check(!row(e).enrolment->digital_review,"digital change review separate");
        check(row(e).enrolment->profiles[0].review,"digital acknowledgement does not clear RF review");
        auto rejected=batch(16);rejected.observations[0].fingerprint->gated_out=true;rejected.observations[0].fingerprint->gate_reason="low quality";e.submit(std::move(rejected));r=row(e);
        check(r.enrolment->rejected==1&&r.identity_count==16,"identity retained despite rejected RF");
        auto unknown=batch(17);unknown.observations[0].fingerprint->cfo_ppm=std::numeric_limits<double>::quiet_NaN();e.submit(std::move(unknown));check(row(e).enrolment->rejected==2,"nonfinite RF excluded");
        auto capped=batch(18);capped.capture.burst_cap_reached=true;e.submit(std::move(capped));r=row(e);check(r.enrolment->profiles[0].captures==15,"unusable capture excluded from profile, not inventory");
        auto dsss=batch(19);dsss.observations[0].frame.phy="DSSS";e.submit(std::move(dsss));r=row(e);check(r.enrolment->profiles.size()==2&&r.enrolment->profiles.back().valid==49,"DSSS IQ estimates unavailable, separate context");
        auto other=batch(20);other.capture.radio_session=2;other.observations[0].frame.radio_session=2;e.submit(std::move(other));check(row(e).enrolment->profiles.size()==3,"receiver session split, no automatic baseline merge");
        auto band=batch(21);band.capture.radio_session=2;band.observations[0].frame.radio_session=2;band.capture.channel_hz=5180e6;band.observations[0].frame.channel_hz=5180e6;e.submit(std::move(band));r=row(e);check(r.enrolment->seen_24>0&&r.enrolment->seen_5>0,"per-band observation history");
        auto offline=batch(22);offline.capture.source="offline:fixture";e.submit(std::move(offline));r=row(e);check(r.enrolment->observations==21&&e.health().rejected_sources==1,"offline input cannot mutate live enrolment");
        send(e,"disallow");check(row(e).enrolment->policy=="disallowed","disallowed persists as monitoring policy");
        {WifiEnrolment second(live);check(!second.health().error.empty(),"second writer read-only");second.submit(batch(23));second.flush();check(row(e).enrolment->observations==21,"second writer cannot append");}
    }
    {
        WifiEnrolment e(live);auto r=row(e);check(r.enrolment->policy=="disallowed"&&r.enrolment->alias=="Office","policy and notes survive restart");
        check(r.enrolment->reference_version==1&&r.enrolment->profiles[0].review,"reference and review survive restart");
        check(!r.enrolment->imported,"discovered identity never becomes imported on restart");
        send(e,"approve");check(row(e).enrolment->profiles[0].review,"approval does not clear conflict");
        // Failed atomic replacement must not change approval in memory or on disk.
        const auto tmp=fs::path(live)/(address+".enrol.json.tmp");fs::create_directory(tmp);
        send(e,"disallow");check(row(e).enrolment->policy=="approved"&&!e.health().error.empty(),"failed commit cannot grant/revoke policy");fs::remove(tmp);
    }
    {
        WifiEnrolment e(live);check(row(e).enrolment->policy=="approved","failed command remains uncommitted after restart");
    }
    {
        const auto dir=(root/"legacy").string();
        {WifiMasterList old(dir);auto bytes=beacon();auto b=wifi::parse_beacon(bytes.data(),bytes.size());old.record_identity(*b,2412e6,100);}
        WifiEnrolment e(dir);auto r=row(e);check(r.enrolment->imported&&r.enrolment->policy=="unreviewed","legacy identities require review, never autoapproved");
        check(r.enrolment->history.empty(),"legacy import does not fabricate discovery alerts");e.submit(batch(1));row(e);check(fs::exists(fs::path(dir)/(address+".ndjson.pre-enrolment")),"legacy source backed up before write");
    }
    {
        WifiEnrolment e((root/"clients").string());e.submit(batch(1,"",1,true));auto r=row(e);check(!r.identity&&r.enrolment->role=="Station transmitter (observed)","client transmitter discovered without fabricated SSID");
        check(e.snapshot().size()==1,"destination address is not enrolled");
        auto bad=batch(2);bad.observations[0].frame.mpdu[10]^=1;e.submit(std::move(bad));check(row(e).enrolment->observations==1,"invalid FCS cannot create identity");
        auto mismatch=batch(3);mismatch.observations[0].frame.capture_seq=999;e.submit(std::move(mismatch));check(row(e).enrolment->observations==1,"mismatched capture provenance rejected");
        auto wrongta=batch(4);auto& m=wrongta.observations[0].frame.mpdu;m[10]=4;auto crc=wifi::fcs32(m.data(),m.size()-4);for(int k=0;k<4;++k)m[m.size()-4+k]=uint8_t(crc>>(8*k));e.submit(std::move(wrongta));check(row(e).enrolment->observations==1,"beacon BSSID cannot acquire another transmitter RF");
        for(uint64_t n=5;n<30;++n){auto context=batch(n,"",1,true);context.capture.gain_db=double(n);e.submit(std::move(context));if(n%8==0)e.flush();}
        auto limited=row(e);check(limited.enrolment->profiles.size()==16&&!limited.enrolment->archived_profiles.empty(),"new receiver contexts continue updating with bounded summary archival");
        auto large=batch(30);large.observations.resize(4001);check(!e.submit(std::move(large))&&e.health().dropped_batches==1,"oversized batch rejected with visible loss");
    }
    {
        WifiEnrolment e((root/"clients").string());check(row(e).enrolment->role=="Station transmitter (observed)","client-only record survives restart");
    }
    {
        const auto dir=(root/"compaction").string();
        {
            WifiEnrolment e(dir);auto many=batch(1);
            for(size_t n=1;n<1105;++n){auto x=many.observations.front();x.frame.sample_start=100+n*1000;many.observations.push_back(std::move(x));}
            e.submit(std::move(many));auto r=row(e);
            check(r.enrolment->observations==1105&&r.enrolment->profiles[0].captures==1,"packet flood has one-capture profile weight");
            check(r.reading_count>=1000&&r.reading_count<=1100,"raw RF retention remains bounded");
            send(e,"approve");
            for(uint64_t n=2;n<145;++n){e.submit(batch(n,"name-"+std::to_string(n)));if(n%16==0)e.flush();}
            r=row(e);check(r.enrolment->policy=="approved"&&r.enrolment->digital_review,"changes do not erase approval or review warning");
            check(r.enrolment->history.size()<=128&&r.enrolment->profiles[0].captures<=64,"bounded history and independent-capture window");
            check(!e.command({address,"unsupported",{}, {}}),"unsupported commands rejected before queue");
        }
        WifiEnrolment e(dir);auto r=row(e);check(r.enrolment->policy=="approved"&&r.enrolment->observations>=1105,"compaction/restart preserves policy and counts");
    }
    {
        WifiEnrolment e((root/"archive-reference").string());
        for(uint64_t n=1;n<=9;++n)e.submit(batch(n));row(e);send(e,"approve");send(e,"reference");
        for(uint64_t n=10;n<13;++n)e.submit(batch(n,"lab",10));check(row(e).enrolment->profiles[0].review,"archive fixture starts with a latched RF warning");
        for(uint64_t n=13;n<31;++n){auto b=batch(n);b.capture.gain_db=100+double(n);e.submit(std::move(b));}
        auto r=row(e);check(wifi_master::has_rf_review(*r.enrolment),"archiving a context cannot hide its RF warning");
        e.submit(batch(31,"lab",2));r=row(e);
        const auto& p=r.enrolment->profiles.back();
        check(p.has_reference&&p.reference[0]==1&&p.deviation[0]==1&&p.review,"returning context restores accepted reference and warning with correct deviation");
        send(e,"review_rf");r=row(e);check(!wifi_master::has_rf_review(*r.enrolment)&&r.enrolment->profiles.back().reference[0]==1,"RF acknowledgement preserves accepted reference");
        auto stale=batch(32);stale.capture.radio_session=2;stale.observations[0].frame.radio_session=2;e.submit(std::move(stale));auto count=row(e).enrolment->observations;
        e.submit(batch(33));check(row(e).enrolment->observations==count,"old receiver session rejected after a newer session");
    }
    {
        auto path=fs::path(live)/(address+".enrol.json");std::ofstream(path)<<"{broken";
        WifiEnrolment e(live);check(row(e).enrolment->policy=="unreviewed"&&!e.health().error.empty(),"damaged policy fails closed with visible warning");
    }
    fs::remove_all(root);std::cout<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}
