// Carrier offsets (measurement only): DroneID packet offset in ppm, gathering per row across bands,
// the summary, the detection-log fields (which must not add update events) and the offset log.
#include "drones/evidence_packet.hpp"
#include "drones/evidence_log.hpp"
#include "drones/offset_log.hpp"
#include "drones/crystal_match.hpp"

#include <cmath>
#include <random>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace rfmon::drone_claims;

static void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
static bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

static nlohmann::json droneid(uint64_t capture, double centre, double channel, double cfo, Clock::time_point when,
                              bool documented = true, const std::string& serial = "5FSCK680119GT5") {
    nlohmann::json p = {
        {"protocol", "DJI DroneID"}, {"outer_crc24", true}, {"identity_readable", true},
        {"payload_version", 2}, {"model", "Mini 2"}, {"model_source", "table"}, {"reported_model", "Mini 2"},
        {"reported_serial", serial}, {"product_code", 63}, {"center_hz", centre}, {"cfo_hz", cfo},
        {"receiver_source", "LIVE / run r1 / session 1 / capture " + std::to_string(capture) + " / sample 0"},
        {"acquired_steady_s", std::chrono::duration<double>(when.time_since_epoch()).count()},
        {"channel_selection", documented ? "documented_droneid_channel" : "strongest_10MHz_per_window_not_emitter_identification"}};
    if (documented) p["channel_hz"] = channel;
    return p;
}

static void summary() {
    require(!summarize_offsets({}), "summary of nothing");
    const auto t = Clock::now();
    std::vector<OffsetSample> s{{1.20, 2897.4, 2414.5e6, t}, {1.22, 2945.7, 2414.5e6, t + std::chrono::seconds(1)},
                                {1.21, 6965.4, 5756.5e6, t + std::chrono::seconds(2)}};
    const auto out = summarize_offsets(s);
    require(out && out->all.count == 3 && near(out->all.mean_ppm, 1.21, 1e-9), "mean of all packets");
    require(near(out->all.spread_ppm, 0.01, 1e-9), "sample standard deviation");
    require(out->low.count == 2 && out->high.count == 1 && near(out->low.mean_ppm, 1.21, 1e-9), "per band");
    require(out->band_difference_ppm && near(*out->band_difference_ppm, 0.0, 1e-9), "band difference");
    require(near(out->latest_hz, 6965.4, 1e-9) && out->latest_channel_hz == 5756.5e6, "latest packet");
    require(!summarize_offsets({s[0]})->band_difference_ppm && summarize_offsets({s[0]})->all.spread_ppm == 0,
            "one packet: no spread, no difference");
    require(!offset_ppm(100, 0) && near(*offset_ppm(2414.5, 2414.5e6), 1.0, 1e-12) && !offset_ppm(NAN, 1e9),
            "ppm conversion");
}

