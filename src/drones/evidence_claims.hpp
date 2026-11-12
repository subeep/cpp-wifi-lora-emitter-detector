#pragma once

#include "symbol_timing_monitor.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <map>
#include <tuple>
#include <string_view>

namespace rfmon::drone_claims {
using Clock = std::chrono::steady_clock;
// Evidence stays current for longer than one coordinated sweep: with the default schedule a
// centre is revisited about every 26 s, so a shorter lifetime makes a steady drone appear and
// expire on every sweep. A link sighting is also withdrawn early, by a later capture of the same
// centre that shows no link (see detections()). DroneID bursts are sparse, so a missing packet
// on a later visit says nothing and packets simply age out.
inline constexpr auto lifetime = std::chrono::seconds(45);
inline constexpr auto packet_lifetime = std::chrono::seconds(60);
// A decoded identity is kept past packet_lifetime while a link that can be the same aircraft is
// still being seen (see supports()), so a drone that is still there does not fall back to
// "Link only" between two decoded packets. The hold ends packet_hold after the packet at the
// latest: beyond that a similar link is as likely to be another aircraft.
inline constexpr auto packet_hold = std::chrono::seconds(300);

struct Key {
    std::string run;
    uint64_t session{}, capture{};
    double center{};
    bool operator==(const Key& other) const {
        return std::tie(run, session, capture, center) ==
               std::tie(other.run, other.session, other.capture, other.center);
    }
};

struct ReportedPosition {
    double latitude{}, longitude{};
    std::optional<double> height_m, altitude_m, speed_m_s;
    Clock::time_point observed{};
    std::string source;
};

inline std::optional<ReportedPosition> reported_position(
    std::optional<double> lat, std::optional<double> lon, Clock::time_point observed, std::string source) {
    if (!lat || !lon || !std::isfinite(*lat) || !std::isfinite(*lon) ||
        std::abs(*lat) > 90 || std::abs(*lon) > 180 || (*lat == 0 && *lon == 0)) return {};
    return ReportedPosition{*lat, *lon, {}, {}, {}, observed, std::move(source)};
}

struct ReportedPositions {
    std::optional<ReportedPosition> aircraft, pilot, home, operator_location;
};

struct Packet {
    std::string model, serial;
    bool readable{};
    Clock::time_point observed;
    int version{};
    double frequency{};
    bool encrypted{};
    int product_code{-1};
    Clock::time_point current_until{};  // set by Store: observed + packet_lifetime, then extended by a supporting link
    ReportedPositions positions;
};

struct Region {
    Key key;
    std::optional<symbol_timing::Result> timing;
    std::vector<Packet> packets;
};

struct Claim {
    const char* observation = "unusable";
    const char* activity = "uncertain";
    std::string group;
    std::string candidates;
    std::vector<Packet> identities;
    bool conflict{};
};

// One row per model: the video link DJI advertises for it and the symbol-timing group
// (subcarrier spacing) that link shows. Names are the DroneID product-table names without a
// leading "DJI ", plus the newer models that have no readable product code yet.
//   measured = true : the group was measured on recordings of that model
//                     (docs/DRONE_RF_T07_CP_LAG_PROBE_2026-11-10.md, DRFF-R2, Tampere, X310;
//                     RFUAV rows: data/drone_ml/2026-11-12/rfuav_decode_01; KU Leuven rows:
//                     data/drone_ml/2026-11-12/kuleuven_decode_01).
//   measured = false: the group is only expected, because measured models with the same link
//                     type show it. Used to list candidates, never to flag a conflict.
//   group = nullptr : the timing detector gives no group for this model (Wi-Fi links look like
//                     Wi-Fi), or the link is not fixed by the model (flight controllers), or it
//                     is not known.
//   also            : a second group seen on some recordings of the model.
// Advertised link names are not waveform labels: O3 and O4 models are split across groups, so
// expected groups are given only where every measured model of that link type agrees.
struct ModelLink {
    const char* model;
    const char* link;
    const char* group;
    bool measured;
    const char* also = nullptr;
};
inline constexpr ModelLink model_links[] = {
    // Lightbridge: 32.8 kHz, measured on five models of both generations.
    {"Inspire 2", "Lightbridge 2", "32.8kHz", true},
    {"M100", "Lightbridge", "32.8kHz", true},
    {"M210", "Lightbridge 2", "32.8kHz", true},
    {"Phantom 4", "Lightbridge 2", "32.8kHz", true},
    {"Phantom 4 Pro", "Lightbridge 2", "32.8kHz", true},
    {"Inspire 1", "Lightbridge", "32.8kHz", false},
    {"Phantom 3 Series", "Lightbridge", "32.8kHz", false},
    {"M600", "Lightbridge 2", "32.8kHz", false},
    {"M600 Pro", "Lightbridge 2", "32.8kHz", false},
    {"M200", "Lightbridge 2", "32.8kHz", false},
    {"M210RTK", "Lightbridge 2", "32.8kHz", false},
    {"Phantom 4 Adv", "Lightbridge 2", "32.8kHz", false},
    // OcuSync 1 and 2: 15 kHz, measured on Mavic Pro (O1) and Mavic Air 2 (O2).
    {"Mavic Pro", "OcuSync 1", "15kHz", true},
    {"Mavic Air 2", "OcuSync 2", "15kHz", true},
    {"Mavic 2", "OcuSync 2", "15kHz", false},
    {"Mavic 2 Enterprise", "OcuSync 2", "15kHz", false},
    {"Mavic 2 Enterprise Advanced", "OcuSync 2", "15kHz", true},   // KU Leuven "Mavic"; code 69
    {"Mini 2", "OcuSync 2", "15kHz", true},               // KU Leuven; DroneID code 63
    {"Mini 3", "OcuSync 2", "15kHz", true},              // RFUAV; DroneID product code 87
    {"M200 V2 Series", "OcuSync 2", "15kHz", false},
    {"M300 RTK", "OcuSync 2 Enterprise", "15kHz", true},          // KU Leuven "matrice"; code 60
    {"Phantom 4 Pro V2.0", "OcuSync 2", "15kHz", false},
    {"Phantom 4 RTK", "OcuSync", "15kHz", false},
    {"P4M", "OcuSync", "15kHz", false},
    {"AGRAS T10", "OcuSync 2", "15kHz", false},
    {"AGRAS T30", "OcuSync 2", "15kHz", false},
    // O3: the measured models disagree (15 kHz alone, or 15 and 30 kHz together), so the
    // unmeasured ones get no expected group.
    {"Mini 3 Pro", "O3", "15kHz", true},
    {"Air 2S", "O3", "15kHz+30kHz", true, "15kHz"},
    {"Mavic 3", "O3+", "15kHz", true, "15kHz+30kHz"},
    {"M30", "O3 Enterprise", nullptr, false},
    {"FPV", "O3 (FPV)", "15kHz", true, "15kHz+30kHz"},   // RFUAV: 30 kHz appears with 20/40 MHz video
    {"Mavic 3 Pro", "O3+", "15kHz", true},               // RFUAV, weak: 0.33-0.65 of blocks
    // O4: Mini 4 Pro and Air 3S measure 30 kHz, Mini 5 Pro (O4+) measures 15 kHz.
    {"Mini 4 Pro", "O4", "30kHz", true},
    {"Avata 2", "O4", "30kHz", true},                    // RFUAV; DroneID encrypted
    {"Air 3S", "O4", "30kHz", true},
    {"Air 3", "O4", "30kHz", false},
    {"Mini 5 Pro", "O4+", "15kHz", true},
    // Wi-Fi links: no drone-link timing group.
    {"Phantom 3 Std", "Wi-Fi", nullptr, false},
    {"Phantom 3 4k", "Wi-Fi", nullptr, false},
    {"Spark", "Wi-Fi", nullptr, false},
    {"Mavic Air", "Enhanced Wi-Fi", nullptr, false},
    {"Mavic Mini", "Enhanced Wi-Fi", nullptr, true},
    {"Mini SE", "Enhanced Wi-Fi", nullptr, false},
    // Link not fixed by the product code, or not known.
    {"Phantom 4 Series", "", nullptr, false},
    {"ACEONE", "", nullptr, false}, {"WKM", "", nullptr, false}, {"NAZA", "", nullptr, false},
    {"A2", "", nullptr, false}, {"A3", "", nullptr, false}, {"N2", "", nullptr, false},
    {"A3_AG", "", nullptr, false}, {"MG1", "", nullptr, false}, {"MG2", "", nullptr, false},
    {"MG1A", "", nullptr, false}, {"MG1P", "", nullptr, false}, {"MG1P-RTK", "", nullptr, false},
};

inline const ModelLink* model_link(const std::string& model) {
    const std::string name = model.rfind("DJI ", 0) == 0 ? model.substr(4) : model;
    for (const auto& row : model_links)
        if (name == row.model) return &row;
    return nullptr;
}

// The group a decoded model name must agree with: measured groups only.
inline const char* known_group(const std::string& model) {
    const auto* row = model_link(model);
    return row && row->measured ? row->group : nullptr;
}

// Models whose link shows this group, measured ones first, each with its link type.
inline std::string candidates_for(const std::string& group) {
    std::string measured, expected;
    for (const auto& row : model_links) {
        if (!row.group || (group != row.group && !(row.also && group == row.also))) continue;
        auto& list = row.measured ? measured : expected;
        if (!list.empty()) list += ", ";
        list += std::string(row.model) + " (" + row.link + ")";
    }
    if (!expected.empty()) measured += (measured.empty() ? "" : "; ") + ("expected from link type: " + expected);
    return measured;
}

inline bool compatible(const std::string& a, const std::string& b) {
    return a == b || (a == "15kHz" && b == "15kHz+30kHz") ||
                     (b == "15kHz" && a == "15kHz+30kHz");
}

// Can a link with this group, seen in this band, be the aircraft that sent this packet?
// In the packet's own band any compatible link can. In the other band (dual-band aircraft move
// between 2.4 and 5.8 GHz) only when the model table gives the decoded model that group.
inline bool supports(const Packet& packet, bool packet_5g, const std::string& link_group, bool link_5g,
                     const std::string& row_group = "") {
    if (link_group.empty() || (!row_group.empty() && !compatible(row_group, link_group))) return false;
    const auto* model = packet.readable ? model_link(packet.model) : nullptr;
    const bool model_says = model && model->group &&
        (compatible(model->group, link_group) || (model->also && compatible(model->also, link_group)));
    if (packet_5g != link_5g) return model_says;
    return model_says || !model || !model->group;  // a model with a different table group is not this link
}

inline Claim assess(const Region& region, Clock::time_point now) {
    Claim out;
    const auto fresh = [&](Clock::time_point t, std::chrono::seconds age = lifetime) { return t <= now && now - t <= age; };
    if (region.timing && fresh(region.timing->observed)) {
        out.observation = "partial"; // sampled blocks of the capture
        if (region.timing->timing_group) {
            out.activity = "drone link present (experimental)";
            out.group = region.timing->timing_group;
            out.candidates = candidates_for(out.group);
        }
    }
    for (const auto& packet : region.packets) {
        if (packet.observed > now || now > packet.current_until) continue;
        if (!region.timing || !fresh(region.timing->observed)) out.observation = "valid packet";
        out.activity = "drone link present (DroneID)";
        out.identities.push_back(packet);
        const auto expected = packet.readable ? known_group(packet.model) : nullptr;
        if (expected && !out.group.empty() && !compatible(out.group, expected)) out.conflict = true;
    }
    return out;
}

// A 20 MHz capture can see the same link at adjacent centres. The observed Phantom 4
// spanned 18 MHz. Separate clusters describe separate links, with no aircraft count implied.
inline constexpr double link_join_hz = 20e6;

struct LinkCluster {
    std::string id, group;
    double low{}, high{};
    const symbol_timing::Result* latest{};
    std::vector<Key> captures;
};

struct PacketSighting {
    const Key* key;
    const Packet* packet;
};

inline std::string packet_id(const Key& key, const Packet& packet) {
    if (packet.readable && !packet.serial.empty()) return "serial:" + packet.serial;
    return key.center < 3e9 ? "band:2.4 GHz/unresolved" : "band:5 GHz/unresolved";
}

inline std::vector<std::optional<std::size_t>> associate(
    const std::vector<LinkCluster>& links, const std::map<std::string, PacketSighting>& packets) {
    std::vector<std::optional<std::size_t>> matches(packets.size());
    std::vector<std::size_t> contenders(links.size());
    std::size_t index = 0;
    for (const auto& [id, sighting] : packets) {
        const bool packet_5g = sighting.key->center >= 3e9;
        std::vector<std::size_t> same_band, other_band;
        for (std::size_t i = 0; i < links.size(); ++i) {
            const bool link_5g = links[i].low >= 3e9;
            if (supports(*sighting.packet, packet_5g, links[i].group, link_5g))
                (packet_5g == link_5g ? same_band : other_band).push_back(i);
        }
        const auto& choices = same_band.empty() ? other_band : same_band;
        for (const auto choice : choices) ++contenders[choice];
        if (choices.size() == 1) matches[index] = choices.front();
        ++index;
    }
    for (auto& match : matches)
        if (match && contenders[*match] != 1) match.reset();
    return matches;
}

class Store {
public:
    void timing(const symbol_timing::Result& result) {
        if (auto* row = find({result.run_id, result.radio_session, result.capture_seq, result.center}))
            row->timing = result;
    }
    void decoded(Key key, Packet packet) {
        auto* row = find(std::move(key));
        if (!row) return;
        auto& packets = row->packets;
        const auto duplicate = std::find_if(packets.begin(), packets.end(), [&](const Packet& p) {
            return p.serial == packet.serial && p.model == packet.model &&
                   p.version == packet.version && p.frequency == packet.frequency &&
                   p.observed == packet.observed && p.readable == packet.readable &&
                   p.encrypted == packet.encrypted && p.product_code == packet.product_code;
        });
        if (duplicate != packets.end()) return;
        packet.current_until = packet.observed + packet_lifetime;
        if (packets.size() == 16) packets.erase(packets.begin());
        packets.push_back(std::move(packet));
    }
    std::vector<LinkCluster> clusters(Clock::time_point now) const {
        std::map<long long, const symbol_timing::Result*> newest;
        for (const auto& row : rows_) {
            if (!row.timing || row.timing->observed > now || now - row.timing->observed > lifetime) continue;
            auto& at = newest[std::llround(row.key.center / 1e3)];
            if (!at || row.timing->observed >= at->observed) at = &*row.timing;
        }
        std::map<std::string, std::vector<LinkCluster>> by_group;
        for (const auto& [centre, timing] : newest) {
            if (!timing->timing_group) continue;
            const std::string prefix = std::string(timing->center < 3e9 ? "band:2.4 GHz/" : "band:5 GHz/") +
                timing->timing_group;
            auto& group = by_group[prefix];
            if (group.empty() || timing->center - group.back().high > link_join_hz)
                group.push_back({{}, timing->timing_group, timing->center, timing->center, timing, {}});
            else {
                auto& cluster = group.back();
                cluster.high = timing->center;
                if (timing->observed >= cluster.latest->observed) cluster.latest = timing;
            }
        }
        std::vector<LinkCluster> links;
        std::vector<Anchor> anchors;
        for (auto& [prefix, group] : by_group) {
            // Keep an existing ID while the current range overlaps its prior range.
            // In a split, only one child inherits the ID.
            for (auto& link : group) {
                for (const auto& old : anchors_) {
                    if (old.prefix != prefix || old.low > link.high || old.high < link.low) continue;
                    if (std::none_of(group.begin(), group.end(), [&](const LinkCluster& current) {
                        return current.id == old.id;
                    })) {
                        link.id = old.id;
                        break;
                    }
                }
            }
            for (auto& link : group) {
                if (link.id.empty()) {
                    const auto base = prefix + "@" + std::to_string(std::llround(link.low / 5e6) * 5);
                    link.id = base;
                    unsigned suffix = 1;
                    while (std::any_of(group.begin(), group.end(), [&](const LinkCluster& other) {
                        return &other != &link && other.id == link.id;
                    })) link.id = base + "-" + std::to_string(++suffix);
                }
                anchors.push_back({prefix, link.id, link.low, link.high});
                links.push_back(link);
            }
        }
        // Keep positive captures at still-active centres, back to the most recent
        // withdrawal or group change. A cluster's display representative is only one capture.
        std::map<long long, Clock::time_point> withdrawn;
        for (const auto& row : rows_) {
            if (!row.timing || row.timing->observed > now || now - row.timing->observed > lifetime) continue;
            const auto centre = std::llround(row.key.center / 1e3);
            const auto current = newest.find(centre);
            if (current == newest.end() || !current->second->timing_group) continue;
            if (!row.timing->timing_group ||
                std::string_view(row.timing->timing_group) != current->second->timing_group)
                withdrawn[centre] = std::max(withdrawn[centre], row.timing->observed);
        }
        for (const auto& row : rows_) {
            if (!row.timing || !row.timing->timing_group ||
                row.timing->observed > now || now - row.timing->observed > lifetime) continue;
            const auto centre = std::llround(row.key.center / 1e3);
            const auto current = newest.find(centre);
            if (current == newest.end() || !current->second->timing_group ||
                std::string_view(row.timing->timing_group) != current->second->timing_group) continue;
            if (const auto cutoff = withdrawn.find(centre);
                cutoff != withdrawn.end() && row.timing->observed <= cutoff->second) continue;
            for (auto& link : links)
                if (row.key.center >= link.low && row.key.center <= link.high &&
                    row.timing->timing_group == link.group)
                    link.captures.push_back(row.key);
        }
        anchors_ = std::move(anchors);
        return links;
    }

