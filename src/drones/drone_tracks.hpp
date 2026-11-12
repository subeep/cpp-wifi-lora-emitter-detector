#pragma once

#include "model_verdict.hpp"
#include <deque>
#include <set>

namespace rfmon::drone_claims {
// A gap shorter than a coordinated revisit is treated as one appearance.
inline constexpr auto track_return_gap = std::chrono::seconds(30);
inline constexpr auto track_update_interval = std::chrono::seconds(5);
inline constexpr std::size_t max_drone_tracks = 256;

struct DroneTrack {
    uint64_t id{};
    std::string key, identity, serial, group, last_link;
    std::set<std::string> bands, remote_ids, evidence;
    std::set<std::pair<double, double>> link_ranges;
    Verdict best;
    ReportedPositions positions;
    Clock::time_point first_seen{}, last_seen{}, gone_since{}, last_logged{};
    Clock::duration time_present{};
    std::size_t appearances{1};
    bool present{true}, closed{}, encrypted{}, conflict{}, pending_update{};
};

struct TrackEvent {
    std::string event, alert;
    DroneTrack track;
    bool lower_severity{};
    uint64_t merged_into{};
};

inline Clock::time_point row_observed(const Detection& row, Clock::time_point now) {
    auto seen = Clock::time_point{};
    if (row.timing) seen = row.timing->observed;
    if (row.packet) seen = std::max(seen, row.packet->observed);
    for (const auto& rid : row.remote_id) seen = std::max(seen, rid.observed);
    return seen == Clock::time_point{} ? now : std::min(seen, now);
}

inline Clock::time_point row_first_observed(const Detection& row, Clock::time_point now) {
    if (row.timing && row.timing->observed != Clock::time_point{}) now = std::min(now, row.timing->observed);
    if (row.packet && row.packet->observed != Clock::time_point{}) now = std::min(now, row.packet->observed);
    for (const auto& rid : row.remote_id)
        if (rid.observed != Clock::time_point{}) now = std::min(now, rid.observed);
    return now;
}

inline bool copy_position(std::optional<ReportedPosition>& target,
                          const std::optional<ReportedPosition>& fix) {
    if (!fix || !reported_position(fix->latitude, fix->longitude, fix->observed, fix->source) ||
        (target && target->observed >= fix->observed)) return false;
    const bool changed = !target || target->latitude != fix->latitude ||
        target->longitude != fix->longitude || target->height_m != fix->height_m ||
        target->altitude_m != fix->altitude_m || target->speed_m_s != fix->speed_m_s ||
        target->source != fix->source;
    target = fix;
    return changed;
}

inline bool copy_positions(ReportedPositions& target, const ReportedPositions& fixes) {
    bool changed = copy_position(target.aircraft, fixes.aircraft);
    changed |= copy_position(target.pilot, fixes.pilot);
    changed |= copy_position(target.home, fixes.home);
    changed |= copy_position(target.operator_location, fixes.operator_location);
    return changed;
}

class DroneTracks {
public:
    explicit DroneTracks(std::size_t limit = max_drone_tracks) : limit_(limit) {}

