#pragma once
#include "security/wifi_frame_event.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace drone {
using RidJson=nlohmann::json;
// Complete MPDU including FCS; independently verified before any parsing.
RidJson parse_wifi_remote_id(const std::vector<uint8_t>& mpdu);
class RemoteIdMonitor {
public:
    RemoteIdMonitor()=default;
    ~RemoteIdMonitor();
    void enable(bool value);
    bool enabled() const { return enabled_; }
    void submit(const rfmon::wifi_security::FrameEvent& frame) noexcept;
    RidJson snapshot();
private:
    struct Message { RidJson value; std::chrono::steady_clock::time_point received; double initial_age=0; };
    struct Track { RidJson info; std::map<std::string,Message> messages; std::chrono::steady_clock::time_point seen; int64_t host_ns=0; };
    void run();
    void consume(const rfmon::wifi_security::FrameEvent& frame);
    std::atomic<bool> enabled_{false};
    std::atomic<uint64_t> dropped_{0};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::deque<rfmon::wifi_security::FrameEvent> queue_;
    std::map<std::string,Track> tracks_;
    uint64_t examined_=0,accepted_=0,rejected_=0,errors_=0,out_of_order_=0;
};
}