    std::map<std::string, PacketSighting> packets(Clock::time_point now, bool for_hold = false) const {
        std::map<std::string, PacketSighting> sightings;
        for (const auto& row : rows_) {
            for (const auto& packet : row.packets) {
                if (packet.observed > now || now > (for_hold ? packet.observed + packet_hold : packet.current_until))
                    continue;
                const auto id = packet_id(row.key, packet);
                const auto existing = sightings.find(id);
                if (existing == sightings.end() || existing->second.packet->observed <= packet.observed)
                    sightings[id] = {&row.key, &packet};
            }
        }
        return sightings;
    }

    void expire(Clock::time_point now) {
        for (auto& row : rows_)
            if (row.timing && now - row.timing->observed > lifetime) row.timing.reset();
        const auto links = clusters(now);
        const auto sightings = packets(now, true);
        const auto matches = associate(links, sightings);
        std::map<const Packet*, Clock::time_point> held;
        std::size_t i = 0;
        for (const auto& [id, sighting] : sightings) {
            const auto match = matches[i++];
            if (match)
                held.emplace(sighting.packet, links[*match].latest->observed + lifetime);
        }
        for (auto& row : rows_) {
            auto& packets = row.packets;
            for (auto& packet : packets) {
                packet.current_until = packet.observed + packet_lifetime;
                const auto match = held.find(&packet);
                if (match != held.end())
                    packet.current_until = std::max(packet.current_until,
                        std::min(match->second, packet.observed + packet_hold));
            }
            packets.erase(std::remove_if(packets.begin(), packets.end(),
                [&](const Packet& packet) { return now > packet.current_until; }), packets.end());
        }
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
            [](const Region& row) { return !row.timing && row.packets.empty(); }), rows_.end());
    }
    const std::vector<Region>& rows() const { return rows_; }
