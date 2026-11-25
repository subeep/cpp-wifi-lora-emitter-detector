#pragma once
// Training recorder: saves receiver captures of drones whose model a readable DJI DroneID packet
// names, labelled with that model, so the universal classifier can be retrained on this
// receiver. The label is self-reported by the aircraft (not authenticated).
//
// Which captures: for every row whose readable DroneID names a model, the captures in which that
// drone's video link was measured ("link") and the capture the DroneID packet was decoded from
// ("droneid"). A capture that two different models claim is skipped (ambiguous). Only captures
// still held in the scanner's IQ retention can be saved; the app raises the retention while
// recording, and a capture evicted before it is offered is counted as missed.
//
// Layout (as ~/Documents/air3s_iq_recording/background_rotate.py, so drone_session_replay and the
// training tools read it): <parent>/labelled-<stamp>/session.json and one .sc16 per capture
// (interleaved little-endian int16 I/Q, full scale 32768). session.json is rewritten after every
// file. Writing happens on its own thread; the GUI thread only selects and queues.
#include "evidence_claims.hpp"
#include "../security/wifi_frame_event.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace rfmon::drone_claims {

struct LabelledCapture {
    Key key;
    std::string model, serial, row_id, group;
    std::string evidence;  // "link": the drone's video link was measured in it; "droneid": its packet came from it
    double link_low_hz{}, link_high_hz{};
};

// Captures to save for these rows (pure; no IQ). `ambiguous` counts captures two models claim.
inline std::vector<LabelledCapture> labelled_captures(const std::map<std::string, Detection>& rows,
                                                      std::size_t* ambiguous = nullptr) {
    std::map<std::pair<std::string, uint64_t>, LabelledCapture> chosen;
    std::set<std::pair<std::string, uint64_t>> clash;
    const auto offer = [&](const Detection& row, const Key& key, const char* evidence) {
        if (key.run.empty() || !key.capture) return;
        LabelledCapture c{key, row.packet->model, row.packet->serial, row.id, row.group, evidence,
                          row.link_low_hz, row.link_high_hz};
        const auto id = std::make_pair(key.run, key.capture);
        auto it = chosen.find(id);
        if (it == chosen.end()) { if (!clash.count(id)) chosen.emplace(id, std::move(c)); return; }
        if (it->second.model != c.model) { clash.insert(id); chosen.erase(it); return; }
        if (c.evidence == std::string("link")) it->second = std::move(c);  // link evidence wins
    };
    for (const auto& [id, row] : rows) {
        if (!row.packet || !row.packet->readable || row.packet->model.empty()) continue;
        for (const auto& key : row.link_captures) offer(row, key, "link");
        offer(row, row.packet_key, "droneid");
    }
    if (ambiguous) *ambiguous = clash.size();
    std::vector<LabelledCapture> out;
    for (auto& [id, c] : chosen) out.push_back(std::move(c));
    return out;
}

struct LabelledRecorderLimits {
    uintmax_t min_free_bytes = uintmax_t(20) << 30;  // stop before the disk is this full
    uintmax_t max_session_bytes = uintmax_t(60) << 30;
    std::size_t max_pending = 6;                      // captures queued for writing
};

class LabelledRecorder {
public:
    struct Retained {
        wifi_security::CaptureRecord record;
        std::shared_ptr<const std::vector<std::complex<float>>> iq;
    };
    using Fetch = std::function<std::optional<Retained>(uint64_t capture_seq)>;
    using Limits = LabelledRecorderLimits;
    struct Status {
        bool recording = false;
        std::string dir, error;
        std::size_t saved = 0, pending = 0, dropped = 0, missed = 0, ambiguous = 0, skipped_rate = 0;
        uintmax_t bytes = 0, free_bytes = 0;
        std::map<std::string, std::size_t> per_model;
    };

    LabelledRecorder() = default;
    explicit LabelledRecorder(Limits limits) : limits_(limits) {}
    ~LabelledRecorder() { stop(); }
    LabelledRecorder(const LabelledRecorder&) = delete;
    LabelledRecorder& operator=(const LabelledRecorder&) = delete;

