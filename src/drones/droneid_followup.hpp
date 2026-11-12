#pragma once
// DroneID follow-up visits (coordinated mode).
//
// A DroneID burst comes about every 0.6 s on one of a few channels, so an ordinary 0.5 s visit
// to a Wi-Fi channel usually misses it. When the symbol-timing detector reports a drone video
// link in a band, this queue asks the scanner for extra, longer visits to that band's listed
// DroneID channels until a DroneID packet is decoded there or the request times out.
//
// Plain state and arithmetic behind one mutex: no radio, no threads, no decoder. The scanner
// calls plan() once per cycle; link sightings and decoded packets are pulled from two source
// functions (set by the wiring) or pushed directly with link_seen()/packet_seen().
// All times are steady-clock seconds (Scanner::now_steady_s()).
#include "droneid_packet.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace rfmon::droneid_followup {

inline constexpr char version[] = "droneid-followup-v1";

struct Config {
    double dwell_s = 1.0;        // valid time per follow-up visit: more than one burst period
    double max_share = 0.25;     // most of the scanner's time follow-up visits may take
    double hold_s = 30;          // a request ends this long after the last link sighting
    double max_request_s = 120;  // ... or this long after it opened, whichever is first
    double cooldown_s = 60;      // after a packet or a give-up, sightings do not reopen it
    double offset_hz = 1.5e6;    // capture centre = channel + offset, keeping it off the DC notch
};

// "" when usable, otherwise the reason.
inline std::string validate(const Config& c) {
    if (!(c.dwell_s >= 0.2 && c.dwell_s <= 3)) return "Follow-up dwell must be 0.2-3 s";
    if (!(c.max_share >= 0 && c.max_share <= 0.5)) return "Follow-up share must be 0-50%";
    if (!(c.hold_s >= 1 && c.hold_s <= 600) || !(c.max_request_s >= 1 && c.max_request_s <= 3600) ||
        !(c.cooldown_s >= 0 && c.cooldown_s <= 3600)) return "Follow-up timers out of range";
    if (!(std::abs(c.offset_hz) <= 4e6)) return "Follow-up offset must keep the channel inside the decoder's view";
    return "";
}

enum class Band { B24 = 0, B58 = 1 };
inline const char* band_name(Band b) { return b == Band::B24 ? "2.4 GHz" : "5.8 GHz"; }
// The band whose DroneID channels a frequency belongs to; false outside both.
inline bool band_of(double hz, Band& out) {
    if (hz >= 2390e6 && hz <= 2500e6) { out = Band::B24; return true; }
    if (hz >= 5650e6 && hz <= 5925e6) { out = Band::B58; return true; }
    return false;
}
inline std::vector<double> channels(Band b) {
    if (b == Band::B24) return {std::begin(drone::droneid::channels_24), std::end(drone::droneid::channels_24)};
    return {std::begin(drone::droneid::channels_58), std::end(drone::droneid::channels_58)};
}

struct LinkSighting { double center_hz = 0, seen_s = 0; std::string group; };   // group "" = no drone link
struct PacketSighting { double center_hz = 0, seen_s = 0; bool identity = false; };  // identity: readable or encrypted

struct Visit {
    Band band = Band::B24;
    double channel_hz = 0, center_hz = 0, dwell_s = 0;
    std::string label;
};

struct BandStatus {
    bool open = false;
    std::string group;            // link group that opened the request
    double opened_s = 0, last_link_s = 0, cooldown_until_s = 0;
    std::string last_outcome;     // "", "packet decoded", "link no longer seen", "timed out"
    uint64_t requests = 0, visits = 0, valid_visits = 0, packets = 0, timeouts = 0, link_lost = 0, suppressed = 0;
};
struct Status {
    std::array<BandStatus, 2> bands;
    double credit_s = 0;          // unspent follow-up time allowance
    uint64_t planned = 0;         // follow-up visits handed to the scanner
};

class Followup {
public:
    explicit Followup(Config config = {}) : config_(validate(config).empty() ? config : Config{}) {}

    Config config() const { std::lock_guard lock(mutex_); return config_; }
    bool set_config(const Config& c) {
        if (!validate(c).empty()) return false;
        std::lock_guard lock(mutex_);
        config_ = c;
        return true;
    }
    void set_enabled(bool on) { std::lock_guard lock(mutex_); enabled_ = on; if (!on) close_all("disabled"); }
    bool enabled() const { std::lock_guard lock(mutex_); return enabled_; }

    // Optional pull sources, called from plan() on the scanner thread. Each returns sightings
    // newer than the previous call needs no bookkeeping by the caller: older ones are ignored.
    void set_link_source(std::function<std::vector<LinkSighting>()> f) { std::lock_guard lock(mutex_); link_source_ = std::move(f); }
    void set_packet_source(std::function<std::vector<PacketSighting>()> f) { std::lock_guard lock(mutex_); packet_source_ = std::move(f); }

    void link_seen(const LinkSighting& s) { std::lock_guard lock(mutex_); on_link(s); }
    void packet_seen(const PacketSighting& p) { std::lock_guard lock(mutex_); on_packet(p); }