static void ingest_and_rows() {
    const auto now = Clock::now();
    Store store;
    const auto at = now - std::chrono::seconds(2);
    // +1.2 ppm on both bands: +2897.4 Hz at 2414.5 MHz, +6907.8 Hz at 5756.5 MHz.
    const auto first = ingest_native_packet(store, droneid(1, 2413.5e6, 2414.5e6, 2897.4, at).dump(), now);
    require(first && first->second.offset_ppm && near(*first->second.offset_ppm, 1.2, 1e-6) &&
            first->second.channel_hz == 2414.5e6 && near(*first->second.offset_hz, 2897.4, 1e-9),
            "documented channel packet gives ppm");
    require(!ingest_native_packet(store, droneid(1, 2413.5e6, 2414.5e6, 2897.4, at).dump(), now),
            "a packet offered again is not new");
    const auto second = ingest_native_packet(store, droneid(2, 5758e6, 5756.5e6, 6907.8, at + std::chrono::seconds(1)).dump(), now);
    require(second && near(*second->second.offset_ppm, 1.2, 1e-6), "5.8 GHz packet ppm");
    // Found by the decoder's automatic centring: the Hz is kept, no ppm (no nominal carrier).
    const auto automatic = ingest_native_packet(store, droneid(3, 2440e6, 0, -86400, at, false).dump(), now);
    require(automatic && automatic->second.offset_hz && !automatic->second.offset_ppm &&
            automatic->second.channel_hz == 0, "automatic centring gives no ppm");
    // No cfo field: nothing measured, still a packet.
    auto bare = droneid(4, 2413.5e6, 2414.5e6, 0, at, true, "OTHER");
    bare.erase("cfo_hz");
    const auto none = ingest_native_packet(store, bare.dump(), now);
    require(none && !none->second.offset_hz && !none->second.offset_ppm, "missing cfo is not zero");

    const auto rows = detections(store, now);
    const auto& mini = rows.at("serial:5FSCK680119GT5");
    require(mini.droneid_offsets.size() == 2, "serial row gathers documented packets from both bands, got " +
                                              std::to_string(mini.droneid_offsets.size()));
    const auto s = summarize_offsets(mini.droneid_offsets);
    require(s && s->low.count == 1 && s->high.count == 1 && near(*s->band_difference_ppm, 0, 1e-6),
            "one crystal: same ppm on both bands");
    require(rows.at("serial:OTHER").droneid_offsets.empty(), "unmeasured packet adds no offset");
}

static void detection_log() {
    char dir[] = "/tmp/rfmon_offsetXXXXXX";
    require(mkdtemp(dir), "temp dir");
    const std::string path = std::string(dir) + "/detections.jsonl";
    const auto now = Clock::now();
    Store store;
    ingest_native_packet(store, droneid(1, 2413.5e6, 2414.5e6, 2897.4, now - std::chrono::seconds(3)).dump(), now);
    DetectionLog log(path);
    log.record(detections(store, now));
    // A new packet with a different offset changes the numbers, not what the row says.
    ingest_native_packet(store, droneid(2, 2413.5e6, 2414.5e6, 2950.0, now - std::chrono::seconds(1)).dump(), now);
    log.record(detections(store, now));
    std::ifstream in(path);
    std::string line;
    std::vector<nlohmann::json> events;
    while (std::getline(in, line)) events.push_back(nlohmann::json::parse(line));
    require(events.size() == 1 && events[0]["event"] == "appeared", "offset change must not log an update");
    const auto& offset = events[0]["evidence"]["droneid"]["carrier_offset"];
    require(near(offset["latest_ppm"].get<double>(), 1.2, 1e-6) && offset["all"]["packets"] == 1 &&
            offset["band_5g"].is_null() && offset["band_difference_ppm"].is_null(), "carrier_offset in the log");
    std::filesystem::remove_all(dir);
}

static void offset_log() {
    char dir[] = "/tmp/rfmon_offsetlogXXXXXX";
    require(mkdtemp(dir), "temp dir");
    const std::string path = std::string(dir) + "/offsets.jsonl";
    {
        OffsetLog log(path);
        const auto now = Clock::now();
        Store store;
        const auto added = ingest_native_packet(store, droneid(7, 2413.5e6, 2414.5e6, 2897.4, now).dump(), now);
        log.packet(added->first, added->second);
        rfmon::symbol_timing::Result r;
        r.run_id = "r1"; r.radio_session = 1; r.capture_seq = 8; r.center = 5758e6; r.rate = 20e6; r.blocks = 24;
        r.timing_group = "15kHz";
        log.link(r);  // no grid measured: nothing written
        r.grid[2] = rfmon::symbol_timing::GridOffset{4321, 15e3, .97, 18};
        log.link(r);
        log.link(r);  // the same capture offered again
        require(!log.failed(), "offset log failed");
    }
    std::ifstream in(path);
    std::string line;
    std::vector<nlohmann::json> lines;
    while (std::getline(in, line)) lines.push_back(nlohmann::json::parse(line));
    require(lines.size() == 2, "expected one packet and one link line, got " + std::to_string(lines.size()));
    require(lines[0]["kind"] == "droneid" && lines[0]["schema"] == "rfmon.drone_offset.v1" &&
            near(lines[0]["offset_ppm"].get<double>(), 1.2, 1e-6) && lines[0]["serial"] == "5FSCK680119GT5" &&
            lines[0]["capture"] == 7, "packet line");
    require(lines[1]["kind"] == "link" && lines[1]["group"] == "15kHz" && lines[1]["grid"].size() == 1 &&
            lines[1]["grid"][0]["hypothesis"] == "lte_like_15kHz" && lines[1]["grid"][0]["hz"] == 4321.0, "link line");
    std::filesystem::remove_all(dir);
}

