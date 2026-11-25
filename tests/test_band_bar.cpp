// Band bar on the Drones page (drones/band_bar.hpp): segments, placement, coverage spans, where
// each row is marked, and label stacking. Pure layout; the drawing is checked by the page test.
#include "drones/band_bar.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace rfmon;
namespace bb = rfmon::band_bar;
namespace dc = rfmon::drone_claims;

namespace {
int failures = 0;
void check(bool ok, const std::string& name, const std::string& detail = "") {
    std::printf("%s [%s]%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++failures;
}
bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

void segment_tests() {
    const auto s = bb::segments();
    check(s.size() == 3 && std::string(s[0].name) == "2.4 GHz" && std::string(s[1].name) == "5.2 GHz" &&
              std::string(s[2].name) == "5.8 GHz",
          "Three segments: 2.4, 5.2 and 5.8 GHz");
    check(s[0].band_low_hz == 2400e6 && s[0].band_high_hz == 2483.5e6 && s[0].low_hz == 2395e6 && s[0].high_hz == 2488.5e6 &&
              s[2].band_low_hz == 5725e6 && s[2].band_high_hz == 5850e6,
          "Each segment is its drone band plus 5 MHz either side");
    check(bb::segment_of(s, 2399.5e6) == 0 && bb::segment_of(s, 5200e6) == 1 && bb::segment_of(s, 5756.5e6) == 2 &&
              bb::segment_of(s, 5500e6) == -1 && bb::segment_of(s, 865e6) == -1,
          "segment_of: DroneID 2399.5 MHz shows in 2.4 GHz; DFS and sub-GHz in none");

    const auto side = bb::place(s, 1000, 10, false);
    const double total = (93.5 + 110 + 135) * 1e6;
    check(side.size() == 3 && side[0].line == 0 && side[2].line == 0 && near(side[0].x0, 0, 1e-3) &&
              near(side[2].x1, 1000, 1e-2) && near(side[1].x0 - side[0].x1, 10, 1e-3) &&
              near(side[0].x1 - side[0].x0, 980 * 93.5e6 / total, 1e-2),
          "Side by side: widths in proportion to the MHz shown, 10 px apart, filling the width");
    const auto stacked = bb::place(s, 600, 10, true);
    check(stacked[0].line == 0 && stacked[1].line == 1 && stacked[2].line == 2 && stacked[1].x0 == 0 && stacked[1].x1 == 600,
          "Stacked: one segment per line, each the full width");
    check(near(bb::x_of(s[0], side[0], 2395e6), side[0].x0, 1e-3) && near(bb::x_of(s[0], side[0], 2488.5e6), side[0].x1, 1e-3) &&
              near(bb::x_of(s[0], side[0], 2300e6), side[0].x0, 1e-3) && near(bb::x_of(s[0], side[0], 3e9), side[0].x1, 1e-3),
          "x_of: segment ends map to its edges; outside values are clamped");
}

void coverage_tests() {
    std::deque<coord::VisitRecord> visits(5);
    visits[0].center_hz = 2413.5e6; visits[0].usable_half_span_hz = 7.6e6; visits[0].start_s = 10; visits[0].end_s = 10.6; visits[0].valid = true;
    visits[1] = visits[0]; visits[1].start_s = 40; visits[1].end_s = 40.6;                    // later visit, same centre
    visits[2].center_hz = 5758e6; visits[2].usable_half_span_hz = 7.6e6; visits[2].start_s = 49; visits[2].in_progress = true;
    visits[3].center_hz = 5191.5e6; visits[3].usable_half_span_hz = 7.6e6; visits[3].start_s = 45; visits[3].end_s = 45.6;  // invalid
    visits[4].center_hz = 865.5e6; visits[4].usable_half_span_hz = 1.9e6; visits[4].start_s = 30; visits[4].end_s = 30.5; visits[4].valid = true;
    const auto v = bb::visited(visits, 50);
    check(v.size() == 3, "visited: one span per centre, sub-GHz left out", std::to_string(v.size()));
    bool latest = false, receiving = false, failed = false;
    for (const auto& s : v) {
        if (near(s.low_hz, 2405.9e6, 1)) latest = near(s.age_s, 9.4, 1e-9) && s.valid && !s.receiving;
        if (near(s.low_hz, 5750.4e6, 1)) receiving = s.receiving && s.age_s == 0 && s.valid;
        if (near(s.low_hz, 5183.9e6, 1)) failed = !s.valid;
    }
    check(latest, "visited: the latest visit to a centre gives its age");
    check(receiving, "visited: the visit in progress is receiving, age 0");
    check(failed, "visited: a failed visit is marked not valid");
    check(bb::shade({0, 0, 0, true, true}) == 1.f && near(bb::shade({0, 0, 0, false, true}), .85, 1e-6) &&
              near(bb::shade({0, 0, 30, false, true}), .525, 1e-6) && near(bb::shade({0, 0, 600, false, true}), .2, 1e-6),
          "shade: 1 while receiving, then 0.85 fading to 0.2 over 60 s");

    const auto plan = bb::configured(coord::default_schedule(20e6));
    std::size_t centres = 0;
    for (const auto& slot : coord::default_schedule(20e6).slots) if (slot.band != BAND_SUB_GHZ) centres += slot.visits.size();
    check(plan.size() == centres && centres == 15 + 9 + 2 + 11, "configured: every scheduled centre outside sub-GHz",
          std::to_string(plan.size()));
    auto schedule = coord::default_schedule(20e6);
    const auto off = schedule.slots.back().visits.size();
    schedule.slots.back().enabled = false;
    check(bb::configured(schedule).size() == centres - off, "configured: a disabled slot is left out");
}

void mark_tests() {
    const auto now = dc::Clock::now();
    dc::Detection row;
    row.id = "packet";
    row.packet = dc::Packet{};
    row.packet->frequency = 5756.5e6;
    row.packet->observed = now - std::chrono::seconds(3);
    row.timing = symbol_timing::Result{};
    row.timing->rate = 20e6;
    row.timing->observed = now - std::chrono::seconds(1);
    row.link_low_hz = 5746.5e6;
    row.link_high_hz = 5766.5e6;
    dc::RemoteIdSighting rid;
    rid.frequency = 2437e6;
    rid.observed = now;
    row.remote_id = {rid, rid};
    const auto m = bb::marks(row, now);
    check(m.size() == 3 && m[0].kind == bb::Kind::DroneId && m[1].kind == bb::Kind::RemoteId && m[2].kind == bb::Kind::Link,
          "marks: DroneID, Remote ID (repeats collapsed), link; most exact first");
    check(m[0].point() && m[0].low_hz == 5756.5e6 && near(m[0].age_s, 3, 1e-6), "marks: DroneID at its decoded channel, with its age");
    check(near(m[2].low_hz, 5738.9e6, 1) && near(m[2].high_hz, 5774.1e6, 1) && near(m[2].age_s, 1, 1e-6),
          "marks: a link spans its captures' usable range (centres 5746.5-5766.5 MHz, +-7.6 MHz)");
    const auto segs = bb::segments();
    check(bb::anchor(m, segs, 2) == &m[0] && bb::anchor(m, segs, 0) == &m[1] && bb::anchor(m, segs, 1) == nullptr,
          "anchor: the label goes to the most exact mark in each segment, none where the row has no mark");

    dc::Detection link;
    link.timing = symbol_timing::Result{};
    link.link_low_hz = link.link_high_hz = 2443.5e6;
    const auto lm = bb::marks(link, now);
    check(lm.size() == 1 && lm[0].kind == bb::Kind::Link && near(lm[0].low_hz, 2435.9e6, 1) && bb::anchor(lm, segs, 0) == &lm[0],
          "marks: a link without a rate uses 20 MSps; a link-only row's label goes to its link");

    dc::Detection guess;
    guess.universal = dc::UniversalClaim{};
    guess.universal->classifier_only = true;
    guess.universal->last_observed = now;
    guess.link_low_hz = guess.link_high_hz = 5835e6;
    const auto gm = bb::marks(guess, now);
    check(gm.size() == 1 && gm[0].kind == bb::Kind::Classifier && gm[0].point() && gm[0].low_hz == 5835e6,
          "marks: a classifier-only row at its slot centre");
    dc::Detection fallback;
    fallback.packet = dc::Packet{};
    fallback.packet->channel_hz = 2414.5e6;
    check(bb::marks(fallback, now).at(0).low_hz == 2414.5e6, "marks: DroneID without a frequency falls back to its channel");
}

void stack_tests() {
    std::vector<bb::Label> l{{100, 60}, {110, 60}, {120, 60}, {130, 60}, {400, 60}};
    bb::stack(l, 0, 1000, 4, 3);
    check(l[0].line == 0 && l[1].line == 1 && l[2].line == 2 && l[3].line == -1 && l[4].line == 0,
          "stack: overlapping labels take the next line; a fourth has no room; a distant one stays on line 0");
    check(near(l[0].x, 70, 1e-4) && near(l[4].x, 370, 1e-4), "stack: a label is centred over its mark");
    std::vector<bb::Label> edge{{5, 60}, {995, 60}};
    bb::stack(edge, 0, 1000, 4, 3);
    check(edge[0].x == 0 && near(edge[1].x, 940, 1e-4) && edge[0].line == 0 && edge[1].line == 0,
          "stack: labels near the ends are kept inside the segment");
    std::vector<bb::Label> reversed{{130, 60}, {100, 60}};
    bb::stack(reversed, 0, 1000, 4, 3);
    check(reversed[1].line == 0 && reversed[0].line == 1, "stack: placed left to right whatever the input order");
    std::vector<bb::Label> wide{{50, 300}};
    bb::stack(wide, 0, 200, 4, 1);
    check(wide[0].line == 0 && wide[0].x == 0, "stack: a label wider than its segment starts at the segment's left edge");
}
}  // namespace

int main() {
    segment_tests();
    coverage_tests();
    mark_tests();
    stack_tests();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
