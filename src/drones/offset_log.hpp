#pragma once

// One line per carrier-offset measurement (data/drone_offsets.jsonl), for checking whether the
// offsets are stable enough to tell drones apart. Measurement only: nothing reads it back.
//   kind "droneid": a new DroneID packet's offset from its documented channel (Hz and ppm).
//   kind "link":    a capture's link-grid position modulo the subcarrier spacing, and its link
//                   clock (crystal ppm from symbol timing) when measured.

#include "evidence_claims.hpp"
#include <deque>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>

namespace rfmon::drone_claims {
class OffsetLog {
public:
    explicit OffsetLog(std::filesystem::path path, std::uintmax_t max_bytes = 64ull << 20)
        : path_(std::move(path)), max_bytes_(max_bytes) {}

    void packet(const Key& key, const Packet& p) {
        Json line{{"kind", "droneid"}, {"run", key.run}, {"session", key.session}, {"capture", key.capture},
            {"center_hz", key.center}, {"serial", p.serial}, {"model", p.model}, {"readable", p.readable},
            {"encrypted", p.encrypted}, {"observed_steady_s", seconds(p.observed)},
            {"offset_hz", number(p.offset_hz)}, {"channel_hz", p.channel_hz > 0 ? Json(p.channel_hz) : Json(nullptr)},
            {"offset_ppm", number(p.offset_ppm)}};
        write(std::move(line));
    }

    // Each capture once (the monitor's history is offered again on every refresh).
    void link(const symbol_timing::Result& r) {
        auto grids = Json::array();
        for (std::size_t i = 0; i < r.grid.size(); ++i)
            if (const auto& g = r.grid[i])
                grids.push_back({{"hypothesis", symbol_timing::hypotheses[i].name}, {"spacing_hz", g->spacing_hz},
                                 {"hz", g->hz}, {"coherence", g->coherence}, {"blocks", g->blocks}});
        if (grids.empty()) return;
        const auto id = std::make_tuple(r.run_id, r.radio_session, r.capture_seq);
        if (!seen_.insert(id).second) return;
        order_.push_back(id);
        if (order_.size() > 512) { seen_.erase(order_.front()); order_.pop_front(); }
        Json line{{"kind", "link"}, {"run", r.run_id}, {"session", r.radio_session}, {"capture", r.capture_seq},
            {"center_hz", r.center}, {"sample_rate_hz", r.rate}, {"blocks", r.blocks},
            {"group", r.timing_group ? Json(r.timing_group) : Json(nullptr)}, {"grid", std::move(grids)},
            {"device_ns", r.device_ns}, {"host_ns", r.host_before_ns}, {"observed_steady_s", seconds(r.observed)}};
        if (const auto& c = r.link_clock)
            line["link_clock"] = {{"link", link_clock::name(c->link)},
                                  {"ppm", c->ppm}, {"sigma_ppm", c->sigma_ppm}, {"blocks", c->blocks},
                                  {"usable", link_clock::usable(*c)}, {"version", link_clock::version}};
        if (const auto& c = r.link_clock; c && c->has_symbol_phase)
            line["link_clock"]["symbol_phase_s"] = c->symbol_phase_s;  // with device_ns: frame timing offline
        write(std::move(line));
    }

    bool failed() const { return failed_; }

private:
    using Json = nlohmann::json;
    static double seconds(Clock::time_point t) { return std::chrono::duration<double>(t.time_since_epoch()).count(); }
    static Json number(const std::optional<double>& v) { return v ? Json(*v) : Json(nullptr); }

    void write(Json data) {
        if (failed_) return;
        data["schema"] = "rfmon.drone_offset.v1";
        data["unix_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto line = data.dump(-1, ' ', false, Json::error_handler_t::replace);
        std::error_code error;
        // Bounded: one previous file is kept (<name>.1).
        if (output_.is_open() && written_ + line.size() + 1 > max_bytes_) {
            output_.close();
            auto old = path_;
            old += ".1";
            std::filesystem::rename(path_, old, error);
            error.clear();
        }
        if (!output_.is_open()) {
            if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path(), error);
            output_.open(path_, std::ios::app);
            if (!output_) { failed_ = true; return; }
            const auto size = std::filesystem::file_size(path_, error);
            written_ = error ? 0 : size;
        }
        output_ << line << '\n';
        output_.flush();
        written_ += line.size() + 1;
        failed_ = !output_;
    }

    std::filesystem::path path_;
    std::uintmax_t max_bytes_;
    std::ofstream output_;
    std::uintmax_t written_{};
    bool failed_{};
    using CaptureId = std::tuple<std::string, uint64_t, uint64_t>;
    std::set<CaptureId> seen_;
    std::deque<CaptureId> order_;
};
}
