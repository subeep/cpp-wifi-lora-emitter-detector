// Finite receive-only check of the production scanner. Never selects sub-GHz.
#include "scanner.hpp"
#include <chrono>
#include <iostream>
#include <thread>
int main(int argc,char**argv) {
    using namespace rfmon;
    if(argc!=2||(std::string(argv[1])!="2g4"&&std::string(argv[1])!="5g")){
        std::cerr<<"Usage: wifi_scan_smoke 2g4|5g (X310; records live observations)\n";return 2;
    }
    Scanner scanner;
    scanner.set_device_type(SdrDeviceType::X310);
    scanner.set_active_band(std::string(argv[1])=="5g"?BAND_WIFI_5G:BAND_WIFI_2G4);
    scanner.start();
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
    bool found=false;
    while(std::chrono::steady_clock::now()<deadline&&!found){
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        for(const auto& p:scanner.wifi_packets())if(p.modulation=="OFDM"&&p.identity&&p.fcs_valid){
            std::cout<<p.identity->bssid<<" "<<wifi::display_text(p.identity->ssid)<<" "<<p.ofdm_rate_mbps<<" Mbps key="<<p.master_key<<std::endl;found=true;
        }
    }
    auto status=scanner.status();scanner.stop();
    std::cout<<"Connected="<<status.connected<<" overflow="<<status.last_overflow<<" error="<<status.error<<'\n';
    return found?0:1;
}
