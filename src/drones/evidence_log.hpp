#pragma once

#include "evidence_claims.hpp"
#include "model_verdict.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace rfmon::drone_claims {
class DetectionLog {
public:
    explicit DetectionLog(std::filesystem::path path) : path_(std::move(path)) {}

    void record(const std::map<std::string, Detection>& rows) {
        std::map<std::string, nlohmann::json> current;
        for (const auto& [id, row] : rows) current.emplace(id, evidence(row));
        for (const auto& [id, previous] : previous_)
            if (!current.count(id)) append("expired", id, previous);
        for (const auto& [id, row] : current) {
            const auto previous = previous_.find(id);
            if (previous == previous_.end()) append("appeared", id, row);
            // A new capture of the same drone is not news: only what the row says is compared.
            else if (stated(previous->second) != stated(row)) append("updated", id, row);
        }
        previous_ = std::move(current);
    }

    bool failed() const { return failed_; }

private:
    using Json = nlohmann::json;

    static double observed(Clock::time_point time) {
        return std::chrono::duration<double>(time.time_since_epoch()).count();
    }

    static Json evidence(const Detection& row) {
        Json result{{"band", row.band}, {"group", row.group}, {"candidates", row.candidates},
                    {"identity_display", display_identity(row)}, {"possible_conflict", row.conflict}};
        const auto answer = verdict(row);
        result["verdict"] = {{"tier", tier_name(answer.tier)}, {"model", answer.model}, {"basis", answer.basis}};
        if (row.timing) {
            const auto& t = *row.timing;
            result["timing"] = {{"run", t.run_id}, {"session", t.radio_session}, {"capture", t.capture_seq},
                {"center_hz", t.center}, {"link_low_hz", row.link_low_hz}, {"link_high_hz", row.link_high_hz}, {"sample_rate_hz", t.rate}, {"blocks", t.blocks},
                {"flagged_fractions", t.fractions}, {"version", t.constants_version},
                {"observed_steady_s", observed(t.observed)}, {"acquisition_host_ns", t.host_before_ns}};
            // Measurement only (not compared by stated()): link-grid position modulo the spacing.
            if (const auto grid = symbol_timing::link_grid(t))
                result["timing"]["grid_offset"] = {{"hz", grid->hz}, {"spacing_hz", grid->spacing_hz},
                                                   {"coherence", grid->coherence}, {"blocks", grid->blocks}};
        }
        if (row.classifier_hint) {
            const auto& hint = *row.classifier_hint;
            result["classifier_hint"] = {{"model", hint.model}, {"votes", hint.votes},
                                         {"voting_link_captures", hint.voting_captures}};
        }
        if (!row.only_classifier_class.empty())
            result["only_classifier_class"] = row.only_classifier_class;
        const auto classifier = [](const UniversalClaim& u) {
            auto models = Json::array(), guesses = Json::array();
            for (const auto& m : u.models) models.push_back({{"model", m.name}, {"score", m.score}});
            for (const auto& m : u.guesses) guesses.push_back({{"model", m.name}, {"score", m.score}});
            const auto number = [](const std::optional<double>& v) { return v ? Json(*v) : Json(nullptr); };
            return Json{{"type", u.type}, {"models", models}, {"accepted", u.accepted},
                {"classifier_only", u.classifier_only}, {"captures", u.captures}, {"typed", u.typed},
                {"named", u.named}, {"disagreements", u.disagreements}, {"accuracy", u.accuracy},
                // Display only: what it ranked without naming, and which limit was missed.
                {"not_named", {{"captures", u.unnamed}, {"below_score_limit", u.below_score},
                               {"beyond_distance_limit", u.beyond_distance}, {"best_guesses", guesses},
                               {"latest_score", number(u.last_score)}, {"score_limit", number(u.score_limit)},
                               {"latest_distance", number(u.last_distance)}, {"distance_limit", number(u.distance_limit)}}}};
        };
        if (row.universal) result["universal_classifier"] = classifier(*row.universal);
        if (row.classifier_check) result["classifier_check"] = classifier(*row.classifier_check);
        if (!row.remote_id.empty()) {
            auto& list = result["remote_id"] = Json::array();
            for (const auto& rid : row.remote_id)
                list.push_back({{"track", rid.track}, {"reported_id", rid.reported_id}, {"ua_type", rid.ua_type},
                                {"serial_id", rid.serial_id}, {"self_reported", true}, {"transport", rid.transport},
                                {"frequency_hz", rid.frequency}, {"observed_steady_s", observed(rid.observed)}});
        }
        if (row.packet) {
            const auto& p = *row.packet;
            result["droneid"] = {{"run", row.packet_key.run}, {"session", row.packet_key.session},
                {"capture", row.packet_key.capture}, {"center_hz", row.packet_key.center},
                {"outer_crc24", true}, {"payload_version", p.version}, {"identity_readable", p.readable},
                {"identity_encrypted", p.encrypted}, {"product_code", p.product_code}, {"model", p.model},
                {"serial", p.serial}, {"observed_steady_s", observed(p.observed)}};
            // Measurement only (not compared by stated()): carrier offset of the row's packets.
            if (const auto offsets = summarize_offsets(row.droneid_offsets)) {
                const auto band = [](const OffsetBand& b) {
                    return b.count ? Json{{"packets", b.count}, {"mean_ppm", b.mean_ppm}, {"spread_ppm", b.spread_ppm}}
                                   : Json(nullptr);
                };
                result["droneid"]["carrier_offset"] = {{"latest_hz", offsets->latest_hz},
                    {"latest_ppm", offsets->latest_ppm}, {"latest_channel_hz", offsets->latest_channel_hz},
                    {"all", band(offsets->all)}, {"band_2g4", band(offsets->low)}, {"band_5g", band(offsets->high)},
                    {"band_difference_ppm", offsets->band_difference_ppm ? Json(*offsets->band_difference_ppm) : Json(nullptr)}};
            }
        }
        return result;
    }

