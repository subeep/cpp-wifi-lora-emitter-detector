// Receive-only Linux worker. No UHD/radio linkage. Raw IQ in, bounded evidence out.
#include "droneid/dji.h"
#include "droneid/o4_detector.h"
#include "drones/droneid_packet.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/resource.h>

using Json=nlohmann::json;
using Sample=droneid::cf32;
static_assert(sizeof(Sample)==8,"cf32 ABI required");

namespace {
// Runs work(0..count-1) on a fixed set of threads plus the caller. The decode windows of one
// message are independent of each other, so they are decoded side by side and their results
// are merged afterwards in the original order: the output is the same as decoding in sequence.
// Threads are kept between messages so each keeps its FFT plans.
class Pool {
public:
    explicit Pool(unsigned helpers) {
        for(unsigned i=0;i<helpers;++i)threads_.emplace_back([this]{
            std::unique_lock<std::mutex> lock(mutex_);
            while(true) {
                wake_.wait(lock,[this]{return stop_||next_<count_;});
                if(stop_)return;
                take(lock);
            }});
    }
    ~Pool() {
        {std::lock_guard<std::mutex> lock(mutex_);stop_=true;}
        wake_.notify_all();
        for(auto& t:threads_)t.join();
    }
    // `work` must not throw.
    void run(size_t count,const std::function<void(size_t)>& work) {
        std::unique_lock<std::mutex> lock(mutex_);
        work_=&work;count_=count;next_=0;pending_=count;
        wake_.notify_all();
        while(next_<count_)take(lock);
        done_.wait(lock,[this]{return pending_==0;});
        count_=0;
    }
private:
    void take(std::unique_lock<std::mutex>& lock) {
        const size_t index=next_++;
        lock.unlock();
        (*work_)(index);
        lock.lock();
        if(--pending_==0)done_.notify_all();
    }
    std::mutex mutex_;
    std::condition_variable wake_,done_;
    std::vector<std::thread> threads_;
    const std::function<void(size_t)>* work_=nullptr;
    size_t count_=0,next_=0,pending_=0;
    bool stop_=false;
};
}

