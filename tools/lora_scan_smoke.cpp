// Finite receive-only production LoRa lane validation, with isolated stores.
// Saves the first capture's IQ for comparison with the live event batch.
#include "scanner.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
int main(int argc,char** argv) {
    using namespace rfmon;
    try {
        if(argc!=4) {std::cerr<<"Usage: lora_scan_smoke CENTER_HZ WALL_SECONDS NEW_DIRECTORY (X310 receive-only)\n";return argc==2&&std::string(argv[1])=="--help"?0:2;}
        size_t f_end=0,s_end=0;double frequency=std::stod(argv[1],&f_end);int seconds=std::stoi(argv[2],&s_end);
        if(f_end!=std::string(argv[1]).size()||s_end!=std::string(argv[2]).size()||!std::isfinite(frequency)||
            frequency<10e6||frequency>6e9||seconds<5||seconds>60)throw std::runtime_error("Invalid receive-only limits");
        auto root=std::filesystem::absolute(argv[3]);
        if(!std::filesystem::create_directories(root))throw std::runtime_error("Output directory must be new");
        // The legacy RF fingerprint log is relative to cwd; isolate it too.
        std::filesystem::current_path(root);
        Scanner scanner(root.string());scanner.set_device_type(SdrDeviceType::X310);
        scanner.set_gain(5.0);scanner.set_active_band(BAND_SUB_GHZ);
        scanner.set_lora_lock_freq(frequency);scanner.set_lora_capture_seconds(2.0);
        scanner.request_lora_capture_save((root/"iq").string());scanner.start();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
        while(std::chrono::steady_clock::now()<deadline) {
            auto snapshot=scanner.lora_security_snapshot();
            if(snapshot->captures>=3&&!scanner.lora_capture_pending())break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        auto status=scanner.status();scanner.stop();auto snapshot=scanner.lora_security_snapshot();
        std::ofstream out(root/"snapshot.json");out<<lora_security::snapshot_json(*snapshot).dump(2)<<'\n';out.close();
        std::cout<<"Connected="<<status.connected<<" captures="<<snapshot->captures<<" device_timed="<<snapshot->device_timed
            <<" usable_s="<<snapshot->usable_s<<" events="<<snapshot->events<<" excluded="<<snapshot->excluded
            <<" queue_loss="<<snapshot->input_loss<<" error="<<status.error<<'\n'<<scanner.lora_capture_message()<<'\n';
        return out&&status.connected&&snapshot->captures>=3&&snapshot->device_timed==snapshot->captures&&
            snapshot->excluded==0&&snapshot->input_loss==0&&snapshot->rejected_inputs==0&&snapshot->recording_error.empty()&&
            scanner.lora_capture_message().rfind("Saved: ",0)==0?0:1;
    }catch(const std::exception& e){std::cerr<<"lora_scan_smoke: "<<e.what()<<'\n';return 1;}
}
