// Finite receive-only check of the production scanner. Never selects sub-GHz.
#include "scanner.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <filesystem>
#include <fstream>
#include <stdexcept>
int main(int argc,char**argv) {
    using namespace rfmon;
    if((argc!=2 && (argc<5 || argc%2==0))||(std::string(argv[1])!="2g4"&&std::string(argv[1])!="5g")){
        std::cerr<<"Usage: wifi_scan_smoke 2g4|5g [CHANNEL WALL_SECONDS NEW_DIRECTORY [--gain DB] [--stop-file PATH]] (X310 receive-only)\n";return 2;
    }
    const bool campaign = argc >= 5;
    int channel = 0, seconds = 90;
    std::optional<double> gain;
    std::string stop_file;
    if (campaign) {
        try {
            size_t channel_end = 0, seconds_end = 0;
            channel = std::stoi(argv[2], &channel_end); seconds = std::stoi(argv[3], &seconds_end);
            if (channel_end != std::string(argv[2]).size() || seconds_end != std::string(argv[3]).size())
                throw std::runtime_error("Expected integer channel and seconds");
            for(int a=5;a<argc;a+=2) {
                if(std::string(argv[a])=="--gain" && !gain) {
                    size_t gain_end=0;gain=std::stod(argv[a+1],&gain_end);
                    if(gain_end!=std::string(argv[a+1]).size() || !std::isfinite(*gain) || *gain<0 || *gain>31.5)
                        throw std::runtime_error("Expected X310 gain in [0,31.5] dB");
                } else if(std::string(argv[a])=="--stop-file" && stop_file.empty()) {
                    stop_file=argv[a+1];
                    if(stop_file.empty() || std::filesystem::exists(stop_file))throw std::runtime_error("Stop file must be new");
                } else throw std::runtime_error("Unknown or repeated option");
            }
            if (seconds < 1 || seconds > 600 ||
                (std::string(argv[1]) == "2g4" ? channel < 1 || channel > 13 :
                 !wifi_5g_channels().count(channel))) throw std::runtime_error("Invalid limits");
            if (!std::filesystem::create_directories(argv[4])) throw std::runtime_error("Test directory must be new");
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
    }
    Scanner scanner(campaign ? argv[4] : std::string(PROJECT_ROOT_DIR) + "/data");
    if(gain)scanner.set_gain(*gain);
    if (campaign) {
        scanner.set_wifi_fixed_channel(channel);
        if (!scanner.set_wifi_security_recording(std::string(argv[4])+"/events.ndjson")) return 2;
    }
    scanner.set_device_type(SdrDeviceType::X310);
    scanner.set_active_band(std::string(argv[1])=="5g"?BAND_WIFI_5G:BAND_WIFI_2G4);
    scanner.start();
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    bool found=false;
    while(std::chrono::steady_clock::now()<deadline&&(!found || campaign)){
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if(!stop_file.empty() && std::filesystem::exists(stop_file))break;
        if (campaign) continue;
        for(const auto& p:scanner.wifi_packets())if(p.modulation=="OFDM"&&p.identity&&p.fcs_valid){
            std::cout<<p.identity->bssid<<" "<<wifi::display_text(p.identity->ssid)<<" "<<p.ofdm_rate_mbps<<" Mbps key="<<p.master_key<<std::endl;found=true;
        }
    }
    auto status=scanner.status();scanner.stop();
    std::cout<<"Connected="<<status.connected<<" overflow="<<status.last_overflow<<" error="<<status.error<<'\n';
    if (campaign) {
        auto snapshot = scanner.wifi_security_snapshot();
        std::ofstream out(std::string(argv[4])+"/snapshot.json");
        out << wifi_security::snapshot_json(*snapshot,true).dump(2) << '\n';
        std::cout << "Accepted=" << snapshot->frames_accepted
                  << " replay_evaluated=" << snapshot->beacon_replay_evaluated
                  << " replay_excluded=" << snapshot->beacon_replay_excluded
                  << " incidents=" << snapshot->incidents_total << std::endl;
        out.close();
        return out && !status.wifi_fixed_channel_invalid && snapshot->frames_accepted && snapshot->beacon_replay_evaluated ? 0 : 1;
    }
    return found?0:1;
}