static void crystal() {
    const auto t = Clock::now();
    const auto at = [&](int s) { return t - std::chrono::seconds(s); };
    std::vector<OffsetSample> mini{{-0.60, 0, 5756.5e6, at(100)}, {-0.62, 0, 2444.5e6, at(40)}, {-0.58, 0, 5756.5e6, at(10)}};
    require(near(*ppm_near(mini, at(10), 30), -0.60, 1e-9) && near(*ppm_near(mini, at(100), 5), -0.60, 1e-9) &&
            !ppm_near(mini, at(300)), "ppm near a time, inside the window only");
    // Residual: 2413.5 MHz x -0.60 ppm = -1448.1 Hz; a grid at 3551.9 Hz is one 5 kHz step above it.
    require(near(link_residual_hz({3551.9, 15e3, 2413.5e6, 1, t}, -0.60), 0, 1e-6) &&
            near(link_residual_hz({3551.9 + 2000, 15e3, 2413.5e6, 1, t}, -0.60), 2000, 1e-6) &&
            near(link_residual_hz({3551.9 + 3000, 15e3, 2413.5e6, 1, t}, -0.60), -2000, 1e-6),
            "link residual wraps at half a 5 kHz step");
    std::vector<LinkGridSample> grids{{3651.9, 15e3, 2413.5e6, .99, at(20)}, {3451.9, 15e3, 2413.5e6, .99, at(15)},
                                      {9999, 30e3, 2413.5e6, .99, at(12)}, {1234, 15e3, 2413.5e6, .99, at(400)}};
    const auto fit = fit_link(grids, mini);
    require(fit && fit->captures == 2 && near(fit->median_residual_hz, 100, 1e-6),
            "link fit: 15 kHz captures with DroneID nearby only");

    Detection serial, link, other, late;
    serial.droneid_offsets = mini;
    link.link_grids = {grids[0], grids[1], {3551.9, 15e3, 2413.5e6, .99, at(18)}};
    require(crystal_match(serial, link).kind == CrystalMatch::Likely &&
            crystal_match(link, serial).kind == CrystalMatch::Likely, "three fitting captures: likely, either order");
    link.link_grids.pop_back();
    require(crystal_match(serial, link).kind == CrystalMatch::Possible, "two fitting captures: possible");
    link.link_grids = {{3551.9 + 1800, 15e3, 2413.5e6, .99, at(20)}, {3551.9 + 1900, 15e3, 2413.5e6, .99, at(15)}};
    require(crystal_match(serial, link).kind == CrystalMatch::Different, "link grid far off: different");
    link.link_grids.pop_back();
    require(crystal_match(serial, link).kind == CrystalMatch::None, "one far capture decides nothing");
    {
        // Grid and link clock together: the stronger agreement wins; a conflict shows nothing.
        Detection both;
        both.link_grids = {grids[0], grids[1]};                       // grid alone: possible
        both.link_clocks = {{-0.63, 0.10, false, 5746.5e6, at(18)}};  // clock: 0.03 ppm off
        const auto m = crystal_match(serial, both);
        require(m.kind == CrystalMatch::Likely && m.reason.find("link clock agrees") != std::string::npos,
                "clock upgrades a possible grid fit: " + m.reason);
        both.link_clocks[0].ppm = 1.0;                                 // clock: 1.6 ppm off
        require(crystal_match(serial, both).kind == CrystalMatch::None, "possible grid, different clock: nothing");
        both.link_grids.push_back({3551.9, 15e3, 2413.5e6, .99, at(18)});  // grid alone: likely
        both.link_clocks[0] = {-0.70, 0.40, false, 5746.5e6, at(18)};  // clock: possible
        const auto g = crystal_match(serial, both);
        require(g.kind == CrystalMatch::Likely && g.reason.find("link grid fits") == 0, "likely grid beats possible clock");
        both.link_grids = {{3551.9 + 1800, 15e3, 2413.5e6, .99, at(20)}, {3551.9 + 1900, 15e3, 2413.5e6, .99, at(15)}};
        both.link_clocks[0] = {-0.62, 0.10, false, 5746.5e6, at(18)};
        require(crystal_match(serial, both).kind == CrystalMatch::None, "different grid, agreeing clock: nothing");
        both.link_clocks.clear();
        require(crystal_match(serial, both).kind == CrystalMatch::Different, "different grid alone still different");
    }
    other.droneid_offsets = {{-0.80, 0, 2414.5e6, at(12)}};
    require(crystal_match(serial, other).kind == CrystalMatch::None, "0.2 ppm apart: undecided");
    other.droneid_offsets[0].ppm = -1.0;
    require(crystal_match(serial, other).kind == CrystalMatch::Different, "0.4 ppm apart: different");
    other.droneid_offsets[0].ppm = -0.65;
    require(crystal_match(serial, other).kind == CrystalMatch::Likely, "0.05 ppm apart: likely");
    late.droneid_offsets = {{-0.60, 0, 2414.5e6, t + std::chrono::seconds(300)}};
    require(crystal_match(serial, late).kind == CrystalMatch::None, "no packets close in time: undecided");

    // detections() gathers the grids of a link's current captures.
    Store store;
    for (uint64_t c = 1; c <= 3; ++c) {
        rfmon::symbol_timing::Result r;
        r.run_id = "r1"; r.radio_session = 1; r.capture_seq = c; r.center = 2413.5e6;
        r.timing_group = "15kHz"; r.observed = t - std::chrono::seconds(4 * c);
        r.grid[2] = rfmon::symbol_timing::GridOffset{3500. + c, 15e3, c == 3 ? .5 : .99, 24};
        store.timing(r);
    }
    const auto rows = detections(store, t);
    require(rows.size() == 1, "one link row");
    const auto& grid_row = rows.begin()->second;
    require(grid_row.link_grids.size() == 2 && grid_row.link_grids[0].centre_hz == 2413.5e6,
            "link row carries its captures' grids, low-coherence ones left out (got " +
                std::to_string(grid_row.link_grids.size()) + ")");

    // A "Lightbridge" link whose link clocks disagree far beyond their errors is dropped; one
    // whose clocks agree (the Phantom 4's, live) is kept.
    require(near(*link_clock_scatter({{-4, 1, true, 0, t}, {-6, 1, true, 0, t}}), 2, 1e-9) &&
            !link_clock_scatter({{-4, 1, true, 0, t}}), "clock scatter");
    for (const bool wild : {false, true}) {
        Store lb;
        const double phantom[] = {-4.6, -3.6, -4.2, -5.4}, wifi[] = {6.5, -13.4, 11.5, 0.7};
        for (uint64_t c = 1; c <= 4; ++c) {
            rfmon::symbol_timing::Result r;
            r.run_id = "r2"; r.radio_session = 1; r.capture_seq = c; r.center = 5778e6;
            r.timing_group = "32.8kHz"; r.observed = t - std::chrono::seconds(3 * c);
            r.link_clock = rfmon::link_clock::Estimate{rfmon::link_clock::Link::lightbridge,
                                                       (wild ? wifi : phantom)[c - 1], 1.2, 24};
            lb.timing(r);
        }
        const auto lb_rows = detections(lb, t);
        require(lb_rows.size() == (wild ? 0u : 1u), wild ? "Lightbridge row with wild clocks kept"
                                                         : "Lightbridge row with agreeing clocks dropped");
    }
}