private:
    Region* find(Key key) {
        if (!std::isfinite(key.center) || key.center <= 0 || key.run.empty()) return nullptr;
        for (auto& row : rows_) if (row.key == key) return &row;
        if (rows_.size() == 512) rows_.erase(rows_.begin());  // about 1.6 captures/s for 60 s is ~100
        rows_.push_back({std::move(key), {}, {}});
        return &rows_.back();
    }
    struct Anchor { std::string prefix, id; double low, high; };
    mutable std::vector<Anchor> anchors_;
    std::vector<Region> rows_;
};

struct ClassifierHint {
    std::string model;
    std::size_t votes{}, voting_captures{};
};

struct RemoteIdSighting {
    std::string track, reported_id, ua_type, transport;
    bool serial_id{};
    double frequency{};
    Clock::time_point observed{};
    ReportedPositions positions;
    int id_type{};
};

struct Detection {
    std::string id, band, group, candidates;
    bool conflict{};
    std::optional<symbol_timing::Result> timing;
    std::optional<Packet> packet;
    Key packet_key;
    double link_low_hz{}, link_high_hz{};
    std::string link_id;
    std::vector<Key> link_captures;
    std::optional<ClassifierHint> classifier_hint;
    std::string only_classifier_class;
    std::vector<RemoteIdSighting> remote_id;
};

