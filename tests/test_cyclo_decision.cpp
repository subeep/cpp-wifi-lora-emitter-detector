#include "cyclostationary/link_decision.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace rfmon::cyclo;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try {
    WaveformReview r,s,b;
    auto d=decide_link(r,s,b);
    check(std::string(d.status)=="unavailable" && std::string(d.protocol)=="unknown","missing evidence is unavailable");
    r.status="experimental_patterns";r.observed_quality_passed=true;r.passband_status="fits_declared_band";
    ReviewedPattern p;p.pattern_consistent=true;p.kind="cp_timing";r.candidates.push_back(p);
    d=decide_link(r,s,b);check(std::string(d.status)=="unknown" && std::string(d.protocol)=="unknown","CP cannot name a protocol");
    r.cp_code_ambiguous=true;
    d=decide_link(r,s,b);check(std::string(d.status)=="ambiguous_waveform_link_unknown","conflict retained");
    d=decide_link(r,s,b,{true,1,1});
    check(d.competing_patterns && d.protocol_frames==2 && std::string(d.protocol)=="wifi","two PHY modes do not erase waveform ambiguity");
    r.observed_quality_passed=false;r.passband_status="outside_declared_band";
    d=decide_link(r,s,b,{true,1,0});
    check(std::string(d.waveform_state)=="quality_rejected" && std::string(d.protocol)=="wifi" && d.competing_patterns,
        "whole-window quality does not erase independently decoded frame");
    check(d.reason_count<=d.reasons.size(),"reason bound");
    bool failed=false;try{decide_link(r,s,b,{false,1,0});}catch(const std::invalid_argument&){failed=true;}
    check(failed,"unassociated protocol facts rejected");
    WifiFactContext c{1,2,2,20e6,2438.5e6};TilePlan plan;plan.count=2;plan.tiles[0]={0,4096};plan.tiles[1]={8000,4096};
    auto obs=collect_wifi_protocol_observations(1,16000,plan,c,[&](std::size_t){
        return WifiDecodeFact{{100,1000},2437e6,6,DecodedWifiPhy::legacy_ofdm,true};});
    check(protocol_for_window(obs,c,16000,plan,plan.tiles[0]).ofdm==1,"contained protocol retained");
    check(protocol_for_window(obs,c,16000,plan,plan.tiles[1]).ofdm==0,"other tile cannot inherit protocol");
    check(!protocol_for_window(obs,c,16000,plan,{0,2048}).associated,"unselected window rejected");
    ++c.capture_sequence;check(!protocol_for_window(obs,c,16000,plan,plan.tiles[0]).associated,"stale capture rejected");
    // Repetition cannot increase specificity: the policy has no history or score accumulation.
    for(int i=0;i<100;++i)check(std::string(decide_link(r,s,b).protocol)=="unknown","repeated DSP cannot imply protocol");
    std::cout<<"bounded link decision, ambiguity, unknown and protocol association passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
