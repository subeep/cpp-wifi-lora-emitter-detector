#include "ml_inference.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace rfmon;
void require(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
template<class F>void until(F f){for(int i=0;i<300&&!f();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));require(f(),"deadline waiting for fixture");}
int main(int argc,char** argv) {
    require(argc==2,"fixture path required");
    ml::Inference worker;
    wifi_security::CaptureRecord c;c.run_id="fixture";c.radio_session=1;c.capture_seq=9007199254740993ull;
    c.samples_received=160000;c.sample_rate_hz=20e6;c.capture_center_hz=2440e6;c.band="wifi_2g4";
    std::vector<std::complex<float>> iq(160000,{1,2}),before=iq;
    worker.observe(iq,c);require(worker.snapshot().offered==0,"disabled observer did work");
    ml::Options options{"/usr/bin/python3",argv[1],"/normal",500};
    require(worker.start(options),"start failed");until([&]{return worker.snapshot().ready;});
    auto bad=c;bad.overflows.push_back({0,{}});
    worker.observe(iq,bad);require(worker.snapshot().invalid==1,"gap accepted");
    bad=c;bad.sample_rate_hz=10e6;worker.observe(iq,bad);require(worker.snapshot().invalid==2,"narrow capture accepted");
    worker.observe(iq,c);worker.observe(iq,c);
    until([&]{return worker.snapshot().processed==1;});
    auto s=worker.snapshot();require(s.dropped>0,"bounded duty not enforced");
    require(iq==before,"observer modified receiver IQ");
    require(s.history[0].capture==c.capture_seq,"64-bit provenance lost");
    require(s.history[0].accepted.empty()&&s.history[0].top[0].score==.8,"candidate became identity");
    worker.invalidate();require(!worker.snapshot().history[0].current,"retune retained current result");
    worker.stop();require(!worker.snapshot().enabled,"stop failed");
    options.model="/slow";require(worker.start(options),"restart failed");until([&]{return worker.snapshot().ready;});
    worker.observe(iq,c);until([&]{return worker.snapshot().state=="Failed";});
    require(worker.snapshot().error.find("timeout")!=std::string::npos,"deadline not enforced");
    options.model="/crash";require(worker.start(options),"restart after timeout failed");
    until([&]{return worker.snapshot().ready;});worker.observe(iq,c);
    until([&]{return worker.snapshot().state=="Failed";});
    worker.clear_history();require(worker.snapshot().history.empty(),"clear failed");
    options.python="/missing/python";require(worker.start(options),"failure test not started");
    until([&]{return worker.snapshot().state=="Failed";});
    // Receiver callbacks may overlap UI stop/restart and slot allocation.
    options.python="/usr/bin/python3";options.model="/normal";
    std::atomic<bool> producing{true};
    std::thread producer([&]{while(producing){worker.observe(iq,c);std::this_thread::yield();}});
    try {
        for(int i=0;i<8;++i){
            require(worker.start(options),"concurrent restart failed");
            until([&]{return worker.snapshot().ready;});
            worker.invalidate();worker.stop();
            require(!worker.snapshot().enabled,"concurrent stop failed");
        }
    } catch(...) {producing=false;producer.join();throw;}
    producing=false;producer.join();
    require(iq==before,"concurrent observer modified receiver IQ");
    std::cout<<"Disabled isolation, gaps, rate, IQ preservation, queue, exact IDs, invalidation, timeout, crash and concurrent restart: PASS\n";
}
