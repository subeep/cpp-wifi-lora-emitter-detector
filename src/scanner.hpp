// Owns the B210 and a background scan thread. Continuously scans
// whichever band is currently "active" (as set from the UI thread),
// keeping one persistent DeviceRegistry per band so switching modes and
// back doesn't lose an accumulated list - only the active band's
// registry is updated while it's selected; the other two stay frozen.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "registry.hpp"
#include "sdr_capture.hpp"

namespace rfmon {

struct ScannerStatus {
    bool connected = false;
    std::string error;              // non-empty if SDR init failed
    std::string active_band;
    std::string active_step_label;  // which scan step is in flight right now
    int cycle_count = 0;            // cycles completed for the active band since it became active
    bool last_overflow = false;
};

class Scanner {
public:
    Scanner();
    ~Scanner();

    void start();  // spawns the background thread and connects to the SDR
    void stop();   // signals the thread to stop and joins it

    void set_active_band(const std::string& band);
    std::string active_band() const;

    void set_threshold_db(double db);
    double threshold_db() const;

    void set_gain(std::optional<double> gain_db);  // nullopt = AGC
    std::optional<double> gain() const;

    std::vector<DeviceRow> snapshot(const std::string& band) const;
    ScannerStatus status() const;

private:
    void run();
    DeviceRegistry& registry_for(const std::string& band);

    std::unique_ptr<B210Capture> sdr_;
    std::thread thread_;
    std::atomic<bool> stop_flag_{false};

    mutable std::mutex config_mutex_;
    std::string active_band_ = BAND_WIFI_2G4;
    double threshold_db_ = DEFAULT_DETECTION_THRESHOLD_DB;
    std::optional<double> gain_db_ = DEFAULT_GAIN_DB;
    bool gain_dirty_ = false;

    mutable std::mutex status_mutex_;
    ScannerStatus status_;

    DeviceRegistry registry_lora_;
    DeviceRegistry registry_wifi24_;
    DeviceRegistry registry_wifi5_;
};

}  // namespace rfmon
