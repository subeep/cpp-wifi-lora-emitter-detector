// Bounded offline check through the same supervisor used by the GUI. No radio.
#include "ml_inference.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv) {
    if(argc!=6){std::cerr<<"usage: ml_inference_probe PYTHON WORKER MODEL CF32 RATE_HZ\n";return 2;}
    try {
        const double rate=std::stod(argv[5]);
        if(!std::isfinite(rate)||rate<20e6||rate>500e6)throw std::runtime_error("invalid rate");
        const size_t n=std::llround(rate*.008);
        std::vector<std::complex<float>> iq(n);
        std::ifstream f(argv[4],std::ios::binary);
        if(!f.read(reinterpret_cast<char*>(iq.data()),n*8))throw std::runtime_error("short IQ file");
        rfmon::ml::Inference engine;
        if(!engine.start({argv[1],argv[2],argv[3],15000}))throw std::runtime_error("start failed");
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(70);
        while(!engine.snapshot().ready) {
            auto s=engine.snapshot();
            if(s.state=="Failed"||std::chrono::steady_clock::now()>deadline)throw std::runtime_error("load failed: "+s.error);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        rfmon::wifi_security::CaptureRecord c;
        c.run_id="offline-model-probe";c.source="offline";c.samples_received=n;c.sample_rate_hz=rate;c.capture_seq=1;
        engine.observe(iq,c);
        while(engine.snapshot().history.empty()) {
            auto s=engine.snapshot();
            if(s.state=="Failed"||std::chrono::steady_clock::now()>deadline)throw std::runtime_error("inference failed: "+s.error);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::cout<<engine.snapshot().history.front().raw<<'\n';engine.stop();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
