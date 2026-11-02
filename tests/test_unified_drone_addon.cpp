#include "drones/addon.hpp"
#include <iostream>
#include <stdexcept>

static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void wait_until(const std::function<bool()>& done){
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(!done()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(done(),"worker timeout");
}
int main(){
    drone::Addon a;
    int starts=0,stops=0;
    a.start_classifier=[&]{++starts;return true;};a.stop_classifier=[&]{++stops;};
    a.start_unified();a.start_unified();
    require(starts==1&&a.unified&&a.remote_id.enabled(),"unified start must be idempotent and include classifier");
    wait_until([&]{return a.native.snapshot().ready||!a.native.running();});
    require(a.native.snapshot().ready,"native worker handshake failed");
    rfmon::wifi_security::CaptureRecord c;c.clock=rfmon::wifi_security::ClockDomain::UsrpDevice;
    c.run_id="synthetic-unified-test";c.sample_rate_hz=10e6;c.capture_center_hz=2444.5e6;
    std::vector<std::complex<float>> iq(80000);c.samples_received=iq.size();
    a.observe(iq,c);wait_until([&]{return a.native.snapshot().samples>=80000;});
    require(a.native.snapshot().o4_windows==0,"partial O4 window must remain pending");
    c.device_time_ns=8000000;c.capture_seq++;c.retuned=false;
    a.observe(iq,c);wait_until([&]{return a.native.snapshot().samples>=160000;});
    auto s=a.native.snapshot();
    require(s.o4_windows==1&&s.o4_candidates==0,"continuity transport did not preserve O4 window");
    require(s.observations.empty(),"native candidates must not enter identity tracks");
    c.device_time_ns=32000000;c.capture_seq++;
    a.observe(iq,c);wait_until([&]{return a.native.snapshot().samples>=240000;});
    require(a.native.snapshot().discontinuities==1&&a.native.snapshot().o4_windows==1,"gap did not reset context");
    a.stop_unified();a.stop_unified();
    require(stops==1&&!a.live.running()&&!a.native.running()&&!a.remote_id.enabled(),"owned workers not stopped exactly once");
    a.live.start();a.remote_id.enable(true);
    a.start_classifier=[&]{++starts;return false;}; // independently running model
    a.start_unified();a.stop_unified();
    require(stops==1&&a.live.running()&&a.remote_id.enabled(),"independent workers must survive unified stop");
    std::cout<<"unified lifecycle, native transport and continuity passed (synthetic/offline only)\n";
}
