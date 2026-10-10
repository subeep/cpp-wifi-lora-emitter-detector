#include "cyclostationary/wifi_protocol_adapter.hpp"
#include "cyclostationary/iq_file.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace rfmon;
using namespace rfmon::cyclo;
namespace {
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
void metadata_checks() {
    WifiFactContext c{9,21,2,200e6,2444.5e6};
    TilePlan p;p.count=2;p.tiles[0]={100,10000};p.tiles[1]={30000,10000};
    WifiDecodeFact good{{200,1000},2443e6,6,DecodedWifiPhy::legacy_ofdm,true};
    std::array<WifiDecodeFact,6> input{good,good,good,good,good,good};
    input[1].verified=false;input[2].decoder_window={50000,1};
    input[3].decoder_window={10000,200}; // overlaps but is not contained
    input[4].decoder_window={30100,200};input[5].decoder_window={500,100};
    auto r=collect_wifi_protocol_observations(input.size(),50000,p,c,[&](std::size_t i){return input[i];});
    check(r.count==3 && r.no_verified_frame==1 && r.invalid_evidence==1 && r.outside_sampled_windows==1,
        "complete-window containment and prefix accounting");
    check(valid_wifi_observations(r,c,50000,p),"bounded context accepted");
    auto other=c;other.capture_sequence++;
    check(!valid_wifi_observations(r,other,50000,p),"cross-capture evidence rejected");
    other=c;other.radio_session++;
    check(!valid_wifi_observations(r,other,50000,p),"cross-session evidence rejected");
    other=c;other.sample_rate_hz=100e6;
    check(!valid_wifi_observations(r,other,50000,p),"sample-rate mismatch rejected");
    auto broken=r;broken.count=9;
    check(!valid_wifi_observations(broken,c,50000,p),"oversized metadata rejected");
    broken=r;broken.facts[0].channel_hz=c.capture_center_hz+c.sample_rate_hz;
    check(!valid_wifi_observations(broken,c,50000,p),"off-capture frequency rejected");
    broken=r;broken.facts[0].phy=DecodedWifiPhy::unknown;
    check(!valid_wifi_observations(broken,c,50000,p),"generic waveform cannot imply protocol");
    broken=r;broken.prefix_samples++;
    check(!valid_wifi_observations(broken,c,50000,p),"different continuity prefix rejected");
    std::size_t calls=0;
    const auto capped=collect_wifi_protocol_observations(1000000,50000,p,c,[&](std::size_t i){
        check(i<1000000,"descriptor selection out of bounds");++calls;return good;});
    check(calls==512 && capped.count==2 && capped.budget_omitted==510,"metadata scan and per-tile bounds");
    check(valid_wifi_observations(capped,c,50000,p),"capped accounting validates");
    check(!contains_window({100,1000},{std::numeric_limits<std::size_t>::max(),100}),"overflow-safe containment");
    good.phy=DecodedWifiPhy::long_dsss_1mbps;good.rate_mbps=1;
    check(valid_wifi_fact(good,c,50000),"1 Mbps DSSS metadata supported in 2.4 GHz");
    c.band_ghz=5;
    check(!valid_wifi_fact(good,c,50000),"DSSS not attributed to 5 GHz");
    wifi::BurstWork w;w.cls.outcome=wifi::BurstClassification::Outcome::Accepted;
    w.cls.result.mod=wifi::ModClass::DSSS;w.dsss.policy.decode=true;w.dsss.result.emplace();
    auto& d=*w.dsss.result;d.preamble_found=true;d.rate_supported=true;d.psdu_complete=true;d.fcs_valid=true;
    d.mpdu.assign(14,0);d.mpdu_len=14;d.plcp.emplace();d.plcp->crc_valid=true;d.plcp->signal=0x0a;
    check(wifi_fact_from_existing_decode({200,1000},w,50000,56e6,2443e6).verified,"complete existing DSSS checks retained");
    d.psdu_complete=false;
    check(!wifi_fact_from_existing_decode({200,1000},w,50000,56e6,2443e6).verified,"partial DSSS payload rejected");
    d.psdu_complete=true;d.plcp->signal=0x14;
    check(!wifi_fact_from_existing_decode({200,1000},w,50000,56e6,2443e6).verified,"unsupported DSSS rate rejected");
}
void received_fixtures(const std::filesystem::path& root) {
    for(const auto name:{"airtel-2g4","avgarde-5g","avgarde-5g-offset"}) {
        std::ifstream file(root/(std::string(name)+".json"));nlohmann::json h;file>>h;
        const auto iq=read_iq_window(root/h.at("iq_file").get<std::string>(),IqFormat::cf32_le,0,h.at("samples"));
        const double rate=h.at("sample_rate_hz"),center=h.at("capture_center_hz"),channel=h.at("channel_hz");
        wifi::BurstWork w;w.cls.outcome=wifi::BurstClassification::Outcome::Accepted;w.cls.result.mod=wifi::ModClass::OFDM;
        w.ofdm=wifi::decode_ofdm_burst(iq.samples.data(),iq.samples.size(),rate,center,channel);
        check(w.ofdm->fcs_valid,"received fixture still passes existing decoder FCS");
        const auto fact=wifi_fact_from_existing_decode({0,iq.samples.size()},w,iq.samples.size(),rate,channel);
        check(fact.verified && fact.rate_mbps==h.at("expected_rate_mbps"),"received frame produces protocol evidence");
        const WifiFactContext c{3,12,channel<3e9?2:5,rate,center};const auto plan=plan_sample_tiles(iq.samples.size());
        const auto evidence=collect_wifi_protocol_observations(1,iq.samples.size(),plan,c,[&](std::size_t){return fact;});
        check(evidence.count==1 && valid_wifi_observations(evidence,c,iq.samples.size(),plan),"received proof stays attached to own samples");
        auto cut=plan;cut.tiles[0].samples--;
        check(!valid_wifi_observations(evidence,c,iq.samples.size(),cut),"one-sample-truncated supporting window rejected");
        w.ofdm->fcs_valid=false;
        check(!wifi_fact_from_existing_decode({0,iq.samples.size()},w,iq.samples.size(),rate,channel).verified,"CRC failure removes protocol evidence");
        w.ofdm->fcs_valid=true;w.ofdm->header_valid=false;
        check(!wifi_fact_from_existing_decode({0,iq.samples.size()},w,iq.samples.size(),rate,channel).verified,"header failure removes protocol evidence");
        w.ofdm->header_valid=true;w.ofdm->samples_required=iq.samples.size()+1;
        check(!wifi_fact_from_existing_decode({0,iq.samples.size()},w,iq.samples.size(),rate,channel).verified,"incomplete OFDM payload rejected");
    }
}
}
int main(int argc,char** argv) {
    try {check(argc==2,"fixture directory required");metadata_checks();received_fixtures(argv[1]);
        std::cout<<"protocol metadata bounds, negative association checks and three received Wi-Fi fixtures passed\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