// Live 21 Nov 2026 cases: outlier readings, and when a Lightbridge link is confirmed.
static void clock_filters() {
    const auto t = Clock::now();
    const auto ocu = [&](double ppm, double sigma) { return LinkClockSample{ppm, sigma, false, 2.41e9, t}; };
    const auto lb = [&](double ppm, double sigma) { return LinkClockSample{ppm, sigma, true, 2.44e9, t}; };
    // The Mini 2 with one capture that had its link in 11 of 24 blocks.
    std::vector<LinkClockSample> mini{ocu(-0.40, .10), ocu(-0.44, .10), ocu(-0.48, .10), ocu(-0.62, .07),
                                      ocu(-11.93, .11), ocu(-0.55, .10)};
    const auto kept = without_clock_outliers(mini);
    require(kept.size() == 5 && std::none_of(kept.begin(), kept.end(), [](const LinkClockSample& s) { return s.ppm < -5; }),
            "OcuSync outlier kept");
    // The Phantom's live readings scatter about 2.5 ppm: all stay.
    std::vector<LinkClockSample> phantom{lb(-5.06, .58), lb(-4.96, 1.0), lb(-2.31, .62), lb(-7.23, 1.13), lb(-1.85, 1.56),
                                         lb(-8.01, 1.66), lb(-3.36, .76), lb(-4.99, .5)};
    require(without_clock_outliers(phantom).size() == phantom.size(), "Phantom reading dropped as outlier");
    require(without_clock_outliers({ocu(-0.4, .1), ocu(-11.9, .1), ocu(-0.5, .1), ocu(-0.45, .1)}).size() == 4,
            "four readings: too few to judge outliers by");
    // Confirmation: three usable readings within +-20 ppm that agree.
    require(lightbridge_link_confirmed({lb(-5.06, .58), lb(-4.96, 1.0), lb(-2.31, .62)}), "three agreeing Phantom readings not confirmed");
    require(!lightbridge_link_confirmed({lb(-5.06, .58), lb(-4.96, 1.0)}), "two readings confirmed");
    require(!lightbridge_link_confirmed({lb(-4.0, 1.0)}), "one reading confirmed");
    // Live, Mini 2 only: three scattered readings; two of them agree by chance.
    require(!lightbridge_link_confirmed({lb(10.94, 1.23), lb(-2.42, 1.82), lb(-3.09, 1.21)}), "chance pair confirmed");
    require(!lightbridge_link_confirmed({}), "no reading confirmed");
    require(!lightbridge_link_confirmed({lb(-30.41, 1.09), lb(-28.77, 1.40)}), "readings beyond 20 ppm confirmed");
    // The 2.4 GHz captures the probe called Lightbridge with only the Mini 2 on.
    require(!lightbridge_link_confirmed({lb(-30.41, 1.09), lb(-0.26, 1.80), lb(2.97, 1.09), lb(-21.82, .99),
                                         lb(10.75, 1.95), lb(-28.77, 1.40)}), "scattered readings confirmed");
    require(lightbridge_link_confirmed(phantom), "Phantom's live readings not confirmed");
    auto with_wild = phantom;
    with_wild.push_back(lb(-30.0, 1.0));
    require(lightbridge_link_confirmed(with_wild), "one wild reading hid the Phantom");
}

