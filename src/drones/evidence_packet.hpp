#pragma once

#include "evidence_claims.hpp"
#include <charconv>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string_view>

namespace rfmon::drone_claims {
inline void ingest_native_packet(Store& store, const std::string& text, Clock::time_point now) {
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    const auto value = [&](const char* key) -> std::string {
        const auto it = j.find(key);
        if (it == j.end() || it->is_null()) return {};
        return it->is_string() ? it->get<std::string>() : it->dump();
    };
    const auto number = [&](const char* key) {
        const auto raw = value(key);
        char* end = nullptr;
        const double v = std::strtod(raw.c_str(), &end);
        return end == raw.c_str() + raw.size() && std::isfinite(v) ? v : 0.;
    };
    if (value("protocol") != "DJI DroneID" || value("outer_crc24") != "true") return;
    const auto origin = value("receiver_source");
    std::string_view source(origin);
    constexpr std::string_view prefix = "LIVE / run ";
    if (source.substr(0, prefix.size()) != prefix) return;
    source.remove_prefix(prefix.size());
    const auto read_field = [&](std::string_view delimiter) {
        const auto split = source.find(delimiter);
        if (split == std::string_view::npos) return std::string_view{};
        const auto field = source.substr(0, split);
        source.remove_prefix(split + delimiter.size());
        return field;
    };
    const auto run = read_field(" / session ");
    const auto session = read_field(" / capture ");
    const auto capture = read_field(" / sample ");
    Key key{std::string(run), 0, 0, number("center_hz")};
    const auto integer = [](std::string_view token, uint64_t& result) {
        if (token.empty()) return false;
        const auto end = token.data() + token.size();
        const auto parsed = std::from_chars(token.data(), end, result);
        return parsed.ec == std::errc{} && parsed.ptr == end;
    };
    if (!integer(session, key.session) || !integer(capture, key.capture)) return;
    const double observed = number("acquired_steady_s");
    const double current = std::chrono::duration<double>(now.time_since_epoch()).count();
    if (observed <= 0 || observed > current || current - observed > packet_lifetime.count()) return;
    const auto when = Clock::time_point(std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(observed)));
    const auto readable = value("identity_readable");
    const auto version = number("payload_version");
    auto model = value("reported_model");
    if (model.empty()) model = value("model");
    if (value("model_source") == "unknown code") model.clear();
    auto serial = value("reported_serial");
    if (serial.empty()) serial = value("serial");
    const auto code = value("product_code");
    int product_code = -1;
    if (!code.empty()) {
        const auto end = code.data() + code.size();
        const auto parsed = std::from_chars(code.data(), end, product_code);
        if (parsed.ec != std::errc{} || parsed.ptr != end || product_code < 0 || product_code > 255)
            product_code = -1;
    }
    Packet packet{std::move(model), std::move(serial),
        readable == "true" || (readable.empty() && value("telemetry_parsed") == "true"),
        when, version == 2 || version == 3 ? static_cast<int>(version) : 0, number("center_hz"), value("identity_encrypted") == "true", product_code};
    const auto optional_number = [&](const char* field) -> std::optional<double> {
        const auto raw = value(field);
        if (raw.empty()) return {};
        char* end = nullptr;
        const auto result = std::strtod(raw.c_str(), &end);
        if (end != raw.c_str() + raw.size() || !std::isfinite(result)) return {};
        return result;
    };
    const auto position = [&](const char* lat, const char* lon) {
        return reported_position(optional_number(lat), optional_number(lon), when, "DroneID");
    };
    packet.positions.aircraft = position("reported_drone_lat", "reported_drone_lon");
    packet.positions.pilot = position("reported_pilot_lat", "reported_pilot_lon");
    packet.positions.home = position("reported_home_lat", "reported_home_lon");
    if (auto& aircraft = packet.positions.aircraft) {
        aircraft->height_m = optional_number("reported_height_m");
        aircraft->altitude_m = optional_number("reported_altitude_m");
        aircraft->speed_m_s = optional_number("reported_speed_m_s");
    }
    store.decoded(std::move(key), std::move(packet));
}
}