    // Creates <parent>/labelled-<local time stamp>/ and starts the writer. False with `error` set.
    bool start(const std::filesystem::path& parent) {
        stop();
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto t = std::time(nullptr);
        std::tm parts{};
        char stamp[32] = "now";
        if (localtime_r(&t, &parts)) std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &parts);
        fs::path dir = parent / (std::string("labelled-") + stamp);
        for (int n = 2; fs::exists(dir, ec); ++n) dir = parent / (std::string("labelled-") + stamp + "-" + std::to_string(n));
        fs::create_directories(dir, ec);
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = Status{};
        if (ec) { status_.error = "Could not create " + dir.string() + ": " + ec.message(); return false; }
        const auto space = fs::space(dir, ec);
        status_.free_bytes = ec ? 0 : space.available;
        if (!ec && space.available < limits_.min_free_bytes) {
            status_.error = "Not enough free space in " + parent.string();
            return false;
        }
        status_.dir = dir.string();
        status_.recording = true;
        session_ = {{"format", "rfmon.labelled_session.v1"},
                    {"label_source", "DJI DroneID model and serial (self-reported, not authenticated)"},
                    {"created", iso_time(std::chrono::system_clock::now())},
                    {"rate_hz", nullptr},
                    {"captures", nlohmann::json::array()}};
        write_session_locked();
        seen_.clear();
        stopping_ = false;
        writer_ = std::thread([this] { run(); });
        return true;
    }

    // Finishes the queued writes, then stops.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!writer_.joinable()) { status_.recording = false; return; }
            stopping_ = true;
        }
        wake_.notify_all();
        writer_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        status_.recording = false;
        status_.pending = 0;
    }

    bool recording() const { std::lock_guard<std::mutex> lock(mutex_); return status_.recording; }
    Status status() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }

    // GUI thread, every refresh: queue the labelled captures not offered before.
    void offer(const std::map<std::string, Detection>& rows, const Fetch& fetch) {
        std::size_t ambiguous = 0;
        const auto wanted = labelled_captures(rows, &ambiguous);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.recording) return;
        status_.ambiguous = std::max(status_.ambiguous, ambiguous);
        for (const auto& c : wanted) {
            const auto id = std::make_pair(c.key.run, c.key.capture);
            if (seen_.count(id)) continue;
            seen_.insert(id);
            auto retained = fetch ? fetch(c.key.capture) : std::nullopt;
            if (!retained || !retained->iq || retained->record.run_id != c.key.run) { ++status_.missed; continue; }
            if (queue_.size() >= limits_.max_pending) { ++status_.dropped; continue; }
            queue_.push_back({c, std::move(*retained)});
            status_.pending = queue_.size();
        }
        wake_.notify_all();
    }