// Frame timing: synthetic Lightbridge captures whose symbol grid moves at a known clock.
// OcuSync frame timing (23 Nov 2026): the same fit and row checks at the 15 and 30 kHz links'
// symbol periods, with their tighter noise and a link heard only every 5-40 s (the Mini 2 live).
static void ocusync_frame_timing() {
    namespace ft = rfmon::frame_timing;
    std::mt19937 rng(11);
    std::normal_distribution<double> noise(0, 0.3e-6);
    const auto sample = [&](double t, double pos0, double ppm, double P) {
        const double pos = pos0 - ppm * 1e-6 * t + noise(rng);
        double phase = std::fmod(pos - std::fmod(t, P), P);
        if (phase < 0) phase += P;
        return ft::Sample{"r", 1, t, phase, P};
    };
    const double gaps[] = {8, 23, 12, 31, 17, 9, 26, 14, 35, 11};
    const auto captures = [&](double pos0, double ppm, double P, std::size_t n, double t0 = 5000) {
        std::vector<ft::Sample> out;
        double t = t0;
        for (std::size_t k = 0; k < n; ++k) { out.push_back(sample(t, pos0, ppm, P)); t += gaps[k % 10]; }
        return out;
    };
    for (const double P : {rfmon::link_clock::ocusync_15k_symbol_s, rfmon::link_clock::ocusync_30k_symbol_s}) {
        const auto lt = ft::timing_for(P);
        const auto enough = captures(10e-6, -0.6, P, lt.min_captures);
        const auto f = ft::fit(enough, -0.5, 0.3);
        require(f && f->good() && std::abs(f->ppm + 0.6) < 0.05,
                "OcuSync frame timing not fitted at " + std::to_string(P * 1e6) + " us");
        std::cout << "OcuSync " << P * 1e6 << " us symbols, " << lt.min_captures << " captures over " << f->span_s
                  << " s: " << f->ppm << " +- " << f->sigma_ppm << " ppm, rms " << f->rms_s * 1e6 << " us\n";
        const auto few = ft::fit({enough.begin(), enough.end() - 1}, -0.5, 0.3);
        require(!few || !few->good(), "OcuSync frame timing good below its minimum captures");
    }
    // Rows. A and B: one 15 kHz transmitter split into two rows (alternate captures). C: another
    // unit of the same model with the same crystal offset but its own frame timing, same band.
    // D: a 5 us offset (too close to tell apart). L: a Lightbridge row.
    const double P = rfmon::link_clock::ocusync_15k_symbol_s;
    const auto one = captures(10e-6, -0.6, P, 10), two = captures(40e-6, -0.6, P, 10, 5003),
               near = captures(15e-6, -0.6, P, 10, 5005);
    Detection a, b, c, d, l;
    a.band = b.band = c.band = d.band = l.band = "5 GHz";
    for (std::size_t i = 0; i < one.size(); ++i) {
        auto& row = i % 2 ? b : a;
        row.frame_samples.push_back(one[i]);
        row.link_clocks.push_back({-0.6 + (int(i % 3) - 1) * 0.1, 0.1, false, 5.76e9, Clock::now()});
    }
    for (const auto& s : two) {
        c.frame_samples.push_back(s);
        c.link_clocks.push_back({-0.6, 0.1, false, 5.78e9, Clock::now()});
    }
    for (const auto& s : near) {
        d.frame_samples.push_back(s);
        d.link_clocks.push_back({-0.6, 0.1, false, 5.78e9, Clock::now()});
    }
    const auto same = crystal_match(a, b), other = crystal_match(a, c), close = crystal_match(a, d);
    std::cout << "OcuSync same link: " << same.reason << "\nOcuSync other unit, same clock: " << other.reason
              << "\nOcuSync 5 us apart: " << close.reason << '\n';
    require(same.kind == CrystalMatch::Likely && same.reason.find("frame timing lines up") == 0, "OcuSync rows not lined up");
    require(other.kind == CrystalMatch::Different && other.reason.find("frame timing does not line up") == 0,
            "two OcuSync units with one clock not told apart");
    require(close.kind != CrystalMatch::Likely, "OcuSync rows 5 us apart called the same");
    // A DroneID row whose link has frame timing joins a link-only row that lines up with it.
    Detection p = a;
    p.droneid_offsets.push_back({-0.6, -0.6e-6 * 5.7565e9, 5.7565e9, Clock::now()});
    const auto with_packet = crystal_match(p, b);
    require(with_packet.kind == CrystalMatch::Likely && with_packet.reason.find("frame timing") == 0,
            "DroneID row and its link not joined by frame timing: " + with_packet.reason);
    // Different link types are never compared on frame timing.
    for (double t = 5000; t < 5100; t += 7) l.frame_samples.push_back({"r", 1, t, 20e-6, ft::symbol_s});
    require(compare_rows_frame_timing(a, l).kind == CrystalMatch::None, "OcuSync compared with Lightbridge timing");
}