    std::vector<TrackEvent> update(const std::map<std::string, Detection>& detections, Clock::time_point now) {
        std::vector<TrackEvent> events;
        if (last_refresh_ && now < *last_refresh_) return events;
        if (last_refresh_)
            for (auto& [key, track] : tracks_)
                if (track.present) track.time_present += now - *last_refresh_;
        last_refresh_ = now;
        std::map<std::string, const Detection*> rows;
        for (const auto& [id, row] : detections) {
            if (row.group.empty() && !row.packet && row.remote_id.empty()) continue;
            // Readable serials outrank an attached link. For an anonymous DroneID/link
            // combination, the link remains the stable identity across decoder expiry.
            std::string key = id;
            if (id.rfind("serial:", 0) != 0) {
                if (!row.remote_id.empty()) key = rid_key(row.remote_id.front());
                else if (!row.link_id.empty()) {
                    key = row.link_id;
                    // The same current link may outlive its decoder evidence. Keep its
                    // history identity while no separate serial row claims that identity.
                    const DroneTrack* prior = nullptr;
                    const bool competing_packet = std::any_of(detections.begin(), detections.end(),
                        [&](const auto& entry) {
                            const auto& other = entry.second;
                            return other.packet && supports(*other.packet, other.packet_key.center >= 3e9,
                                                            row.group, row.band == "5 GHz");
                        });
                    if (!row.packet && !competing_packet)
                        for (const auto& [old_key, track] : tracks_) {
                            if ((!track.present && (track.closed || now - track.gone_since >= track_return_gap)) ||
                                track.serial.empty() || track.last_link != key || detections.count(old_key)) continue;
                            if (prior) { prior = nullptr; break; }
                            prior = &track;
                        }
                    if (prior) key = prior->key;
                }
            }
            const auto [at, inserted] = rows.emplace(key, &row);
            if (!inserted && row_observed(row, now) > row_observed(*at->second, now)) at->second = &row;
        }


        std::map<std::string, std::size_t> claims;
        for (const auto& [key, row] : rows)
            for (const auto& alias : aliases(*row, key))
                if (alias != key) ++claims[alias];
        for (const auto& [key, row] : rows)
            for (const auto& alias : aliases(*row, key)) {
                if (alias == key || rows.count(alias) || claims[alias] != 1) continue;
                promote(alias, key, now, events);
            }

        for (auto& [key, track] : tracks_) {
            if (rows.count(key)) continue;
            if (track.present) {
                track.present = false;
                track.gone_since = now;
            }
            if (!track.closed && now - track.gone_since >= track_return_gap)
                close(track, events);
        }

        for (const auto& [key, evidence] : rows) {
            const auto& row = *evidence;
            auto found = tracks_.find(key);
            const bool fresh = found == tracks_.end();
            if (fresh) {
                if (!make_room(events)) continue;
                found = tracks_.try_emplace(key).first;
                found->second.id = next_id_++;
                found->second.key = key;
                found->second.first_seen = row_first_observed(row, now);
            }
            auto& track = found->second;
            const bool returned = !track.present && (track.closed || now - track.gone_since >= track_return_gap);
            if (returned) {
                if (!track.closed) close(track, events);
                ++track.appearances;
            }
            track.present = true;
            track.closed = false;
            track.last_seen = std::max(track.last_seen, row_observed(row, now));
            absorb(track, row);
            if (fresh || returned) {
                TrackEvent event{"opened", std::string(fresh ? "New drone: " : "Drone returned: ") +
                    track.best.model + ", " + row.band + ", " + track.best.basis,
                    track, track.best.tier != Tier::Confirmed};
                alerts_.push_front(event);
                if (alerts_.size() > 16) alerts_.pop_back();
                events.push_back(std::move(event));
                track.last_logged = now;
                track.pending_update = false;
            } else if (track.pending_update && now - track.last_logged >= track_update_interval) {
                events.push_back({"updated", {}, track});
                track.last_logged = now;
                track.pending_update = false;
            }
        }
        return events;
    }

    std::vector<TrackEvent> finish(Clock::time_point now) {
        auto events = update({}, now);
        for (auto& [key, track] : tracks_)
            if (!track.closed) close(track, events);
        return events;
    }

    const std::map<std::string, DroneTrack>& tracks() const { return tracks_; }
    const std::deque<TrackEvent>& alerts() const { return alerts_; }

private:
    static std::string rid_key(const RemoteIdSighting& rid) {
        return rid.reported_id.empty() ? "rid:" + rid.track :
            "rid-id:" + std::to_string(rid.id_type) + "/" + rid.reported_id;
    }

    static std::set<std::string> aliases(const Detection& row, const std::string& key) {
        std::set<std::string> keys;
        if (row.packet && !row.link_id.empty()) keys.insert(row.link_id);
        for (const auto& rid : row.remote_id) {
            keys.insert("rid:" + rid.track);
            if (key.rfind("serial:", 0) == 0) keys.insert(rid_key(rid));
        }
        return keys;
    }