private:
    struct Job { LabelledCapture label; Retained capture; };

    static std::string iso_time(std::chrono::system_clock::time_point t) {
        const auto tt = std::chrono::system_clock::to_time_t(t);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count() % 1000000;
        std::tm parts{};
        char b[64] = "";
        if (gmtime_r(&tt, &parts)) {
            char base[32];
            std::strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &parts);
            std::snprintf(b, sizeof b, "%s.%06lld+00:00", base, static_cast<long long>(us < 0 ? us + 1000000 : us));
        }
        return b;
    }
    static std::string safe(std::string s) {
        for (auto& ch : s) if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
        return s;
    }
    void write_session_locked() {
        namespace fs = std::filesystem;
        const fs::path dir = status_.dir;
        const auto tmp = dir / "session.json.tmp";
        {
            std::ofstream out(tmp);
            out << session_.dump(1) << '\n';
            if (!out) { status_.error = "Could not write session.json"; return; }
        }
        std::error_code ec;
        fs::rename(tmp, dir / "session.json", ec);
        if (ec) status_.error = "Could not write session.json: " + ec.message();
    }

    void run() {
        namespace fs = std::filesystem;
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stopping, nothing left
                job = std::move(queue_.front());
                queue_.pop_front();
                status_.pending = queue_.size();
                if (!status_.recording) continue;
            }
            const auto& r = job.capture.record;
            const auto& iq = *job.capture.iq;
            const double centre = r.actual_rf_hz != 0 ? r.actual_rf_hz - r.actual_dsp_hz : r.capture_center_hz;
            const uintmax_t size = uintmax_t(iq.size()) * 4;
            std::string error;
            std::error_code ec;
            const fs::path dir = status_.dir;  // set before the thread starts, never changed while it runs
            const auto space = fs::space(dir, ec);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!ec) status_.free_bytes = space.available;
                if (!session_["rate_hz"].is_null() && session_["rate_hz"].get<double>() != r.sample_rate_hz) {
                    ++status_.skipped_rate;
                    continue;
                }
                if (!ec && space.available < limits_.min_free_bytes + size) error = "Stopped: the disk is nearly full";
                else if (status_.bytes + size > limits_.max_session_bytes) error = "Stopped: this session reached its size limit";
                if (!error.empty()) { status_.error = error; status_.recording = false; queue_.clear(); status_.pending = 0; continue; }
            }
            char name[160];
            std::snprintf(name, sizeof name, "%06llu_%.1fMHz_%s.sc16", static_cast<unsigned long long>(r.capture_seq),
                          centre / 1e6, safe(job.label.model).c_str());
            {
                std::vector<int16_t> raw(iq.size() * 2);
                const auto q = [](float v) {
                    return static_cast<int16_t>(std::clamp(std::lround(double(v) * 32768.0), -32768L, 32767L));
                };
                for (std::size_t i = 0; i < iq.size(); ++i) { raw[2 * i] = q(iq[i].real()); raw[2 * i + 1] = q(iq[i].imag()); }
                std::ofstream out(dir / name, std::ios::binary);
                out.write(reinterpret_cast<const char*>(raw.data()), std::streamsize(raw.size() * sizeof(int16_t)));
                if (!out) error = std::string("Could not write ") + name;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (!error.empty()) { status_.error = error; continue; }
            if (session_["rate_hz"].is_null()) {
                session_["rate_hz"] = r.sample_rate_hz;
                session_["device"] = r.device;
                session_["antenna"] = r.antenna;
                session_["gain_db"] = r.gain_db ? nlohmann::json(*r.gain_db) : nlohmann::json(nullptr);
                session_["run_id"] = r.run_id;
            }
            const auto host = r.host_before_ns ? std::chrono::system_clock::time_point(
                                                     std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                                         std::chrono::nanoseconds(r.host_before_ns)))
                                               : std::chrono::system_clock::now();
            session_["captures"].push_back({
                {"file", name}, {"status", "ok"}, {"centre_hz", centre}, {"samples", iq.size()},
                {"requested_samples", iq.size()}, {"host_start", iso_time(host)},
                {"model", job.label.model}, {"serial", job.label.serial}, {"evidence", job.label.evidence},
                {"group", job.label.group}, {"link_low_hz", job.label.link_low_hz}, {"link_high_hz", job.label.link_high_hz},
                {"row_id", job.label.row_id}, {"capture_seq", r.capture_seq}, {"radio_session", r.radio_session},
                {"band", r.band}, {"requested_centre_hz", r.capture_center_hz}, {"actual_rf_hz", r.actual_rf_hz},
                {"actual_dsp_hz", r.actual_dsp_hz}, {"sample_rate_hz", r.sample_rate_hz},
                {"gain_db", r.gain_db ? nlohmann::json(*r.gain_db) : nlohmann::json(nullptr)},
                {"device_time_ns", r.device_time_ns}});
            write_session_locked();
            ++status_.saved;
            status_.bytes += size;
            ++status_.per_model[job.label.model];
        }
    }

    Limits limits_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread writer_;
    bool stopping_ = false;
    Status status_;
    nlohmann::json session_;
    std::deque<Job> queue_;
    std::set<std::pair<std::string, uint64_t>> seen_;
};

}  // namespace rfmon::drone_claims