static void frame_timing_chain() {
    namespace ft = rfmon::frame_timing;
    const double P = ft::symbol_s;
    std::mt19937 rng(5);
    std::normal_distribution<double> noise(0, 2.5e-6);
    // A capture at device time t of a link whose grid sat at pos0 at t=0 and runs at ppm.
    const auto sample = [&](double t, double pos0, double ppm, std::uint64_t session = 1) {
        const double pos = pos0 - ppm * 1e-6 * t + noise(rng);
        double phase = std::fmod(pos - std::fmod(t, P), P);
        if (phase < 0) phase += P;
        return ft::Sample{"r", session, t, phase};
    };
    // Live cadence (21 Nov 2026): a Lightbridge capture at 2.4 GHz every 1-3 s, now and then a
    // 9 s gap, over 45 s, from device time 1000 s.
    std::vector<double> times;
    for (double t = 1000; t < 1045;) {
        times.push_back(t);
        t += times.size() % 7 == 0 ? 9.0 : 1.0 + (times.size() % 3);
    }
    std::vector<ft::Sample> phantom, other;
    for (double t : times) phantom.push_back(sample(t, 20e-6, -4.0));
    for (double t : times) other.push_back(sample(t + 0.4, 61e-6, -3.6));
    const auto f = ft::fit(phantom, -5.0, 2.5);  // a poor prior, as from single captures
    require(f && f->connected == times.size() && f->good() && std::abs(f->ppm + 4.0) < 0.1,
            "frame timing: " + (f ? std::to_string(f->connected) + " connected, " + std::to_string(f->ppm) + " ppm" : "no fit"));
    std::cout << "frame timing: " << f->connected << " captures, " << f->ppm << " +- " << f->sigma_ppm << " ppm, rms "
              << f->rms_s * 1e6 << " us\n";
    auto both = phantom;
    both.insert(both.end(), other.begin(), other.end());
    const auto u = ft::fit(both, -3.8, 0.5);
    require(u && !u->good() && u->connected < u->used, "two links fitted as one: " +
            (u ? std::to_string(u->connected) + " of " + std::to_string(u->used) + " on the line" : "no fit"));
    {
        // Sparse bursts (5 captures in 3.6 s, 26 s apart; 3 in 1.8 s, 40 s apart) with a poor
        // prior, over many noise draws: a gap is bridged only when the line can predict across it,
        // and whatever is returned as good has the right clock.
        int bridged = 0, wrong = 0;
        for (int draw = 0; draw < 200; ++draw)
            for (const auto& [per, gap] : {std::pair{5, 26.0}, std::pair{3, 40.0}}) {
                std::vector<ft::Sample> sparse;
                for (int pass = 0; pass < 4; ++pass)
                    for (int k = 0; k < per; ++k) sparse.push_back(sample(2000 + pass * gap + k * 0.9, 20e-6, -4.0));
                const auto fsp = ft::fit(sparse, -5.0, 1.5);
                if (fsp && fsp->connected > std::size_t(per)) ++bridged;
                if (fsp && fsp->good() && std::abs(fsp->ppm + 4.0) > 0.3) ++wrong;
            }
        std::cout << "sparse bursts: " << bridged << " of 400 fits spanned more than one burst, " << wrong << " wrong clocks\n";
        require(wrong == 0, "sparse bursts gave a wrong clock");
    }
    {
        // Live run4 (21 Nov 2026, 17:25): the Phantom seen 14 times over 85 s, 0.7-13.4 s apart.
        // The earlier chaining broke after 2 captures; the slope search fits them all.
        const double gaps[] = {0.7, 11.6, 8.3, 5.1, 8.8, 1.8, 13.4, 8.1, 1.8, 1.3, 12.6, 7.7, 3.8};
        int good = 0, wrong = 0;
        for (int draw = 0; draw < 100; ++draw) {
            std::vector<ft::Sample> live;
            double t = 3000;
            live.push_back(sample(t, 30e-6, -4.07));
            for (double g : gaps) live.push_back(sample(t += g, 30e-6, -4.07));
            const auto fl = ft::fit(live, -4.48, 1.5);  // the live single-capture mean as prior
            if (fl && fl->good()) { ++good; wrong += std::abs(fl->ppm + 4.07) > 0.3; }
        }
        std::cout << "run4 cadence: " << good << " of 100 good, " << wrong << " wrong\n";
        require(good >= 90 && wrong == 0, "run4 cadence not fitted");
    }
    // A reconnect restarts the device clock: only the larger session is chained.
    auto split = phantom;
    for (int k = 0; k < 3; ++k) split.push_back(sample(5.0 + k, 0, -4.0, 2));
    const auto fs = ft::fit(split, -4.0, 1.0);
    require(fs && fs->used == phantom.size(), "sessions mixed");
    // Too few captures or too short a span: not good.
    const auto short_fit = ft::fit({phantom.begin(), phantom.begin() + 3}, -4.0, 1.0);
    require(short_fit && !short_fit->good(), "three captures over 2 s called good");

    // Rows: two Lightbridge rows of one link (centres too far apart to cluster) line up; a row of
    // another link at a similar clock in the same band does not.
    Detection a, b, c;
    a.band = b.band = c.band = "2.4 GHz";
    for (std::size_t i = 0; i < phantom.size(); ++i) {
        auto& row = i % 2 ? b : a;
        row.frame_samples.push_back(phantom[i]);
        row.link_clocks.push_back({-4.0 + (int(i % 3) - 1) * 1.5, 1.0, true, 2.43e9, Clock::now()});
    }
    for (std::size_t i = 0; i < other.size(); ++i) {
        c.frame_samples.push_back(other[i]);
        c.link_clocks.push_back({-3.6 + (int(i % 3) - 1) * 1.5, 1.0, true, 2.47e9, Clock::now()});
    }
    const auto same = crystal_match(a, b), diff = crystal_match(a, c);
    std::cout << "same link: " << same.reason << "\nother link: " << diff.reason << '\n';
    require(same.kind == CrystalMatch::Likely && same.reason.find("frame timing lines up") == 0, "same link not lined up");
    require(diff.kind == CrystalMatch::Different && diff.reason.find("frame timing does not line up") == 0,
            "other link not told apart");
    c.band = "5 GHz";
    require(crystal_match(a, c).kind != CrystalMatch::Likely, "other link on another band joined");
    const auto rc = row_clock(a);
    require(rc && rc->frame && std::abs(rc->ppm + 4.0) < 0.15, "row clock not from frame timing");
}

int main() try {
    summary();
    crystal();
    frame_timing_chain();
    ocusync_frame_timing();
    clock_filters();
    ingest_and_rows();
    detection_log();
    offset_log();
    std::cout << "carrier offset tests passed\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
