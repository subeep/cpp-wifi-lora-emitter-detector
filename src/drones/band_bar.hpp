#pragma once

// The band bar at the top of the Drones page: the three drone bands (2.4, 5.2, 5.8 GHz), where
// the receiver looks in them, and where each current row was heard. Display only.
//
// Pure layout and arithmetic, no ImGui, so it can be tested; drones_page.cpp draws it.
// How exact a mark is depends on what heard it:
//   DroneID   the documented channel the packet was decoded on (exact);
//   Remote ID the channel it was received on;
//   link      only the captures it was heard in, so the mark spans those captures' usable
//             range: the link is somewhere inside, not across all of it;
//   classifier-only  the slot centre the classifier saw.

#include "coordinated_schedule.hpp"
#include "drones/evidence_claims.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace rfmon::band_bar {
namespace dc = rfmon::drone_claims;

// Each segment shows its drone band with this margin on both sides, so DroneID 2399.5 MHz (just
// below 2.4 GHz) and visit spans that reach past the band edges stay visible.
inline constexpr double margin_hz = 5e6;

struct Segment {
    double low_hz{}, high_hz{};            // shown
    double band_low_hz{}, band_high_hz{};  // the drone band itself
    const char* name{};
};
inline std::vector<Segment> segments() {
    std::vector<Segment> out;
    for (const auto& b : coord::drone_bands)
        out.push_back({b.low_hz - margin_hz, b.high_hz + margin_hz, b.low_hz, b.high_hz, b.name});
    return out;
}
// Index of the segment showing hz, or -1.
inline int segment_of(const std::vector<Segment>& s, double hz) {
    for (std::size_t i = 0; i < s.size(); ++i)
        if (hz >= s[i].low_hz && hz <= s[i].high_hz) return int(i);
    return -1;
}

// Where each segment goes. Side by side: one line, widths proportional to the spans shown,
// `gap` apart. Stacked: each segment the full width, on its own line.
struct Placed {
    float x0{}, x1{};
    int line{};
};
inline std::vector<Placed> place(const std::vector<Segment>& s, float width, float gap, bool stacked) {
    std::vector<Placed> out;
    if (stacked) {
        for (std::size_t i = 0; i < s.size(); ++i) out.push_back({0, width, int(i)});
        return out;
    }
    double total = 0;
    for (const auto& seg : s) total += seg.high_hz - seg.low_hz;
    const float usable = std::max(0.f, width - gap * float(s.size() > 0 ? s.size() - 1 : 0));
    float x = 0;
    for (const auto& seg : s) {
        const float w = total > 0 ? float((seg.high_hz - seg.low_hz) / total) * usable : 0;
        out.push_back({x, x + w, 0});
        x += w + gap;
    }
    return out;
}
// x of a frequency inside a placed segment, clamped to it.
inline float x_of(const Segment& s, const Placed& p, double hz) {
    const double f = std::clamp((hz - s.low_hz) / (s.high_hz - s.low_hz), 0.0, 1.0);
    return p.x0 + float(f) * (p.x1 - p.x0);
}

// Coverage: what the receiver heard and where it will look.
struct Span {
    double low_hz{}, high_hz{};
    double age_s{};          // since that visit ended (0 while receiving)
    bool receiving{}, valid{true};
};
// The latest visit to each centre outside sub-GHz, with its usable span.
inline std::vector<Span> visited(const std::deque<coord::VisitRecord>& visits, double now_s) {
    std::map<long long, const coord::VisitRecord*> latest;
    for (const auto& v : visits) {
        if (v.center_hz < 1e9) continue;
        auto& slot = latest[std::llround(v.center_hz / 1e3)];
        if (!slot || v.start_s >= slot->start_s) slot = &v;
    }
    std::vector<Span> out;
    for (const auto& [key, v] : latest)
        out.push_back({v->center_hz - v->usable_half_span_hz, v->center_hz + v->usable_half_span_hz,
                       v->in_progress ? 0 : std::max(0.0, now_s - v->end_s), v->in_progress, v->valid || v->in_progress});
    return out;
}
// Every centre the schedule visits outside sub-GHz (enabled slots), with its usable span.
inline std::vector<Span> configured(const coord::Schedule& s) {
    std::vector<Span> out;
    for (const auto& slot : s.slots) {
        if (!slot.enabled) continue;
        for (const auto& v : slot.visits)
            if (v.center_hz >= 1e9) {
                const double h = coord::usable_half_span(v.sample_rate_hz);
                out.push_back({v.center_hz - h, v.center_hz + h, 0, false, true});
            }
    }
    return out;
}
// Opacity of a visited span: full while receiving, then from 0.85 down to 0.2 over fade_s.
inline float shade(const Span& s, double fade_s = 60) {
    if (s.receiving) return 1.f;
    return float(0.2 + 0.65 * std::clamp(1 - s.age_s / fade_s, 0.0, 1.0));
}

