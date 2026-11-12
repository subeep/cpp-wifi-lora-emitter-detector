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
    // A whole visit arrives as consecutive <=160 ms chunks; all of them must reach the backend.
    {
        const auto before=a.native.snapshot();
        a.native_max_capture_s=3600.0;
        std::vector<std::complex<float>> visit(5000000); // 0.5 s at 10 MSps -> four chunks
        c.device_time_ns=1000000000;c.capture_seq++;c.samples_received=visit.size();
        const auto chunks_before=a.native_chunks.load();
        a.observe(visit,c);
        require(a.native_chunks-chunks_before==4,"visit was not split into four chunks");
        wait_until([&]{return a.native.snapshot().samples>=before.samples+visit.size();});
        const auto after=a.native.snapshot();
        require(after.dropped_full==before.dropped_full&&after.dropped_busy==before.dropped_busy&&
                after.dropped==before.dropped,"chunks of one visit were dropped");
        require(after.discontinuities==before.discontinuities+1,"visit chunks were not contiguous after the retune gap");
        // Flooding must be refused and counted, never queued without bound.
        for(int i=0;i<12;++i){c.device_time_ns+=2000000000;c.capture_seq++;a.observe(visit,c);}
        const auto flooded=a.native.snapshot();
        require(flooded.dropped_full>0&&flooded.queue_depth<=drone::kNativeQueueChunks,"flood was not bounded and counted");
        require(flooded.dropped>=flooded.dropped_full,"dropped sample total must include refused chunks");
        a.native_max_capture_s=0.160;
        // A DroneID follow-up capture is offered whole (whatever the per-capture limit) and takes
        // the place of waiting ordinary chunks instead of being refused by the full queue.
        const uint64_t followup_seq=c.capture_seq+1;
        a.native_priority=[&](uint64_t seq){return seq==followup_seq;};
        std::vector<std::complex<float>> followup(10000000); // 1 s at 10 MSps -> seven chunks
        c.device_time_ns+=2000000000;c.capture_seq=followup_seq;c.samples_received=followup.size();
        a.observe(followup,c);
        const auto served=a.native.snapshot();
        require(served.priority_chunks==7,"follow-up capture was not accepted whole");
        require(served.displaced_chunks>0&&served.dropped_full>=flooded.dropped_full+served.displaced_chunks,
                "waiting ordinary chunks were not displaced and counted");
        require(served.queue_depth<=drone::kNativeQueueChunks,"priority must not exceed the queue bound");
        c.device_time_ns+=2000000000;c.capture_seq++;c.samples_received=visit.size();
        a.observe(visit,c);
        require(a.native.snapshot().priority_chunks==7,"an ordinary capture must not be given priority");
        a.native_priority=nullptr;
    }
    a.stop_unified();a.stop_unified();
    require(stops==1&&!a.live.running()&&!a.native.running()&&!a.remote_id.enabled(),"owned workers not stopped exactly once");
    a.live.start();a.remote_id.enable(true);
    a.start_classifier=[&]{++starts;return false;}; // independently running model
    a.start_unified();a.stop_unified();
    require(stops==1&&a.live.running()&&a.remote_id.enabled(),"independent workers must survive unified stop");
    std::cout<<"unified lifecycle, native transport and continuity passed (synthetic/offline only)\n";
}
