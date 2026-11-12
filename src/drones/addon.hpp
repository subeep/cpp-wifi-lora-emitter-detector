#pragma once
#include "detector.hpp"
#include "remote_id.hpp"
#include "security/wifi_frame_event.hpp"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <functional>
#include <sstream>

namespace drone {
// Optional observer only: never owns, configures or waits for the radio.
class Addon {
public:
    struct ClassifierState { bool enabled=false, ready=false; std::string detail="Classifier connection unavailable"; };
    // Hooks share the existing inference worker; no second model/radio process.
    std::function<bool()> start_classifier; // true only if this service started it
    std::function<void()> stop_classifier;
    std::function<ClassifierState()> classifier_state;
    bool unified=false;
    Detector live, replay, native;
    std::atomic<uint64_t> native_offered{0}, native_omitted{0}, native_unhealthy{0}, native_max_observer_ns{0};
    // Contiguous input offered to the native backend per capture, as
    // consecutive <=160 ms messages (the message bound is separate from the
    // observed duration). 0.160 keeps the band modes' single-prefix
    // behaviour; coordinated mode raises it to cover each whole visit. The
    // backend's own timestamp check turns any message it drops into a
    // discontinuity, so later chunks never bridge a gap.
    std::atomic<double> native_max_capture_s{0.160};
    std::atomic<uint64_t> native_chunks{0};
    // True for a capture made by a DroneID follow-up visit (set by the wiring, before scanning
    // starts). Such a capture is offered whole and ahead of waiting ordinary captures.
    std::function<bool(uint64_t capture_seq)> native_priority;
    void start_unified() {
        if(unified)return;
        native_offered=0;native_omitted=0;native_unhealthy=0;native_max_observer_ns=0;
        owned_classifier_=start_classifier ? start_classifier() : false;
        owned_live_=!live.running();owned_rid_=!remote_id.enabled();
        if(owned_live_){offered=0;omitted=0;unhealthy=0;live.start();}
        if(owned_rid_)remote_id.enable(true);
        native.start(true,true);unified=true;
    }
    void stop_unified() {
        native.stop();
        if(owned_live_)live.stop();
        if(owned_rid_)remote_id.enable(false);
        if(owned_classifier_&&stop_classifier)stop_classifier();
        owned_classifier_=owned_live_=owned_rid_=false;unified=false;
    }
    RemoteIdMonitor remote_id;
    std::atomic<uint64_t> offered{0}, omitted{0}, unhealthy{0};
    std::atomic<bool> replay_busy{false};
    std::atomic<bool> replay_native{false}; // backend of the latest offline load
    ~Addon() { cancel_=true; if(loader_.joinable()) loader_.join(); replay.stop(); live.stop(); native.stop(); }
    void observe(const std::vector<std::complex<float>>& iq,
                 const rfmon::wifi_security::CaptureRecord& c) noexcept {
        if(native.running()) {
            const auto observer_start=std::chrono::steady_clock::now();
            try {
                native_offered+=iq.size();
                if(!c.contiguous() || c.samples_received!=iq.size() || c.clock!=rfmon::wifi_security::ClockDomain::UsrpDevice ||
                   !std::isfinite(c.sample_rate_hz)||c.sample_rate_hz<10e6||c.sample_rate_hz>250e6) {
                    ++native_unhealthy;native_omitted+=iq.size();++native_epoch_;
                } else {
                    std::ostringstream key;key.precision(17);
                    key<<c.run_id<<'/'<<c.radio_session<<'/'<<c.source<<'/'<<c.band<<'/'<<c.device<<'/'<<c.antenna
                       <<'/'<<c.actual_rf_hz<<'/'<<c.actual_dsp_hz<<'/'<<c.capture_center_hz<<'/'<<c.sample_rate_hz;
                    if(c.gain_db)key<<"/gain="<<*c.gain_db;
                    if(c.retuned||key.str()!=native_source_){native_source_=key.str();++native_epoch_;}
                    // Consecutive contiguous messages from the capture start; retain O4 history
                    // only over actual contiguous samples.
                    const size_t chunk=std::min<size_t>(40000000,size_t(c.sample_rate_hz*.160));
                    const bool priority=native_priority&&native_priority(c.capture_seq);
                    const double limit_s=priority?3600.:std::clamp(native_max_capture_s.load(),.160,3600.);
                    const size_t total=std::min<size_t>(iq.size(),size_t(c.sample_rate_hz*limit_s));
                    native_omitted+=iq.size()-total;
                    const double center=c.stream_center_hz();
                    for(size_t offset=0;chunk&&offset<total;offset+=chunk) {
                        const size_t n=std::min(chunk,total-offset);
                        ++native_chunks;
                        native.submit(iq.data()+offset,n,c.sample_rate_hz,center,c.device_time_ns*1e-9+offset/c.sample_rate_hz,native_epoch_,
                            "LIVE / run "+c.run_id+" / session "+std::to_string(c.radio_session)+
                            " / capture "+std::to_string(c.capture_seq)+" / sample "+std::to_string(offset),priority);
                    }
                }
            } catch(...) { ++native_unhealthy;++native_epoch_; }
            const uint64_t elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()-observer_start).count();
            auto previous=native_max_observer_ns.load();
            while(previous<elapsed&&!native_max_observer_ns.compare_exchange_weak(previous,elapsed)){}
        }
        if(!live.running()) return;
        try {
            offered+=iq.size();
            if(!c.contiguous() || c.clock!=rfmon::wifi_security::ClockDomain::UsrpDevice ||
               !std::isfinite(c.sample_rate_hz) || c.sample_rate_hz<15.26e6 || c.sample_rate_hz>100e6) {
                ++unhealthy; omitted+=iq.size(); return;
            }
            const size_t n=std::min<size_t>(iq.size(),std::min<size_t>(2000000,size_t(c.sample_rate_hz*.02)));
            if(!n) return;
            const size_t windows=std::min<size_t>(4,iq.size()/n);
            omitted+=iq.size()-windows*n;
            for(size_t i=0;i<windows;++i) {
                const size_t offset=windows==1?0:i*(iq.size()-n)/(windows-1);
                const double center=c.stream_center_hz();
                live.submit(iq.data()+offset,n,c.sample_rate_hz,center,
                    c.device_time_ns*1e-9+offset/c.sample_rate_hz,++epoch_,
                    "LIVE / run "+c.run_id+" / session "+std::to_string(c.radio_session)+
                    " / capture "+std::to_string(c.capture_seq)+" / sample "+std::to_string(offset));
            }
        } catch(...) { ++unhealthy; } // An optional observer cannot terminate scanning.
    }
    std::string replay_status() { std::lock_guard<std::mutex> l(message_mutex_); return message_; }
    // native=false: legacy decoder, at most 2M samples (40 ms cap).
    // native=true: native packets + O4 evidence, at most 160 ms / 40M samples.
    void load(const std::string& path,double rate,double frequency,double offset,bool native=false) {
        if(replay_busy.exchange(true)) return;
        if(loader_.joinable()) loader_.join();
        cancel_=false;
        replay_native=native;
        loader_=std::thread([this,path,rate,frequency,offset,native] {
            try {
                const double min_rate=native?10e6:15.26e6, max_rate=native?250e6:100e6;
                if(!std::isfinite(rate)||rate<min_rate||rate>max_rate||!std::isfinite(frequency)||
                   !((frequency>2.3e9&&frequency<2.6e9)||(frequency>5.6e9&&frequency<6e9))||
                   !std::isfinite(offset)||offset<0||offset>86400) throw std::runtime_error("Invalid rate, band or offset");
                if(!std::filesystem::is_regular_file(path)) throw std::runtime_error("Select a regular raw little-endian complex-float32 IQ file");
                const auto bytes=std::filesystem::file_size(path);
                if(bytes%8) throw std::runtime_error("IQ file length is not a whole complex-float32 sample");
                const uint64_t first=uint64_t(offset*rate);
                if(first>=bytes/8) throw std::runtime_error("Offset is beyond the recording");
                const size_t count=native?std::min<uint64_t>({bytes/8-first,40000000,uint64_t(rate*.16)})
                                         :std::min<uint64_t>({bytes/8-first,2000000,uint64_t(rate*.04)});
                std::vector<std::complex<float>> data(count);
                std::ifstream file(path,std::ios::binary); file.seekg(first*8);
                if(!file.read(reinterpret_cast<char*>(data.data()),count*8)) throw std::runtime_error("Cannot read IQ interval");
                if(cancel_) throw std::runtime_error("Cancelled");
                replay.start(native);
                // Native analysis of 160 ms runs well slower than real time.
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(native?120:40);
                while(!cancel_ && replay.running() && !replay.snapshot().ready && std::chrono::steady_clock::now()<deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if(cancel_ || !replay.snapshot().ready) throw std::runtime_error("Replay decoder unavailable: "+replay.snapshot().status);
                replay.submit(data.data(),count,rate,frequency,first/rate,1,"OFFLINE / "+path+" / sample "+std::to_string(first));
                while(!cancel_ && replay.running() && replay.snapshot().samples<count && std::chrono::steady_clock::now()<deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                const auto result=replay.snapshot();
                replay.stop();
                if(result.samples<count || result.analysis_errors) throw std::runtime_error("Replay incomplete: "+result.status);
                message(std::string(native?"Native: completed ":"Completed ")+std::to_string(count/rate*1000)+" ms at offset "+std::to_string(first/rate)+" s. Offline results only.");
            } catch(const std::exception& e) { replay.stop(); message(e.what()); }
            replay_busy=false;
        });
    }
private:
    bool owned_classifier_=false,owned_live_=false,owned_rid_=false;
    uint64_t native_epoch_=0;
    std::string native_source_;
    std::atomic<bool> cancel_{false};
    uint64_t epoch_=0; // single scanner processing lane
    std::thread loader_;
    std::mutex message_mutex_;
    std::string message_="No offline interval loaded";
    void message(const std::string& s) { std::lock_guard<std::mutex> l(message_mutex_); message_=s; }
};
}
