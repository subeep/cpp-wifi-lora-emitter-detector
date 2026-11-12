#pragma once

#include "drone_tracks.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace rfmon::drone_claims {
inline nlohmann::json track_json(const DroneTrack& track) {
    using Json = nlohmann::json;
    const auto seconds = [](Clock::duration time) { return std::chrono::duration<double>(time).count(); };
    Json result{{"id", track.id}, {"key", track.key}, {"identity", track.identity},
        {"first_seen_steady_s", seconds(track.first_seen.time_since_epoch())},
        {"last_seen_steady_s", seconds(track.last_seen.time_since_epoch())},
        {"time_present_s", seconds(track.time_present)}, {"appearances", track.appearances},
        {"state", track.present ? "present" : "gone"}, {"bands", track.bands}, {"evidence", track.evidence},
        {"timing_group", track.group}, {"link_ranges_hz", track.link_ranges}, {"serial", track.serial},
        {"remote_ids", track.remote_ids}, {"identity_self_reported", !track.serial.empty() || !track.remote_ids.empty()},
        {"encrypted_seen", track.encrypted}, {"conflict_seen", track.conflict},
        {"verdict", {{"tier", tier_name(track.best.tier)}, {"model", track.best.model},
                     {"basis", track.best.basis}, {"detail", track.best.detail}}}};
    const auto add = [&](const char* name, const std::optional<ReportedPosition>& position) {
        if (!position) return;
        const auto& p = *position;
        Json fix{{"latitude", p.latitude}, {"longitude", p.longitude},
            {"observed_steady_s", seconds(p.observed.time_since_epoch())},
            {"source", p.source}, {"self_reported", true}, {"authenticated", false}};
        if (p.height_m) fix["height_m"] = *p.height_m;
        if (p.altitude_m) fix["altitude_m"] = *p.altitude_m;
        if (p.speed_m_s) fix["speed_m_s"] = *p.speed_m_s;
        result["positions"][name] = std::move(fix);
    };
    add("aircraft", track.positions.aircraft);
    add("pilot", track.positions.pilot);
    add("home", track.positions.home);
    add("operator", track.positions.operator_location);
    return result;
}

class DroneTrackLog {
public:
    explicit DroneTrackLog(std::filesystem::path path, std::uintmax_t max_bytes = 64ull << 20)
        : path_(std::move(path)), max_bytes_(max_bytes),
          run_(std::chrono::system_clock::now().time_since_epoch().count()) {}

    void record(const std::vector<TrackEvent>& events) {
        for (const auto& event : events) {
            if (failed_ || !open()) return;
            const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            nlohmann::json data{{"schema", "rfmon.drone_track.v1"}, {"event", event.event},
                {"run", run_}, {"unix_ms", unix_ms}, {"track", track_json(event.track)}};
            if (!event.alert.empty()) {
                data["alert"] = event.alert;
                data["severity"] = event.lower_severity ? "low" : "normal";
            }
            if (event.merged_into) data["merged_into"] = event.merged_into;
            const auto line = data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            if (written_ && written_ + line.size() + 1 > max_bytes_) {
                output_.close();
                auto previous = path_;
                previous += ".1";
                std::error_code error;
                std::filesystem::rename(path_, previous, error);
                if (error) { failed_ = true; return; }
                if (!open()) return;
            }
            output_ << line << '\n';
            output_.flush();
            written_ += line.size() + 1;
            failed_ = !output_;
        }
    }

    bool failed() const { return failed_; }

private:
    bool open() {
        if (output_.is_open()) return true;
        std::error_code error;
        if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path(), error);
        if (!error) output_.open(path_, std::ios::app);
        if (!output_) { failed_ = true; return false; }
        const auto size = std::filesystem::file_size(path_, error);
        written_ = error ? 0 : size;
        return true;
    }

    std::filesystem::path path_;
    std::uintmax_t max_bytes_, written_{};
    std::chrono::system_clock::rep run_;
    std::ofstream output_;
    bool failed_{};
};
}