// Where a row was heard.
enum class Kind { DroneId, RemoteId, Classifier, Link };  // most exact first
inline const char* kind_name(Kind k) {
    return k == Kind::DroneId ? "DroneID" : k == Kind::RemoteId ? "Remote ID" : k == Kind::Classifier ? "classifier" : "link";
}
struct Mark {
    Kind kind{};
    double low_hz{}, high_hz{};  // equal for a point
    double age_s{};
    double centre() const { return (low_hz + high_hz) / 2; }
    bool point() const { return low_hz == high_hz; }
};
// Usable half span of a link's captures when the result does not carry its rate.
inline constexpr double default_rate_hz = 20e6;

inline std::vector<Mark> marks(const dc::Detection& row, dc::Clock::time_point now) {
    const auto age = [&](dc::Clock::time_point t) { return std::max(0.0, std::chrono::duration<double>(now - t).count()); };
    std::vector<Mark> out;
    if (row.packet) {
        const double hz = row.packet->frequency > 0 ? row.packet->frequency
                          : row.packet->channel_hz > 0 ? row.packet->channel_hz : row.packet_key.center;
        if (hz > 0) out.push_back({Kind::DroneId, hz, hz, age(row.packet->observed)});
    }
    std::vector<long long> seen;
    for (const auto& rid : row.remote_id) {
        if (!(rid.frequency > 0)) continue;
        const auto key = std::llround(rid.frequency / 1e5);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        out.push_back({Kind::RemoteId, rid.frequency, rid.frequency, age(rid.observed)});
    }
    if (row.universal && row.universal->classifier_only && row.link_low_hz > 0)
        out.push_back({Kind::Classifier, row.link_low_hz, row.link_high_hz, age(row.universal->last_observed)});
    if (row.timing && row.link_low_hz > 0) {
        const double h = coord::usable_half_span(row.timing->rate > 0 ? row.timing->rate : default_rate_hz);
        out.push_back({Kind::Link, row.link_low_hz - h, row.link_high_hz + h, age(row.timing->observed)});
    }
    return out;
}
// The mark a row's label points at in one segment: the most exact kind there (marks() lists
// them most exact first). nullptr when the row has no mark in that segment.
inline const Mark* anchor(const std::vector<Mark>& m, const std::vector<Segment>& s, int segment) {
    for (const auto& x : m)
        if (segment_of(s, x.centre()) == segment) return &x;
    return nullptr;
}

// Labels above the bar. Taken from left to right; each goes on the lowest line where it does
// not come within `gap` of a label already there, as close above its mark as the range
// [x0, x1] allows. A label with no room on any of `lines` lines gets line -1 (not drawn).
struct Label {
    float want_x{}, width{};  // mark x; label width
    float x{};                // left edge, once placed
    int line{-1};
};
inline void stack(std::vector<Label>& labels, float x0, float x1, float gap, int lines) {
    std::vector<std::size_t> order(labels.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return labels[a].want_x < labels[b].want_x; });
    std::vector<std::vector<std::pair<float, float>>> taken(std::size_t(std::max(lines, 0)));
    for (const auto i : order) {
        auto& l = labels[i];
        l.line = -1;
        l.x = std::clamp(l.want_x - l.width / 2, x0, std::max(x0, x1 - l.width));
        for (int line = 0; line < lines; ++line) {
            auto& used = taken[std::size_t(line)];
            const bool free = std::none_of(used.begin(), used.end(), [&](const auto& u) {
                return l.x < u.second + gap && l.x + l.width + gap > u.first;
            });
            if (!free) continue;
            used.push_back({l.x, l.x + l.width});
            l.line = line;
            break;
        }
    }
}

}  // namespace rfmon::band_bar
