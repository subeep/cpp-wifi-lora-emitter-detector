#include "wifi_ofdm_rx.hpp"
#include "wifi_iq_capture.hpp"
#include "wifi_phy.hpp"
#include "config.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <cstring>
using nlohmann::json;
int main(int argc,char**argv){try{
 if(argc<2){std::cerr<<"Usage: wifi_ofdm_replay MANIFEST.json [--packets OUTPUT.ndjson] [--threshold-db DB]\n";return 2;}
 double threshold=rfmon::DEFAULT_DETECTION_THRESHOLD_DB;std::string packet_path;
 for(int a=2;a<argc;a+=2){if(a+1>=argc)throw std::runtime_error("Missing option value");std::string option=argv[a];if(option=="--packets")packet_path=argv[a+1];else if(option=="--threshold-db")threshold=std::stod(argv[a+1]);else throw std::runtime_error("Unknown option");}
 if(!std::isfinite(threshold)||threshold<0||threshold>50)throw std::runtime_error("Invalid threshold");
 auto capture=rfmon::load_wifi_iq_capture(argv[1]);const auto& j=capture.metadata;const auto& iq=capture.iq;
 size_t n=iq.size();double rate=j.at("sample_rate_hz"),center=j.at("capture_center_hz"),channel=j.at("channel_hz");
 if(rate<20e6 || j.value("overflow",true) || (capture.timing && (capture.timing->timed_out || capture.timing->exception)))throw std::runtime_error("Invalid/overflowed capture");
 auto bursts=rfmon::wifi::detect_bursts(iq.data(),iq.size(),rate,threshold,rfmon::WIFI_MAX_BURSTS_PER_CAPTURE);
 std::map<std::string,int> counts;std::map<int,int> rates;std::ofstream packets;
 if(!packet_path.empty()){if(std::filesystem::exists(packet_path))throw std::runtime_error("Packet output must be new");packets.open(packet_path);if(!packets)throw std::runtime_error("Cannot open packet output");}
 for(const auto&b:bursts){size_t pad=size_t(rate*4e-6);size_t start=b.start>pad?b.start-pad:0;size_t end=std::min(n,b.start+b.length+pad);
  auto r=rfmon::wifi::decode_ofdm_burst(iq.data()+start,end-start,rate,center,channel);++counts[r.status];if(r.header_valid)++rates[r.rate_mbps];
  if(r.beacon)std::cout<<"BEACON "<<r.rate_mbps<<" Mbps "<<r.beacon->bssid<<" "<<rfmon::wifi::display_text(r.beacon->ssid)<<" ch="<<r.beacon->channel<<" start="<<start<<" length="<<end-start<<'\n';
  if(packets.is_open()){json p={{"start",start},{"length",end-start},{"status",r.status},{"rate_mbps",r.rate_mbps},{"psdu_length",r.psdu_length},{"fcs_valid",r.fcs_valid},{"ltf_corr",r.ltf_correlation},{"cfo_hz",r.cfo_hz}};
   if(r.fcs_valid){std::string hex;const char*d="0123456789abcdef";for(auto c:r.mpdu){hex+=d[c>>4];hex+=d[c&15];}p["mpdu_hex"]=hex;}packets<<p.dump()<<'\n';}
 }
 if(packets.is_open()){packets.flush();if(!packets)throw std::runtime_error("Packet output failed");}
 std::cout<<"threshold_db="<<threshold<<" bursts="<<bursts.size()<<" results="<<json(counts).dump()<<" header rates=";for(auto [r,count]:rates)std::cout<<r<<":"<<count<<" ";std::cout<<'\n';
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
