#include "drones/drone_track_log.hpp"
#include "drones/evidence_packet.hpp"
#include "drones/evidence_remote_id.hpp"

#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace rfmon::drone_claims;
using namespace std::chrono_literals;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() try {
    const auto now = Clock::now();
    DroneTracks tracks;
    Detection link;
    link.id = link.link_id = "band:2.4 GHz/15kHz@2410";
    link.band = "2.4 GHz";
    link.group = "15kHz";
    link.link_low_hz = link.link_high_hz = 2412e6;
    link.timing.emplace();
    link.timing->center = 2412e6;
    link.timing->observed = now - 1s;
    std::map<std::string, Detection> rows{{link.id, link}};
    auto events = tracks.update(rows, now);
    require(events.size() == 1 && events.front().lower_severity &&
            events.front().alert.find("New drone:") == 0, "link opening did not raise one advisory");
    const auto opened = events;
    const auto id = tracks.tracks().begin()->second.id;
    require(tracks.update(rows, now + 1s).empty(), "refresh raised another event");

    Detection named = link;
    named.id = "serial:SERIAL1";
    named.packet = Packet{"Mini 2", "SERIAL1", true, now + 2s, 2, 2412e6};
    named.packet_key.center = 2412e6;
    named.packet->positions.aircraft = reported_position(10., 20., now + 2s, "DroneID");
    named.packet->positions.pilot = ReportedPosition{91., 20., {}, {}, {}, now + 2s, "DroneID"};
    rows = {{named.id, named}};
    tracks.update(rows, now + 2s);
    const auto& promoted = tracks.tracks().at(named.id);
    require(tracks.tracks().size() == 1 && promoted.id == id && promoted.first_seen == now - 1s &&
            promoted.best.tier == Tier::Confirmed && tracks.alerts().size() == 1,
            "serial promotion lost history or raised another alert");
    require(promoted.positions.aircraft && !promoted.positions.pilot, "invalid position entered track");
    events = tracks.update(rows, now + 5s);
    require(events.size() == 1 && events.front().event == "updated", "promotion update was not logged");

    require(tracks.update({{link.id, link}}, now + 5s).empty() &&
            tracks.tracks().size() == 1 && tracks.tracks().count(named.id),
            "decoder evidence flicker recreated an existing link track");
    tracks.update({}, now + 6s);
    require(!tracks.tracks().at(named.id).present, "missing row still present");
    tracks.update(rows, now + 10s);
    require(tracks.tracks().at(named.id).appearances == 1 && tracks.alerts().size() == 1,
            "flicker caused a return alert");
    tracks.update({}, now + 12s);
    events = tracks.update(rows, now + 12s + track_return_gap);
    require(events.size() == 2 && events.front().event == "closed" && events.back().event == "opened" &&
            events.back().alert.find("Drone returned:") == 0 && !events.back().lower_severity &&
            tracks.tracks().at(named.id).appearances == 2 && tracks.alerts().size() == 2,
            "return did not emit a single named alert");
    require(tracks.tracks().at(named.id).time_present == 8s, "time present included absence");

    DroneTracks capped(1);
    capped.update({{link.id, link}}, now);
    capped.update({}, now + 1s);
    auto unrelated = rows;
    unrelated.begin()->second.link_id.clear();
    capped.update(unrelated, now + 2s);
    require(capped.tracks().size() == 1 && capped.tracks().count(named.id), "cap did not evict gone track");
    Detection hint_only;
    hint_only.classifier_hint = ClassifierHint{"Mini 2", 2, 2};
    require(capped.update({{"hint", hint_only}}, now + 3s).empty(), "hint alone opened a track");

    DroneTracks remote_tracks;
    Detection remote;
    remote.id = "rid:original";
    remote.band = "2.4 GHz";
    remote.remote_id.push_back({"original", "ABCD7SERIAL1", "Helicopter / multirotor", "Wi-Fi",
                                true, 2412e6, now, {}, 1});
    remote_tracks.update({{remote.id, remote}}, now);
    remote.id = "rid:changed";
    remote.remote_id.front().track = "changed";
    remote.remote_id.front().observed = now + 1s;
    remote_tracks.update({{remote.id, remote}}, now + 1s);
    auto combined = named;
    combined.remote_id = remote.remote_id;
    remote_tracks.update({{combined.id, combined}}, now + 2s);
    require(remote_tracks.tracks().size() == 1 && remote_tracks.tracks().count(named.id) &&
            remote_tracks.tracks().at(named.id).first_seen == now && remote_tracks.alerts().size() == 1,
            "Remote ID transport change or serial promotion split history");

    Store evidence;
    const auto steady = std::chrono::duration<double>(now.time_since_epoch()).count();
    nlohmann::json packet{{"protocol", "DJI DroneID"}, {"outer_crc24", true},
        {"identity_readable", true}, {"model", "Mini 2"}, {"serial", "S1"}, {"center_hz", 2412e6},
        {"receiver_source", "LIVE / run run / session 1 / capture 2 / sample 0"},
        {"acquired_steady_s", steady - 1.}, {"reported_drone_lat", 12.5}, {"reported_drone_lon", 78.5},
        {"reported_pilot_lat", 91.}, {"reported_pilot_lon", 1.},
        {"reported_home_lat", "12.4"}, {"reported_home_lon", "78.4"}, {"reported_speed_m_s", 3.}};
    ingest_native_packet(evidence, packet.dump(), now);
    const auto positions = evidence.rows().front().packets.front().positions;
    require(positions.aircraft && positions.aircraft->speed_m_s == 3. && positions.home && !positions.pilot,
            "DroneID adapter lost positions or retained invalid latitude");

    using Json = nlohmann::json;
    Json rid{{"enabled", true}, {"tracks", Json::array({Json{{"key", "rid1"}, {"messages", Json::array({
        Json{{"type", "Location"}, {"age_s", 1.}, {"latitude", 12.}, {"longitude", 78.}, {"height_m", 10.}},
        Json{{"type", "System"}, {"age_s", 2.}, {"latitude", 13.}, {"longitude", 79.}, {"operator_location_type", 1}}
    })}}})}};
    const auto sightings = remote_id_sightings(rid, now);
    require(sightings.size() == 1 && sightings[0].positions.aircraft && sightings[0].positions.pilot &&
            sightings[0].positions.pilot->observed < sightings[0].positions.aircraft->observed,
            "Remote ID adapter lost position roles or timestamps");

    const auto path = std::filesystem::temp_directory_path() /
        ("rfmon-drone-tracks-" + std::to_string(getpid()) + ".jsonl");
    auto previous = path;
    previous += ".1";
    std::filesystem::remove(path);
    std::filesystem::remove(previous);
    {
        DroneTrackLog log(path, 1);
        log.record(opened);
        log.record({events.back()});
        require(!log.failed(), "track log failed");
    }
    std::ifstream current(path), rotated(previous);
    std::string line;
    require(bool(std::getline(rotated, line)) && Json::parse(line).at("schema") == "rfmon.drone_track.v1",
            "rotation lost previous event");
    require(bool(std::getline(current, line)), "rotation lost current event");
    const auto json = Json::parse(line);
    require(json.at("track").at("positions").at("aircraft").at("self_reported") == true &&
            json.at("severity") == "normal", "track log lost position attribution or severity");
    std::filesystem::remove(path);
    std::filesystem::remove(previous);
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
