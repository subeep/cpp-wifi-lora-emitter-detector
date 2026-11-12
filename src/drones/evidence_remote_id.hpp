#pragma once

#include "evidence_claims.hpp"
#include <nlohmann/json.hpp>
#include <limits>

namespace rfmon::drone_claims {
// Adapt the existing monitor snapshot. Only a fresh serial-number Basic ID can join DroneID.
inline std::vector<RemoteIdSighting> remote_id_sightings(const nlohmann::json& snapshot,
                                                       Clock::time_point now) {
    std::vector<RemoteIdSighting> sightings;
    if (!snapshot.is_object() || !snapshot.value("enabled", false)) return sightings;
    constexpr double absent = std::numeric_limits<double>::infinity();
    for (const auto& track : snapshot.value("tracks", nlohmann::json::array())) {
        RemoteIdSighting rid;
        rid.track = track.value("key", std::string{});
        rid.transport = track.value("transport", std::string{"Wi-Fi"});
        double youngest = absent, identity_age = absent;
        for (const auto& message : track.value("messages", nlohmann::json::array())) {
            const double age = message.value("age_s", absent);
            if (!std::isfinite(age) || age < 0 || age > remote_id_lifetime.count()) continue;
            if (age < youngest) {
                youngest = age;
                const auto source = message.find("source");
                rid.frequency = source != message.end() && source->is_object()
                    ? source->value("channel_hz", 0.) : 0.;
                if (!std::isfinite(rid.frequency) || rid.frequency < 0) rid.frequency = 0;
            }
            const auto type = message.value("type", std::string{});
            if (type == "Location" || type == "System") {
                const auto number = [&](const char* key) -> std::optional<double> {
                    const auto field = message.find(key);
                    if (field == message.end() || !field->is_number()) return {};
                    const double v = field->get<double>();
                    return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
                };
                const auto observed = now - std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(age));
                auto fix = reported_position(number("latitude"), number("longitude"), observed, "Remote ID");
                if (fix && type == "Location") {
                    fix->height_m = number("height_m");
                    fix->altitude_m = number("altitude_geo_m");
                    fix->speed_m_s = number("speed_horizontal_m_s");
                    rid.positions.aircraft = std::move(fix);
                } else if (fix) {
                    fix->altitude_m = number("operator_altitude_geo_m");
                    const int location_type = message.value("operator_location_type", -1);
                    // The System message distinguishes takeoff location from live operator GNSS.
                    if (location_type == 0) {
                        fix->source = "Remote ID takeoff";
                        rid.positions.home = std::move(fix);
                    }
                    else if (location_type == 1) rid.positions.pilot = std::move(fix);
                    else rid.positions.operator_location = std::move(fix);
                }
            }
            if (type != "Basic ID") continue;
            const auto id = message.value("reported_id", std::string{});
            const bool serial = message.value("id_type", 0) == 1 && !id.empty();
            if (rid.serial_id && !serial) continue;
            if (rid.serial_id == serial && age >= identity_age) continue;
            rid.reported_id = id.empty() ? message.value("reported_id_hex", std::string{}) : id;
            rid.ua_type = message.value("ua_type_name", std::string{});
            rid.serial_id = serial;
            rid.id_type = message.value("id_type", 0);
            identity_age = age;
        }
        if (rid.track.empty() || !std::isfinite(youngest)) continue;
        rid.observed = now - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(youngest));
        sightings.push_back(std::move(rid));
    }
    return sightings;
}

inline std::string remote_id_identity(const RemoteIdSighting& rid) {
    std::string value = rid.reported_id.empty() ? "ID unavailable" : rid.reported_id + " (self-reported)";
    if (!rid.ua_type.empty()) value += " / " + rid.ua_type + " (self-reported)";
    return value;
}

inline std::string evidence_sources(const Detection& row) {
    std::string sources;
    const auto add = [&](const char* text) {
        if (!sources.empty()) sources += ", ";
        sources += text;
    };
    if (row.timing) add("Link");
    if (row.packet) add("DroneID");
    if (!row.remote_id.empty()) add("Remote ID");
    if (row.classifier_hint) add("Classifier hint");
    return sources;
}
}
