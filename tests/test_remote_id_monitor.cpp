#include "drones/remote_id.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using J=drone::RidJson;
void require(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
rfmon::wifi_security::FrameEvent event(const std::string& hex,int64_t time){
    rfmon::wifi_security::FrameEvent e;e.fcs_valid=true;e.run_id="synthetic-test";e.radio_session=1;e.host_time_ns=time;e.phy="OFDM";
    for(size_t i=0;i<hex.size();i+=2)e.mpdu.push_back(std::stoul(hex.substr(i,2),nullptr,16));return e;
}
void wait(drone::RemoteIdMonitor& m,uint64_t n){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(m.snapshot()["examined"].get<uint64_t>()<n&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(5));require(m.snapshot()["examined"].get<uint64_t>()==n,"monitor timeout");}
const J& msg(const J& t,const char* type){for(const auto& m:t["messages"])if(m["type"]==type)return m;throw std::runtime_error("message missing");}
int main(){
    std::ifstream input(std::string(PROJECT_ROOT_DIR)+"/tests/fixtures/remote_id/monitor_frames.json");J frames;input>>frames;
    auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    drone::RemoteIdMonitor m;
    m.submit(event(frames["full"],now));require(m.snapshot()["examined"]==0,"disabled observer active");
    m.enable(true);m.submit(event(frames["full"],now-20000000000LL));wait(m,1);
    auto s=m.snapshot();require(s["tracks"].size()==1&&s["accepted"]==1,"reference not decoded");
    require(msg(s["tracks"][0],"Basic ID")["stale"]==true,"old identity fresh");
    m.submit(event(frames["location"],now-1000000000LL));wait(m,2);s=m.snapshot();
    require(msg(s["tracks"][0],"Basic ID")["stale"]==true,"location refreshed identity");
    require(msg(s["tracks"][0],"Location")["stale"]==false,"recent location stale");
    m.submit(event(frames["unavailable"],now));wait(m,3);s=m.snapshot();
    require(msg(s["tracks"][0],"Location")["latitude"].is_null(),"unavailable position retained old value");
    m.submit(event(frames["full"],now-10000000000LL));wait(m,4);s=m.snapshot();require(s["out_of_order"]==1,"old frame rolled track back");
    m.submit(event(frames["changed_id"],now+1));wait(m,5);s=m.snapshot();
    require(s["tracks"][0]["messages"].size()==1,"changed identity retained old telemetry");
    auto e=event(frames["full"],now+2);e.radio_session=2;m.submit(e);wait(m,6);require(m.snapshot()["tracks"].size()==2,"sessions merged");
    e.mpdu_truncated=true;m.submit(e);wait(m,7);require(m.snapshot()["accepted"]==5,"truncated MPDU accepted");
    m.enable(false);require(!m.snapshot()["enabled"].get<bool>()&&m.snapshot()["tracks"].size()==2,"disable did not retain history");
    m.enable(true);require(m.snapshot()["tracks"].empty(),"restart failed to reset evidence");
    for(int i=0;i<66;++i){auto f=event(frames["full"],now);f.radio_session=i+100;m.submit(f);wait(m,i+1);}
    require(m.snapshot()["tracks"].size()==64,"source retention is not bounded");
    m.enable(false);
    std::cout<<"Remote ID monitor: isolation, per-message age, unavailable values, reset, ordering and provenance passed\n";
}