    // Follow-up visits for the coming cycle. `cycle_s` is the time the cycle's ordinary visits
    // take and `overhead_s` the settle + pre-roll added to each visit. Time is granted as a
    // share of ordinary scanning, so follow-ups never exceed max_share of the total, whatever
    // the schedule; an allowance is only kept while a request is open.
    std::vector<Visit> plan(double now_s, double cycle_s, double overhead_s) {
        std::function<std::vector<LinkSighting>()> links;
        std::function<std::vector<PacketSighting>()> packets;
        { std::lock_guard lock(mutex_); links = link_source_; packets = packet_source_; }
        std::vector<LinkSighting> new_links;
        std::vector<PacketSighting> new_packets;
        try { if (links) new_links = links(); if (packets) new_packets = packets(); } catch (...) {}

        std::lock_guard lock(mutex_);
        // Packets first: a capture that held both a link and its DroneID closes, not reopens.
        for (const auto& p : new_packets) if (p.seen_s > packet_mark_) on_packet(p);
        for (const auto& p : new_packets) packet_mark_ = std::max(packet_mark_, p.seen_s);
        for (const auto& s : new_links) if (s.seen_s > link_mark_) on_link(s);
        for (const auto& s : new_links) link_mark_ = std::max(link_mark_, s.seen_s);
        expire(now_s);

        std::vector<Visit> out;
        const bool any_open = status_.bands[0].open || status_.bands[1].open;
        if (!enabled_ || !any_open || !(cycle_s > 0) || !(overhead_s >= 0)) { status_.credit_s = 0; return out; }
        const double cost = config_.dwell_s + overhead_s;
        status_.credit_s = std::min(status_.credit_s + config_.max_share / (1 - config_.max_share) * cycle_s, 4 * cost);
        while (status_.credit_s >= cost && out.size() < 8) {
            // Alternate between open bands, rotate through each band's channels.
            if (!status_.bands[next_band_].open) next_band_ ^= 1;
            const Band band = Band(next_band_);
            const auto list = channels(band);
            if (list.empty()) break;
            const double channel = list[cursor_[next_band_]++ % list.size()];
            char text[64];
            std::snprintf(text, sizeof text, "DroneID follow-up %.1f MHz", channel / 1e6);
            out.push_back({band, channel, channel + config_.offset_hz, config_.dwell_s, text});
            status_.credit_s -= cost;
            ++status_.planned;
            next_band_ ^= 1;
        }
        return out;
    }

    // The scanner reports each follow-up visit it made, with the capture it produced (0: none).
    void visit_done(const Visit& v, uint64_t capture_seq, bool valid) {
        std::lock_guard lock(mutex_);
        auto& b = status_.bands[size_t(v.band)];
        ++b.visits;
        b.valid_visits += valid;
        if (capture_seq) {
            captures_.push_back(capture_seq);
            if (captures_.size() > 32) captures_.pop_front();
        }
    }
    // True for a capture made by a follow-up visit: the decoder serves these first.
    bool is_followup_capture(uint64_t capture_seq) const {
        std::lock_guard lock(mutex_);
        return capture_seq && std::find(captures_.begin(), captures_.end(), capture_seq) != captures_.end();
    }

    Status status() const { std::lock_guard lock(mutex_); return status_; }

private:
    void on_link(const LinkSighting& s) {
        Band band;
        if (!enabled_ || s.group.empty() || !std::isfinite(s.seen_s) || !band_of(s.center_hz, band)) return;
        auto& b = status_.bands[size_t(band)];
        if (s.seen_s < b.cooldown_until_s) { ++b.suppressed; return; }
        if (!b.open) {
            b.open = true;
            b.opened_s = s.seen_s;
            b.group = s.group;
            b.last_outcome.clear();
            ++b.requests;
        }
        b.last_link_s = std::max(b.last_link_s, s.seen_s);
    }
    void on_packet(const PacketSighting& p) {
        Band band;
        if (!p.identity || !std::isfinite(p.seen_s) || !band_of(p.center_hz, band)) return;
        auto& b = status_.bands[size_t(band)];
        ++b.packets;
        // Whether or not a request was open, the identity question is answered for a while.
        b.cooldown_until_s = std::max(b.cooldown_until_s, p.seen_s + config_.cooldown_s);
        if (b.open) { b.open = false; b.last_outcome = "packet decoded"; }
    }
    void expire(double now_s) {
        for (auto& b : status_.bands) {
            if (!b.open) continue;
            if (now_s - b.opened_s > config_.max_request_s) {
                b.open = false;
                b.last_outcome = "timed out";
                b.cooldown_until_s = now_s + config_.cooldown_s;
                ++b.timeouts;
            } else if (now_s - b.last_link_s > config_.hold_s) {
                b.open = false;
                b.last_outcome = "link no longer seen";
                ++b.link_lost;
            }
        }
    }
    void close_all(const char* why) {
        for (auto& b : status_.bands) if (b.open) { b.open = false; b.last_outcome = why; }
        status_.credit_s = 0;
    }

    mutable std::mutex mutex_;
    Config config_;
    bool enabled_ = true;
    Status status_;
    std::function<std::vector<LinkSighting>()> link_source_;
    std::function<std::vector<PacketSighting>()> packet_source_;
    double link_mark_ = -1e300, packet_mark_ = -1e300;
    size_t next_band_ = 0;
    std::array<size_t, 2> cursor_{};
    std::deque<uint64_t> captures_;
};

}  // namespace rfmon::droneid_followup
