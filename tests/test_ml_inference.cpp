#include "ml_inference.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace rfmon;
void require(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
template<class F>void until(F f){for(int i=0;i<300&&!f();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));require(f(),"deadline waiting for fixture");}
int main(int argc,char** argv) {
    require(argc==2,"fixture path required");
    auto selection=ml::select_intervals(20000000,160000,true);
    require(selection.count==3&&selection.offsets[0]==0&&selection.offsets[1]==9920000&&selection.offsets[2]==19840000,"capture dispersion failed");
    require(ml::select_intervals(20000000,4000000,true).count==1,"copy budget exceeded");
    require(ml::select_intervals(160000,160000,true).count==1,"short capture duplicated");
    require(ml::select_intervals(20000000,160000,false).count==1,"legacy selection changed");
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
    require(s.history[0].accepted.empty()&&s.history[0].top[0].score==.99999,"candidate became identity");
    require(s.history[0].stft&&s.history[0].stft->pixels.size()==160000,"STFT frame missing");
    require(s.history[0].stft->pixels[255]==255&&s.history[0].stft->pixels[256]==0,"STFT transport corrupted");
    require(std::string(ml::observation_decision(s.history[0])).find("presence undetermined")!=std::string::npos,"candidate became presence");
    worker.invalidate();require(!worker.snapshot().history[0].current,"retune retained current result");
    require(std::string(ml::observation_decision(worker.snapshot().history[0])).find("Historical")!=std::string::npos,"stale observation became current decision");
    worker.stop();require(!worker.snapshot().enabled,"stop failed");
    options.model="/checkspread";require(worker.start(options),"spread start failed");
    until([&]{return worker.snapshot().ready;});
    auto spread_capture=c;spread_capture.samples_received=480000;
    std::vector<std::complex<float>> spread(480000);
    for(size_t i=0;i<spread.size();++i)spread[i]={float(i),float(i)+.5f};
    worker.observe(spread,spread_capture);
    until([&]{return worker.snapshot().processed==4;});
    auto multiple=worker.snapshot();
    require(multiple.history[0].offset_samples==320000&&multiple.history[1].offset_samples==160000&&multiple.history[2].offset_samples==0,"interval provenance lost");
    require(multiple.history[0].interval_count==3&&multiple.history[0].interval_index==2,"interval grouping lost");
    require(multiple.history[0].stft&&!multiple.history[1].stft,"old display pixels retained");
    require(multiple.submitted_samples==640000&&multiple.submitted_jobs==2,"coverage counters wrong");
    worker.stop();
    options.model="/delayedspread";require(worker.start(options),"delayed spread start failed");
    until([&]{return worker.snapshot().ready;});worker.observe(spread,spread_capture);
    until([&]{return worker.snapshot().processed==5;});
    const auto stale_before=worker.snapshot().stale;
    worker.invalidate();
    until([&]{return worker.snapshot().stale>stale_before;});
    require(worker.snapshot().processed==5&&!worker.snapshot().history[0].current,"retune accepted remaining old capture intervals");
    worker.stop();
    options.model="/badpreview";require(worker.start(options),"bad preview fixture start failed");
    until([&]{return worker.snapshot().ready;});worker.observe(iq,c);
    until([&]{return worker.snapshot().state=="Failed";});
    require(worker.snapshot().error.find("STFT preview contract")!=std::string::npos,"bad STFT dimensions accepted");
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