    // What a row tells the reader, without the capture numbers and times behind it.
    static Json stated(const Json& row) {
        Json out;
        for (const char* key : {"band", "group", "candidates", "identity_display", "possible_conflict"}) out[key] = row.at(key);
        if (row.contains("droneid")) out["serial"] = row.at("droneid").at("serial");
        if (row.contains("classifier_hint")) out["classifier_hint"] = row.at("classifier_hint").at("model");
        if (row.contains("only_classifier_class")) out["only_classifier_class"] = row.at("only_classifier_class");
        if (row.contains("remote_id")) {
            out["remote_id"] = row.at("remote_id");
            for (auto& rid : out["remote_id"]) rid.erase("observed_steady_s");
        }
        out["has_timing"] = row.contains("timing");
        out["verdict"] = row.at("verdict");
        return out;
    }

    void append(const char* event, const std::string& id, const Json& row) {
        std::error_code error;
        // Bounded: one previous file is kept (<name>.1) once the log passes max_bytes.
        if (output_.is_open() && written_ > max_bytes) {
            output_.close();
            auto old = path_;
            old += ".1";
            std::filesystem::rename(path_, old, error);
            error.clear();
        }
        if (!output_.is_open() && !failed_) {
            if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path(), error);
            if (!error) output_.open(path_, std::ios::app);
            if (!output_) failed_ = true;
            const auto size = std::filesystem::file_size(path_, error);
            written_ = error ? 0 : size;
        }
        if (failed_) return;
        const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto line = Json{{"schema", "rfmon.drone_detection.v1"}, {"event", event},
                               {"id", id}, {"unix_ms", unix_ms}, {"evidence", row}}
                              .dump(-1, ' ', false, Json::error_handler_t::replace);
        output_ << line << '\n';
        written_ += line.size() + 1;
        output_.flush();
        failed_ = !output_;
    }

    std::filesystem::path path_;
    std::ofstream output_;
    std::map<std::string, Json> previous_;
    bool failed_{};
    std::uintmax_t written_{};
public:
    std::uintmax_t max_bytes = 64ull << 20;
};
}