    static void absorb(DroneTrack& track, const Detection& row) {
        const auto answer = verdict(row);
        if (track.best.model.empty() || answer.tier < track.best.tier ||
            (answer.tier == track.best.tier && answer.model != track.best.model)) {
            track.best = answer;
            track.pending_update = true;
        }
        if (row.timing) {
            track.evidence.insert("Link");
            track.group = row.group;
            track.last_link = row.link_id;
            track.bands.insert(row.timing->center < 3e9 ? "2.4 GHz" : "5 GHz");
            track.link_ranges.emplace(row.link_low_hz, row.link_high_hz);
            if (track.link_ranges.size() > 64) track.link_ranges.erase(track.link_ranges.begin());
        }
        if (!row.band.empty()) track.bands.insert(row.band);
        if (row.packet) {
            track.evidence.insert("DroneID");
            track.bands.insert(row.packet_key.center < 3e9 ? "2.4 GHz" : "5 GHz");
            if (row.packet->readable && !row.packet->serial.empty() && track.serial != row.packet->serial) {
                track.serial = row.packet->serial;
                track.pending_update = true;
            }
            track.encrypted |= row.packet->encrypted;
            track.pending_update |= copy_positions(track.positions, row.packet->positions);
        }
        for (const auto& rid : row.remote_id) {
            track.evidence.insert("Remote ID");
            if (rid.frequency > 0) track.bands.insert(rid.frequency < 3e9 ? "2.4 GHz" : "5 GHz");
            if (!rid.reported_id.empty()) track.pending_update |= track.remote_ids.insert(rid.reported_id).second;
            if (track.remote_ids.size() > 64) track.remote_ids.erase(track.remote_ids.begin());
            track.pending_update |= copy_positions(track.positions, rid.positions);
        }
        if (row.classifier_hint) track.evidence.insert("Classifier hint");
        track.conflict |= row.conflict;
        auto identity = track.serial.empty() ? display_identity(row) : track.serial + " (self-reported)";
        if (track.serial.empty() && !track.remote_ids.empty())
            identity = *track.remote_ids.begin() + " (self-reported)";
        track.pending_update |= track.identity != identity;
        track.identity = std::move(identity);
    }

    static void close(DroneTrack& track, std::vector<TrackEvent>& events, uint64_t merged_into = 0) {
        track.closed = true;
        events.push_back({"closed", {}, track, false, merged_into});
    }

    void promote(const std::string& alias, const std::string& key, Clock::time_point now,
                 std::vector<TrackEvent>& events) {
        auto source = tracks_.find(alias);
        if (source == tracks_.end() || (!source->second.present &&
            (source->second.closed || now - source->second.gone_since >= track_return_gap))) return;
        auto destination = tracks_.find(key);
        if (destination == tracks_.end()) {
            auto node = tracks_.extract(source);
            node.key() = key;
            node.mapped().key = key;
            node.mapped().pending_update = true;
            tracks_.insert(std::move(node));
            return;
        }
        auto& dst = destination->second;
        auto& src = source->second;
        // Overlapping histories represent the same presence. Avoid counting overlap twice.
        const bool disjoint = src.last_seen < dst.first_seen || dst.last_seen < src.first_seen;
        dst.time_present = disjoint ? dst.time_present + src.time_present : std::max(dst.time_present, src.time_present);
        dst.appearances = disjoint ? dst.appearances + src.appearances : std::max(dst.appearances, src.appearances);
        dst.first_seen = std::min(dst.first_seen, src.first_seen);
        dst.last_seen = std::max(dst.last_seen, src.last_seen);
        dst.bands.insert(src.bands.begin(), src.bands.end());
        dst.evidence.insert(src.evidence.begin(), src.evidence.end());
        dst.remote_ids.insert(src.remote_ids.begin(), src.remote_ids.end());
        dst.link_ranges.insert(src.link_ranges.begin(), src.link_ranges.end());
        while (dst.remote_ids.size() > 64) dst.remote_ids.erase(dst.remote_ids.begin());
        while (dst.link_ranges.size() > 64) dst.link_ranges.erase(dst.link_ranges.begin());
        dst.encrypted |= src.encrypted;
        dst.conflict |= src.conflict;
        if (dst.best.model.empty() || src.best.tier < dst.best.tier) dst.best = src.best;
        if (dst.group.empty()) dst.group = src.group;
        copy_positions(dst.positions, src.positions);
        dst.pending_update = true;
        src.present = false;
        close(src, events, dst.id);
        tracks_.erase(source);
    }

    bool make_room(std::vector<TrackEvent>& events) {
        if (tracks_.size() < limit_) return true;
        auto oldest = tracks_.end();
        for (auto it = tracks_.begin(); it != tracks_.end(); ++it)
            if (!it->second.present && (oldest == tracks_.end() || it->second.last_seen < oldest->second.last_seen))
                oldest = it;
        if (oldest == tracks_.end()) return false;
        if (!oldest->second.closed) close(oldest->second, events);
        tracks_.erase(oldest);
        return true;
    }

    std::size_t limit_;
    uint64_t next_id_{1};
    std::optional<Clock::time_point> last_refresh_;
    std::map<std::string, DroneTrack> tracks_;
    std::deque<TrackEvent> alerts_;
};
}
