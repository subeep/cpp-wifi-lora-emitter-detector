// Combined drone-entry model for the Drones page (upgradedplan.md §3.2, P2):
// rows from every detector, provisional relationships between rows, and a
// bounded evidence export. Rows are observations, never a count of aircraft;
// relationships are hints with stated reasons, never merges.
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "coordinated_schedule.hpp"
#include "scanner.hpp"

namespace rfmon::evidence {

struct Entry {
    std::string key;         // stable per row across frames (source + identity)
    std::string source;      // "DJI native", "DJI fallback", "Wi-Fi Remote ID", "Model (diagnostic)"
    std::string evidence;    // what the evidence actually is
    std::string identity;    // reported/candidate identity, with its status
    std::string reported_id;     // reported serial / Remote ID (empty: none); used only for claim matching
    std::string reported_model;  // reported model text (empty: none)
    double frequency_hz = 0;
    double last_seen_s = 0;      // Scanner::now_steady_s() clock
    uint64_t count = 0;
    std::optional<uint64_t> capture_seq;        // Wi-Fi/visit capture the latest evidence came from
    std::optional<double> event_device_time_s;  // receiver device time of the latest evidence
    std::vector<std::pair<std::string, std::string>> fields;  // decoded / diagnostic fields
    std::string raw_json;                       // as received from the detector
    coord::Freshness freshness = coord::Freshness::RecentBandUnobserved;
    std::vector<std::string> relations;  // provisional, with reasons
    std::vector<std::string> conflicts;  // disagreements that must stay visible
};

inline std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string row_name(size_t i, const Entry& e) { return "#" + std::to_string(i + 1) + " " + e.source; }

// Provisional relationships (upgradedplan.md §3.2): rows are related only by
// explicit evidence - the same reported identifier, or the same capture.
// Product names, channel, power or time proximity alone never relate rows.
inline void associate(std::vector<Entry>& rows) {
    for (auto& e : rows) { e.relations.clear(); e.conflicts.clear(); }
    for (size_t i = 0; i < rows.size(); ++i)
        for (size_t j = i + 1; j < rows.size(); ++j) {
            auto& a = rows[i];
            auto& b = rows[j];
            if (!a.reported_id.empty() && lower(a.reported_id) == lower(b.reported_id)) {
                const std::string why = "same reported identifier (unauthenticated claims; not proof of one aircraft)";
                a.relations.push_back(row_name(j, b) + ": " + why);
                b.relations.push_back(row_name(i, a) + ": " + why);
                if (!a.reported_model.empty() && !b.reported_model.empty() && lower(a.reported_model) != lower(b.reported_model)) {
                    const std::string c = "reported model differs: '" + a.reported_model + "' (" + a.source + ") vs '" +
                                          b.reported_model + "' (" + b.source + ")";
                    a.conflicts.push_back(c);
                    b.conflicts.push_back(c);
                }
            }
            if (a.capture_seq && b.capture_seq && *a.capture_seq == *b.capture_seq) {
                const std::string why = "evidence from the same capture #" + std::to_string(*a.capture_seq) +
                                        " (same samples; not independent confirmation)";
                a.relations.push_back(row_name(j, b) + ": " + why);
                b.relations.push_back(row_name(i, a) + ": " + why);
            }
        }
}

// Parses "... / capture N / ..." from a drone detector source string.
inline std::optional<uint64_t> capture_from_source(const std::string& source) {
    const auto p = source.find("/ capture ");
    if (p == std::string::npos) return std::nullopt;
    try {
        size_t used = 0;
        const auto v = std::stoull(source.substr(p + 10), &used);
        if (used) return v;
    } catch (...) {
    }
    return std::nullopt;
}

// [begin, end) samples of a retained capture to export around the entry's
// event time; the whole capture when the time cannot be mapped.
inline std::pair<size_t, size_t> export_span(const Entry& e, const Scanner::RetainedCapture& c, double half_window_s) {
    const size_t n = c.iq ? c.iq->size() : 0;
    const double rate = c.record.sample_rate_hz;
    if (!e.event_device_time_s || c.record.clock != wifi_security::ClockDomain::UsrpDevice || !(rate > 0) || !n)
        return {0, n};
    const double offset = (*e.event_device_time_s - c.record.device_time_ns * 1e-9) * rate;
    if (!std::isfinite(offset) || offset < 0 || offset >= double(n)) return {0, n};
    const double half = half_window_s * rate;
    const size_t begin = size_t(std::llround(std::max(0.0, offset - half)));
    const size_t end = size_t(std::llround(std::min(double(n), offset + half)));
    return end > begin ? std::make_pair(begin, end) : std::make_pair(size_t(0), n);
}

inline nlohmann::json visit_json(const coord::VisitRecord& v) {
    return {{"cycle", v.cycle}, {"visit_seq", v.visit_seq}, {"capture_seq", v.capture_seq}, {"band", v.band},
            {"label", v.label}, {"kind", coord::kind_name(v.kind)}, {"center_hz", v.center_hz}, {"rate_hz", v.rate_hz},
            {"usable_half_span_hz", v.usable_half_span_hz}, {"start_steady_s", v.start_s}, {"end_steady_s", v.end_s},
            {"valid", v.valid}, {"analysed", v.analysed}, {"in_progress", v.in_progress}};
}

// Writes a new directory under `parent` (never overwriting) holding
// evidence.json and, when the entry's capture is still retained, iq.cf32_le
// (interleaved little-endian float32) for the exported span. Returns the
// directory. Throws on I/O errors.
inline std::string export_entry(const std::string& parent, const Entry& e, const std::deque<coord::VisitRecord>& visits,
                                const std::string& schedule_hash, const std::optional<Scanner::RetainedCapture>& capture,
                                double now_s, double half_window_s = 0.05) {
    namespace fs = std::filesystem;
    fs::create_directories(parent);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &tm);
    std::string slug;
    for (char c : e.source) slug += std::isalnum(static_cast<unsigned char>(c)) ? char(std::tolower(c)) : '-';
    fs::path dir;
    for (int i = 0;; ++i) {
        dir = fs::path(parent) / (std::string(stamp) + "-" + slug + (i ? "-" + std::to_string(i) : ""));
        if (fs::create_directory(dir)) break;  // false: exists; try the next suffix
        if (i > 1000) throw std::runtime_error("Cannot create a unique evidence directory");
    }
    nlohmann::json j;
    j["schema"] = "rfmon-drone-evidence";
    j["version"] = 1;
    j["note"] = "Unauthenticated claims and unqualified model output; a row is an observation, not a physical aircraft.";
    j["schedule_hash"] = schedule_hash;
    j["entry"] = {{"source", e.source}, {"evidence", e.evidence}, {"identity", e.identity}, {"reported_id", e.reported_id},
                  {"reported_model", e.reported_model}, {"frequency_hz", e.frequency_hz}, {"count", e.count},
                  {"age_s", now_s - e.last_seen_s}, {"freshness", coord::freshness_name(e.freshness)},
                  {"capture_seq", e.capture_seq ? nlohmann::json(*e.capture_seq) : nlohmann::json(nullptr)},
                  {"event_device_time_s", e.event_device_time_s ? nlohmann::json(*e.event_device_time_s) : nlohmann::json(nullptr)},
                  {"relations", e.relations}, {"conflicts", e.conflicts}};
    for (const auto& [k, v] : e.fields) j["entry"]["fields"].push_back({{"name", k}, {"value", v}});
    if (!e.raw_json.empty()) {
        const auto parsed = nlohmann::json::parse(e.raw_json, nullptr, false);
        j["entry"]["raw"] = parsed.is_discarded() ? nlohmann::json(e.raw_json) : parsed;
    }
    j["visits_covering_frequency"] = nlohmann::json::array();
    for (const auto& v : visits)
        if (std::abs(e.frequency_hz - v.center_hz) <= v.usable_half_span_hz || (e.capture_seq && v.capture_seq == *e.capture_seq))
            j["visits_covering_frequency"].push_back(visit_json(v));
    if (capture && capture->iq) {
        const auto& r = capture->record;
        const auto [begin, end] = export_span(e, *capture, half_window_s);
        nlohmann::json overflows = nlohmann::json::array();
        for (const auto& o : r.overflows) overflows.push_back(o.at_sample);
        j["iq"] = {{"file", "iq.cf32_le"}, {"format", "cf32_le"}, {"capture_seq", r.capture_seq}, {"run_id", r.run_id},
                   {"radio_session", r.radio_session}, {"band", r.band}, {"capture_center_hz", r.capture_center_hz},
                   {"actual_rf_hz", r.actual_rf_hz}, {"actual_dsp_hz", r.actual_dsp_hz}, {"sample_rate_hz", r.sample_rate_hz},
                   {"capture_samples", capture->iq->size()}, {"exported_first_sample", begin}, {"exported_samples", end - begin},
                   {"device_time_valid", r.clock == wifi_security::ClockDomain::UsrpDevice},
                   {"first_exported_device_time_ns", r.device_time_ns + int64_t(std::llround(double(begin) / r.sample_rate_hz * 1e9))},
                   {"overflow_samples", overflows}, {"gain_db", r.gain_db ? nlohmann::json(*r.gain_db) : nlohmann::json(nullptr)},
                   {"device", r.device}, {"antenna", r.antenna}};
        std::ofstream iq(dir / "iq.cf32_le", std::ios::binary);
        iq.write(reinterpret_cast<const char*>(capture->iq->data() + begin), std::streamsize((end - begin) * sizeof(float) * 2));
        if (!iq) throw std::runtime_error("IQ write failed");
    } else {
        j["iq"] = nullptr;
        j["iq_unavailable_reason"] = e.capture_seq ? "Capture no longer retained (enable IQ retention before the evidence arrives)"
                                                   : "Entry has no capture reference";
    }
    std::ofstream(dir / "evidence.json") << j.dump(2) << "\n";
    return dir.string();
}

}  // namespace rfmon::evidence