// Keep Remote ID current across a coordinated sweep (about 26 seconds per revisit).
inline constexpr auto remote_id_lifetime = lifetime;

// CTA-2063 form: four uppercase manufacturer characters, then a base-36 serial
// length character, then the serial. This association is unverified on a live aircraft.
inline bool remote_id_serial_matches(std::string_view reported, std::string_view serial) {
    if (serial.empty()) return false;
    if (reported == serial) return true;
    if (reported.size() != serial.size() + 5 || reported.substr(5) != serial) return false;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < 4; ++i) if (digit(reported[i]) < 0) return false;
    return digit(reported[4]) == static_cast<int>(serial.size());
}

inline std::map<std::string, Detection> detections(
    const Store& store, Clock::time_point now, const std::vector<RemoteIdSighting>& remote_ids = {}) {
    std::map<std::string, Detection> rows;
    const auto links = store.clusters(now);
    const auto packets = store.packets(now);
    const auto matches = associate(links, packets);
    std::vector<bool> folded(links.size());
    const auto attach = [](Detection& row, const LinkCluster& link) {
        row.timing = *link.latest;
        row.group = link.group;
        row.candidates = candidates_for(row.group);
        row.link_id = link.id;
        row.link_low_hz = link.low;
        row.link_high_hz = link.high;
        row.link_captures = link.captures;
        if (!row.packet || link.latest->observed >= row.packet->observed)
            row.band = link.low < 3e9 ? "2.4 GHz" : "5 GHz";
    };
    std::size_t i = 0;
    for (const auto& [id, sighting] : packets) {
        auto& row = rows[id];
        row.id = id;
        row.packet = *sighting.packet;
        row.packet_key = *sighting.key;
        row.band = row.packet_key.center < 3e9 ? "2.4 GHz" : "5 GHz";
        if (const auto match = matches[i++]) {
            attach(row, links[*match]);
            folded[*match] = true;
        }
        // Preserve a measured disagreement from the packet's own capture, even when the
        // incompatible link must be shown separately.
        if (row.packet->readable)
            for (const auto& region : store.rows())
                if (region.key == row.packet_key && region.timing &&
                    region.timing->observed <= now && now - region.timing->observed <= lifetime &&
                    region.timing->timing_group)
                    if (const auto* expected = known_group(row.packet->model))
                        row.conflict = !compatible(region.timing->timing_group, expected);
    }
    for (std::size_t link = 0; link < links.size(); ++link) {
        if (folded[link]) continue;
        auto& row = rows[links[link].id];
        row.id = links[link].id;
        attach(row, links[link]);
    }
    for (const auto& rid : remote_ids) {
        if (rid.track.empty() || rid.observed > now || now - rid.observed > remote_id_lifetime) continue;
        auto serial = rid.serial_id && !rid.reported_id.empty()
            ? rows.find("serial:" + rid.reported_id) : rows.end();
        if (rid.serial_id && serial == rows.end()) {
            for (auto it = rows.begin(); it != rows.end(); ++it) {
                const auto& packet = it->second.packet;
                if (!packet || !packet->readable || !remote_id_serial_matches(rid.reported_id, packet->serial)) continue;
                if (serial != rows.end()) { serial = rows.end(); break; }
                serial = it;
            }
        }
        if (serial != rows.end()) {
            serial->second.remote_id.push_back(rid);
        } else {
            const auto id = "rid:" + rid.track;
            auto& row = rows[id];
            row.id = id;
            row.band = rid.frequency > 0 ? (rid.frequency < 3e9 ? "2.4 GHz" : "5 GHz") : "Unknown";
            row.remote_id.push_back(rid);
        }
    }
    return rows;
}

inline std::string display_identity(const Detection& row) {
    if (!row.packet) {
        if (!row.remote_id.empty()) return "Remote ID broadcast";
        return "Link only (" + row.group + ")";
    }
    const auto& packet = *row.packet;
    if (packet.encrypted) return "DJI DroneID present, identity encrypted";
    if (packet.readable && !packet.model.empty()) return packet.model;
    if (packet.readable && packet.product_code >= 0)
        return "Unknown DJI product code " + std::to_string(packet.product_code);
    return "DJI DroneID present, identity unreadable";
}
}
