#include "lora_receiver.hpp"
#include "lora_capture.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <random>
#include <limits>
using namespace rfmon;
namespace rx=rfmon::lora::receiver;
void require(bool ok, const std::string& message) {if(!ok)throw std::runtime_error(message);}
// Waveform builder consumes fixed physical-bin vectors, never the app encoder.
std::vector<std::complex<float>> waveform(const std::vector<double>& symbols,int sf,int sync,
                                          int preamble,int prefix,double cfo) {
 int n=1<<sf; std::vector<std::complex<float>> x(prefix);
 std::vector<std::complex<float>> up(n);
 for(int j=0;j<n;++j)up[j]=std::polar(1.f,float(2*M_PI*(double(j)*j/(2*n)-j/2.)));
 auto chirp=[&](int bin,bool down,int count){for(int j=0;j<count;++j){auto v=up[(j+bin)%n];x.push_back(down?std::conj(v):v);}};
 for(int i=0;i<preamble;++i)chirp(0,false,n);
 chirp((sync>>4)*8,false,n);chirp((sync&15)*8,false,n);
 chirp(0,true,n);chirp(0,true,n);chirp(0,true,n/4);
 for(double symbol:symbols)chirp(int(symbol),false,n);
 for(size_t i=0;i<x.size();++i)x[i]*=std::polar(1.f,float(2*M_PI*cfo*i/n));
 return x;
}
int main(int argc,char** argv){try{
 require(argc==1||(argc==2&&std::string(argv[1])=="--hardware-fixtures"),"Usage: test_lora_receiver [--hardware-fixtures]");
 auto root=std::filesystem::path(PROJECT_ROOT_DIR)/"tests/fixtures/lora_m2";
 nlohmann::json vectors;std::ifstream(root.parent_path()/"lora_standard_symbols.json")>>vectors;
 size_t tested=0;
 for(auto& v:vectors){
  auto symbols=v["symbols"].get<std::vector<double>>(); auto expected=v["payload"].get<std::vector<uint8_t>>();
  int sf=v["sf"],cr=v["cr"];bool ldro=v["ldro"],crc=v["crc"];
  auto p=rx::decode_symbols(symbols,sf,ldro);
  require(p.header_valid&&p.payload_complete&&p.payload==expected&&p.cr==cr&&p.crc_on==crc&&(!crc||p.crc_valid),"Symbol vector "+std::to_string(tested));
  if(symbols.size()>8){symbols.resize(8);auto partial=rx::decode_symbols(symbols,sf,ldro);require(partial.header_valid&&!partial.payload_complete,"Truncated symbol vector");}
  ++tested;
 }
 // Exercise full synchronization across SF7..12, both payload LDRO widths,
 // non-default sync words, arbitrary leading samples and fractional CFO.
 for(auto& v:vectors){if(!v["crc"].get<bool>()||v["source"]!="reference-derived Python vector")continue;
  int sf=v["sf"];auto expected=v["payload"].get<std::vector<uint8_t>>();
  auto iq=waveform(v["symbols"].get<std::vector<double>>(),sf,0xab,16,(1<<sf)/3,2.375);
  auto packets=rx::demodulate(iq,sf,125000);
  bool found=false;for(auto&p:packets)if(p.crc_valid&&p.payload==expected&&p.sync_word==0xab)found=true;
  require(found,"IQ vector SF"+std::to_string(sf)+" LDRO"+v["ldro"].dump());
  for (auto& sample : iq) sample=std::conj(sample);
  auto inverted=rx::demodulate(iq,sf,125000);
  found=false;
  for(auto&p:inverted) if(p.crc_valid&&p.payload==expected&&p.inverted_iq&&
       p.preamble_peak_ratio>.5&&p.sfd_peak_ratio>.5)found=true;
  require(found,"Inverted IQ vector SF"+std::to_string(sf));
 }
 // Multiple frames, truncation, corruption, negative CFO and public sync.
 const auto& base=vectors[2];
 auto fixed=base["symbols"].get<std::vector<double>>();
 auto expected=base["payload"].get<std::vector<uint8_t>>();
 auto first=waveform(fixed,7,0x34,8,47,-1.375);
 auto combined=first;combined.resize(combined.size()+512);
 auto second=waveform(fixed,7,0x12,40,23,0.25);
 combined.insert(combined.end(),second.begin(),second.end());
 auto multiple=rx::demodulate(combined,7,125000);
 require(multiple.size()==2&&multiple[0].crc_valid&&multiple[1].crc_valid&&
         multiple[0].payload==expected&&multiple[1].payload==expected,"Multiple frames lost");
 auto truncated=first;truncated.resize(truncated.size()-128*5);
 auto partial=rx::demodulate(truncated,7,125000);
 require(partial.size()==1&&partial[0].header_valid&&!partial[0].payload_complete,"Partial IQ header lost");
 auto corrupt=first;std::fill(corrupt.end()-128*5,corrupt.end(),std::complex<float>{});
 auto failed=rx::demodulate(corrupt,7,125000);
 require(failed.size()==1&&!failed[0].crc_valid,"Corrupt IQ CRC accepted");
 auto damaged=fixed;
 for(size_t i=8;i<damaged.size();++i)damaged[i]=std::fmod(damaged[i]+13,128);
 auto bad_packet=rx::demodulate(waveform(damaged,7,0x34,16,47,0.375),7,125000);
 require(bad_packet.size()==1&&bad_packet[0].header_valid&&bad_packet[0].payload_complete&&
         !bad_packet[0].crc_valid,"Corrupted symbols accepted by CRC");
 // Independent SX1262 hardware fixtures are small and included in portable CI.
 nlohmann::json hardware;auto sxroot=root.parent_path()/"lora_sx1262";
 std::ifstream(sxroot/"expected.json")>>hardware;
 for(const auto& fixture:hardware) {
  auto c=load_lora_capture((sxroot/fixture["directory"].get<std::string>()).string());
  require(c.sample_rate_hz==500000&&!c.overflow,"SX1262 fixture metadata");
  std::vector<std::complex<float>> iq(c.iq.size()/4);
  for(size_t i=0;i<iq.size();++i)for(int j=0;j<4;++j)iq[i]+=c.iq[i*4+j]/4.f;
  auto packets=rx::demodulate(iq,7,125000);bool found=false;
  for(const auto& p:packets) {
   std::string hex;const char* digits="0123456789abcdef";
   for(auto b:p.payload){hex+=digits[b>>4];hex+=digits[b&15];}
   if(p.crc_valid&&p.cr==fixture["cr"].get<int>()&&p.sync_word==fixture["sync_word"].get<int>()&&
      !p.inverted_iq&&hex==fixture["payload_hex"].get<std::string>())found=true;
  }
  require(found,"SX1262 byte-exact regression "+fixture["directory"].get<std::string>());
 }
 require(hardware.size()==3,"Expected three SX1262 recordings");
 auto broken_header=fixed;std::fill(broken_header.begin(),broken_header.begin()+8,1);
 rx::Diagnostics diagnostics;
 auto rejected_header=rx::demodulate(waveform(broken_header,7,0x12,16,47,0.375),7,125000,{},&diagnostics);
 require(rejected_header.empty()&&diagnostics.preamble_candidates>0&&diagnostics.aligned_candidates>0&&
         diagnostics.headers_rejected>0,"Synchronization failure diagnostics missing");
 size_t captures=0;
 if(argc>1) {
 for(auto& entry:std::filesystem::recursive_directory_iterator(root)){
  if(entry.path().filename()!="manifest.json" || entry.path().parent_path().filename()=="raw-capture")continue;
  auto c=load_lora_capture(entry.path().parent_path().string());
  int factor=int(c.sample_rate_hz/125000);std::vector<std::complex<float>> iq(c.iq.size()/factor);
  for(size_t i=0;i<iq.size();++i)for(int j=0;j<factor;++j)iq[i]+=c.iq[i*factor+j]/float(factor);
  auto packets=rx::demodulate(iq,12,125000);bool found=false;
  for(auto&p:packets){
   const auto name=entry.path().parent_path().filename().string();
   int counter=(name.find("5151649527")!=std::string::npos)?3:
               (name.find("5584193236")!=std::string::npos||name.find("3743354033900")!=std::string::npos)?1:2;
   std::vector<uint8_t> expected={0x0f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
       0x3c,0xc1,0xf6,0x05,0x00,0x05,0xd8,0xe0,0x00,0x00,uint8_t(counter),0x00,0x00};
   const std::string sent="SF12_BW125_TEST";expected.insert(expected.end(),sent.begin(),sent.end());
   if(p.crc_valid&&p.payload==expected&&p.cr==1&&p.sync_word==0x12)found=true;
  }
  require(found,"Recorded capture "+entry.path().string());++captures;
 }
 require(captures==6,"Expected all six hardware fixtures");
 }
 std::mt19937 rng(19);std::normal_distribution<float> noise;
 std::vector<std::complex<float>> random_iq(100000);
 for(auto& sample:random_iq)sample={noise(rng),noise(rng)};
 require(rx::demodulate(random_iq,7,125000).empty(),"Noise accepted");
 require(rx::demodulate(std::vector<std::complex<float>>(100000),7,125000).empty(),"Silence accepted");
 require(rx::demodulate({},6,125000).empty(),"Unsupported SF accepted");
 require(!rx::decode_symbols(std::vector<double>(8,std::numeric_limits<double>::quiet_NaN()),7,false).header_valid,"NaN accepted");
 std::cout<<"PASS: "<<tested<<" symbol vectors, 96 normal/inverted synchronization cases, "<<captures<<" real CRC-valid captures, multiple frames, truncation and negative controls\n";
 }catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
