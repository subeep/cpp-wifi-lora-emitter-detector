#include "scanner.hpp"

#include <chrono>

#include "classifier.hpp"
#include "spectrum.hpp"

namespace rfmon {

namespace {
double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

Scanner::Scanner() = default;

Scanner::~Scanner() { stop(); }

void Scanner::start() {
    stop_flag_ = false;
    thread_ = std::thread(&Scanner::run, this);
}

void Scanner::stop() {
    stop_flag_ = true;
    if (thread_.joinable()) thread_.join();
}

void Scanner::set_active_band(const std::string& band) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    active_band_ = band;
}

std::string Scanner::active_band() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return active_band_;
}

void Scanner::set_threshold_db(double db) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    threshold_db_ = db;
}

double Scanner::threshold_db() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return threshold_db_;
}

void Scanner::set_gain(std::optional<double> gain_db) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    gain_db_ = gain_db;
    gain_dirty_ = true;
}

std::optional<double> Scanner::gain() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return gain_db_;
}

std::vector<DeviceRow> Scanner::snapshot(const std::string& band) const {
    // registry_for() is non-const (map-lookup by reference); the
    // registries themselves are separately mutex-protected, so a
    // const_cast here is safe - it never mutates Scanner's own state.
    return const_cast<Scanner*>(this)->registry_for(band).snapshot(now_seconds());
}

ScannerStatus Scanner::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

DeviceRegistry& Scanner::registry_for(const std::string& band) {
    if (band == BAND_SUB_GHZ) return registry_lora_;
    if (band == BAND_WIFI_2G4) return registry_wifi24_;
    return registry_wifi5_;
}

void Scanner::run() {
    try {
        std::optional<double> initial_gain;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            initial_gain = gain_db_;
            gain_dirty_ = false;
        }
        sdr_ = std::make_unique<B210Capture>(ANTENNA, initial_gain, 0, DEVICE_ARGS);
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.connected = true;
        status_.error.clear();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.connected = false;
        status_.error = e.what();
        return;
    }

    std::string last_band_seen;
    int band_cycle_count = 0;

    while (!stop_flag_.load()) {
        std::string band;
        double threshold;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            band = active_band_;
            threshold = threshold_db_;
            if (gain_dirty_) {
                sdr_->set_gain(gain_db_);
                gain_dirty_ = false;
            }
        }
        if (band != last_band_seen) {
            band_cycle_count = 0;
            last_band_seen = band;
        }

        std::vector<ScanStep> plan = scan_plan_for_band(band);
        std::vector<Detection> detections;
        bool any_overflow = false;

        for (const auto& step : plan) {
            if (stop_flag_.load()) break;
            {
                // Mode changed mid-cycle: abandon the rest of this
                // cycle's steps and let the outer loop pick up the new
                // band fresh next iteration.
                std::lock_guard<std::mutex> lock(config_mutex_);
                if (active_band_ != band) break;
            }

            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.active_band = band;
                status_.active_step_label = step.label;
            }

            std::vector<std::vector<std::complex<float>>> captures;
            double actual_rate = step.sample_rate_hz;
            for (int i = 0; i < SUB_CAPTURES_PER_STEP; ++i) {
                auto [iq, rate, overflow] =
                    sdr_->capture(step.center_hz, step.sample_rate_hz, SUB_CAPTURE_DURATION_S);
                actual_rate = rate;
                any_overflow = any_overflow || overflow;
                if (!iq.empty()) captures.push_back(std::move(iq));
            }
            if (captures.empty()) continue;

            Spectrum spec = max_hold_spectrum(captures, actual_rate);
            double guard_hz = std::max(actual_rate * DC_GUARD_FRACTION, DC_GUARD_MIN_HZ);
            std::vector<bool> dc_mask = mask_dc_guard(spec.freqs_offset_hz, guard_hz);
            std::vector<bool> edge_mask =
                mask_edge_guard(spec.freqs_offset_hz, actual_rate, EDGE_GUARD_FRACTION);

            std::vector<Segment> segments =
                find_segments(spec.freqs_offset_hz, spec.psd_db, step.center_hz, edge_mask,
                              dc_mask, NOISE_FLOOR_PERCENTILE, threshold, MIN_SEGMENT_BINS,
                              MERGE_GAP_BINS);
            for (const auto& seg : segments) {
                detections.push_back(Detection{step.band, seg, classify(step.band, seg)});
            }
        }

        DeviceRegistry& reg = registry_for(band);
        double now = now_seconds();
        reg.update_cycle(detections, now);
        reg.end_cycle();
        ++band_cycle_count;

        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.cycle_count = band_cycle_count;
            status_.last_overflow = any_overflow;
        }
    }

    sdr_.reset();
}

}  // namespace rfmon