int main(int argc,char** argv) {
    // --listed-channels-only: live captures have an exact centre frequency, so the listed
    // channels are enough there. --threads N: decode threads (1 = the old sequential path).
    bool listed_only=false;
    // Default: one 160 ms message at 20 MSps is 8 windows plus the O4 job, so about ten threads
    // take it in one round; more than 12 gains nothing there. Some cores are left to the scanner.
    unsigned threads=std::clamp(std::thread::hardware_concurrency()*3/5,1u,12u);
    for(int i=1;i<argc;++i) {
        if(!std::strcmp(argv[i],"--listed-channels-only"))listed_only=true;
        else if(!std::strcmp(argv[i],"--threads")&&i+1<argc)threads=std::clamp(std::atoi(argv[++i]),1,64);
        else return 2;
    }
    Pool pool(threads-1);
    // Independent process: failures/resources cannot take down scanner or legacy decoder.
    rlimit memory{size_t(3)<<30,size_t(3)<<30};setrlimit(RLIMIT_AS,&memory);
    // Decoding gives way to the radio and scanner threads when cores are short.
    setpriority(PRIO_PROCESS,0,10);
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
            // Bounded overlapping decode windows; O4 sees each original sample once.
            const size_t stride=std::min<size_t>(2000000,std::llround(rate*.02));
            const size_t overlap=std::llround(rate*.002);
            // Tune to each documented DroneID channel that lies inside the capture: a strong
            // unrelated signal otherwise wins the decoder's own strongest-10-MHz choice. That
            // automatic choice still runs last, because capture-centre metadata can be wrong and
            // the 5.8 GHz list is incomplete. A payload found by both is reported once.
            std::vector<double> channels;
            for(double channel:drone::droneid::channels_24)
                if(std::abs(channel-center)<=rate*.45-5e6)channels.push_back(channel);
            for(double channel:drone::droneid::channels_58)
                if(std::abs(channel-center)<=rate*.45-5e6)channels.push_back(channel);
            const size_t passes=channels.size()+(listed_only&&!channels.empty()?0:1);
            struct Window { std::vector<Sample> samples; size_t offset,carried; };
            std::vector<Window> windows;
            for(size_t offset=0;offset<input.size();offset+=stride) {
                const size_t n=std::min(stride,input.size()-offset),carried=tail.size();
                std::vector<Sample> window=tail;
                window.insert(window.end(),input.begin()+offset,input.begin()+offset+n);
                tail.assign(window.end()-std::min(overlap,window.size()),window.end());
                windows.push_back({std::move(window),offset,carried});
            }
            // Job 0 is the O4 detector (it keeps state, so it stays one job); the rest are one
            // decode per window and channel.
            std::vector<droneid::O4Evidence> o4;
            std::vector<std::vector<droneid::dji::DecodedBurst>> decoded(windows.size()*passes);
            std::vector<std::exception_ptr> failed(1+decoded.size());
            pool.run(failed.size(),[&](size_t job) {
                try {
                    if(!job){o4=detector->feed(input.data(),input.size(),position);return;}
                    const size_t pass=(job-1)%passes;
                    droneid::dji::DecodeOptions options;options.sample_rate=rate;
                    if(pass<channels.size())options.freq_offset=center-channels[pass]; // shift that channel to baseband
                    else options.auto_center=true;
                    decoded[job-1]=droneid::dji::decode_samples(windows[(job-1)/passes].samples,options);
                }catch(...){failed[job]=std::current_exception();}
            });
            if(failed[0])std::rethrow_exception(failed[0]);
            for(const auto& e:o4) {
                auto evidence=Json::parse(e.to_jsonl());
                evidence["timestamp"]=time+(double(e.start_sample)-double(position))/rate;
                evidence["continuity_epoch"]=epoch;
                result["o4_evidence"].push_back(std::move(evidence));
            }
            for(size_t w=0;w<windows.size();++w) {
                const size_t offset=windows[w].offset,carried=windows[w].carried;
                for(size_t pass=0;pass<passes;++pass) {
                if(failed[1+w*passes+pass])std::rethrow_exception(failed[1+w*passes+pass]);
                const bool documented=pass<channels.size();
                for(const auto& burst:decoded[w*passes+pass]) {
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
                        {"channel_selection",documented?"documented_droneid_channel"
                                                        :"strongest_10MHz_per_window_not_emitter_identification"}};
                    if(documented)p["channel_hz"]=channels[pass];
                    // Contract fields (drone::droneid::contract_version): what the payload says, not who sent it.
                    const auto info=drone::droneid::classify(burst.payload,envelope.format);
                    p["contract"]=drone::droneid::contract_version;p["envelope"]=info.format;
                    p["payload_version"]=info.payload_version;p["identity_readable"]=info.identity_readable;
                    p["identity_encrypted"]=info.identity_encrypted;
                    p["product_code"]=info.product_code;p["model"]=info.model;p["model_source"]=info.model_source;
                    p["payload_shift"]=info.payload_shift;
                    if(parsed){p["reported_serial"]=telemetry.serial;
                        p["reported_model"]=info.model.empty()?telemetry.type_name:info.model;
                        p["sequence"]=telemetry.sequence;p["reported_drone_lat"]=telemetry.drone_lat;
                        p["reported_drone_lon"]=telemetry.drone_lon;}
                    // Version 3 and padded records: the vendored parser declines them, but the
                    // serial and product code are plain text at the version-2 offsets.
                    else if(info.identity_readable){p["reported_serial"]=info.serial;p["reported_model"]=info.model;}
                    result["native_packets"].push_back(std::move(p));
                }
                }
            }
            position+=count;
        }catch(const std::exception& e){result["error"]=e.what();initialized=false;tail.clear();recent.clear();}
        result["processing_ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        std::cout<<result.dump()<<std::endl;
    }
}
