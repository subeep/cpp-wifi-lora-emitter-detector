// Dedicated receive-only X310 fixture collection. Never instantiates a TX streamer.
#include "sdr_capture.hpp"
#include "wifi_iq_capture.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <cstring>
#include <cmath>
using nlohmann::json;
int main(int argc,char** argv) {
    try {
        if(argc<5) { std::cerr<<"Usage: wifi_capture_cli NEW_DIRECTORY OFFSET_HZ SECONDS CHANNEL_HZ...\n"; return 2; }
        double offset=std::stod(argv[2]),seconds=std::stod(argv[3]);
        if(!std::isfinite(offset)||std::abs(offset)>2e6||!std::isfinite(seconds)||seconds<=0||seconds>1) throw std::runtime_error("Invalid capture limits");
        std::vector<double> channels;
        for(int i=4;i<argc;++i) { double f=std::stod(argv[i]); if(!std::isfinite(f)||f<2.4e9||f>5.9e9) throw std::runtime_error("Invalid Wi-Fi frequency"); channels.push_back(f); }
        std::filesystem::path dir(argv[1]); if(!dir.parent_path().empty())std::filesystem::create_directories(dir.parent_path());
        if(!std::filesystem::create_directory(dir)) throw std::runtime_error("Output directory must be new");
        auto profile=rfmon::device_profile(rfmon::SdrDeviceType::X310);
        rfmon::UsrpCapture radio(profile.antenna,20.0,0,profile.device_args);
        bool incomplete=false;
        const std::string run_id="wifi-iq:"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        for(size_t i=0;i<channels.size();++i) {
            double channel=channels[i];
            auto start=std::chrono::system_clock::now().time_since_epoch();
            auto captured=radio.capture_detailed(channel+offset,20e6,seconds);
            const auto& iq=captured.samples;const double rate=captured.sample_rate_hz;const bool overflow=captured.overflow;
            incomplete=incomplete||iq.empty()||overflow||captured.timing.timed_out||captured.timing.exception;
            if(iq.empty()) {std::cerr<<"Dropped empty capture for "<<channel<<" Hz\n";continue;}
            std::string stem=std::to_string(i)+"-"+std::to_string(int(channel/1e6));
            json meta={{"schema",1},{"format","cf32_le"},{"iq_file",stem+".cf32"},{"samples",iq.size()},
                {"sample_rate_hz",rate},{"requested_rate_hz",20e6},{"channel_hz",channel},{"capture_center_hz",channel+offset},
                {"center_semantics","requested tuning frequency"},{"requested_gain_db",20},{"rx_channel",0},{"antenna","RX2"},
                {"device_args",profile.device_args},{"overflow",overflow},{"host_start_ns",std::chrono::duration_cast<std::chrono::nanoseconds>(start).count()},
                {"requested_duration_s",seconds},{"run_id",run_id},{"radio_session",1},{"capture_seq",i+1}};
            rfmon::save_wifi_iq_capture((dir/(stem+".json")).string(),meta,iq,captured.timing);
            std::cout<<stem<<": "<<iq.size()<<" samples, "<<rate<<" Hz, overflow="<<overflow<<std::endl;
        }
        return incomplete ? 1 : 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
