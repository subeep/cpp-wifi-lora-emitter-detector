// Receive-only Linux worker. No UHD/radio linkage. Raw IQ in, bounded evidence out.
#include "droneid/dji.h"
#include "droneid/o4_detector.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <sys/resource.h>

using Json=nlohmann::json;
using Sample=droneid::cf32;
static_assert(sizeof(Sample)==8,"cf32 ABI required");

int main() {
    // Independent process: failures/resources cannot take down scanner or legacy decoder.
    rlimit memory{size_t(3)<<30,size_t(3)<<30};setrlimit(RLIMIT_AS,&memory);
    std::cout<<"{\"ready\":true,\"backend\":\"punjdrone_linux_v1\"}"<<std::endl;
    std::unique_ptr<droneid::O4CandidateDetector> detector;
    std::vector<Sample> tail;
    uint64_t last_epoch=0,position=0;double last_rate=0,last_center=0,expected_time=0;
    bool initialized=false;
    std::vector<std::pair<int64_t,std::string>> recent;
    while(true) {
        char header[40];std::cin.read(header,40);
        if(std::cin.gcount()==0)return 0;
        if(std::cin.gcount()!=40 || std::memcmp(header,"DDIQ",4))return 2;
        uint32_t count;double rate,center,time;uint64_t epoch;
        std::memcpy(&count,header+4,4);std::memcpy(&rate,header+8,8);
        std::memcpy(&center,header+16,8);std::memcpy(&time,header+24,8);std::memcpy(&epoch,header+32,8);
        if(!count||count>40000000||!std::isfinite(rate)||rate<10e6||rate>250e6||
           !std::isfinite(center)||!std::isfinite(time)||count/rate>.160001)return 2;
        std::vector<Sample> input(count);
        if(!std::cin.read(reinterpret_cast<char*>(input.data()),count*sizeof(Sample)))return 2;
        Json result={{"events",Json::array()},{"native_packets",Json::array()},
                     {"o4_evidence",Json::array()},{"packet_rows_omitted",0},{"identity_confirmed",false}};
        const auto begin=std::chrono::steady_clock::now();
        try {
            if(!std::all_of(input.begin(),input.end(),[](Sample x){return std::isfinite(x.real())&&std::isfinite(x.imag());}))
                throw std::runtime_error("non-finite IQ; continuity reset");
            if(!initialized||epoch!=last_epoch||rate!=last_rate||center!=last_center||
               std::abs(time-expected_time)>std::max(2/rate,1e-9)) {
                detector=std::make_unique<droneid::O4CandidateDetector>(rate);
                tail.clear();recent.clear();position=0;
            }
            initialized=true;last_epoch=epoch;last_rate=rate;last_center=center;expected_time=time+count/rate;
            for(const auto& e:detector->feed(input.data(),input.size(),position)) {
                auto evidence=Json::parse(e.to_jsonl());
                evidence["timestamp"]=time+(double(e.start_sample)-double(position))/rate;
                evidence["continuity_epoch"]=epoch;
                result["o4_evidence"].push_back(std::move(evidence));
            }
            // Bounded overlapping decode windows; O4 sees each original sample once.
            const size_t stride=std::min<size_t>(2000000,std::llround(rate*.02));
            const size_t overlap=std::llround(rate*.002);
            for(size_t offset=0;offset<input.size();offset+=stride) {
                const size_t n=std::min(stride,input.size()-offset),carried=tail.size();
                std::vector<Sample> window=tail;
                window.insert(window.end(),input.begin()+offset,input.begin()+offset+n);
                tail.assign(window.end()-std::min(overlap,window.size()),window.end());
                droneid::dji::DecodeOptions options;options.sample_rate=rate;options.auto_center=true;
                for(const auto& burst:droneid::dji::decode_samples(window,options)) {
                    if(!burst.crc_ok)continue;
                    const int64_t start=int64_t(position+offset)-int64_t(carried)+burst.sample_index;
                    if(start<0)continue;
                    static constexpr char digits[]="0123456789abcdef";std::string hex;
                    for(auto b:burst.payload){hex+=digits[b>>4];hex+=digits[b&15];}
                    // Same payload at a nearby overlapping-window location is one reception.
                    const bool duplicate=std::any_of(recent.begin(),recent.end(),[&](const auto& old){
                        return old.second==hex && std::abs(old.first-start)<int64_t(overlap);});
                    if(duplicate)continue;
                    recent.emplace_back(start,hex);if(recent.size()>256)recent.erase(recent.begin());
                    if(result["native_packets"].size()>=64){result["packet_rows_omitted"]=result["packet_rows_omitted"].get<int>()+1;continue;}
                    auto envelope=droneid::dji::inspect_packet(burst.payload);
                    droneid::dji::Telemetry telemetry;const bool parsed=droneid::dji::parse_frame(burst.payload,telemetry);
                    Json p={{"protocol","DJI DroneID"},{"outer_crc24",true},{"inner_crc16",envelope.crc16_ok},
                        {"envelope",envelope.format},{"length_valid",envelope.length_valid},{"telemetry_parsed",parsed},
                        {"payload_hex",hex},{"sample_index",start},{"sample_index_scope","continuity_epoch_padded_window_start"},
                        {"timestamp",time+(double(start)-double(position))/rate},{"sample_rate_hz",rate},
                        {"center_hz",center},{"correlation",burst.corr},{"cfo_hz",burst.cfo_hz},
                        {"generation","unverified"},{"identity_authenticated",false},
                        {"channel_selection","strongest_10MHz_per_window_not_emitter_identification"}};
                    if(parsed){p["reported_serial"]=telemetry.serial;p["reported_model"]=telemetry.type_name;
                        p["sequence"]=telemetry.sequence;p["reported_drone_lat"]=telemetry.drone_lat;
                        p["reported_drone_lon"]=telemetry.drone_lon;}
                    result["native_packets"].push_back(std::move(p));
                }
            }
            position+=count;
        }catch(const std::exception& e){result["error"]=e.what();initialized=false;tail.clear();recent.clear();}
        result["processing_ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        std::cout<<result.dump()<<std::endl;
    }
}
