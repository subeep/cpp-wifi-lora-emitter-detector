// Coordinated Drones-page section, offscreen (upgradedplan.md P2): renders the
// production draw_section() against a production Scanner running coordinated
// mode on a fake radio, and checks the combined-entry builder keeps evidence
// levels and coverage-aware freshness distinct. No receiver is used.
#include "coordinated_panel.hpp"
#include "drones/crystal_match.hpp"
#include "drones_page.hpp"
#include "gui/app_shell.hpp"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace rfmon;
namespace rfmon {
struct ScannerTestAccess {
    static void set_device_factory(Scanner& s, decltype(Scanner::device_factory_) f) { s.device_factory_ = std::move(f); }
};
}  // namespace rfmon

namespace {
void require(bool value, const std::string& why) {
    if (!value) throw std::runtime_error(why);
    std::cout << "PASS " << why << '\n';
}
class QuietRadio : public CaptureDevice {
public:
    CaptureResult capture_detailed(double center, double rate, double duration, double settle, bool retune) override {
        std::this_thread::sleep_for(std::chrono::duration<double>(settle + duration));
        CaptureResult r;
        r.sample_rate_hz = rate;
        r.samples.resize(size_t(rate * duration));
        std::normal_distribution<float> g(0.f, 1e-3f);
        for (auto& x : r.samples) x = {g(rng_), g(rng_)};
        r.timing.device_time_valid = true;
        r.timing.device_time_ns = (ns_ += 1000000000);
        r.timing.requested_samples = r.samples.size();
        r.timing.actual_rf_hz = center;
        r.timing.retuned = retune;
        return r;
    }
    void set_gain(std::optional<double>) override {}
private:
    std::mt19937 rng_{3};
    int64_t ns_ = 0;
};

std::vector<rfmon::evidence::Entry> g_entries;
std::deque<coord::VisitRecord> g_visits;

void entry_tests() {
    const double now = Scanner::now_steady_s();
    const auto at = [](double s) {
        return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(s)));
    };
    drone::Snapshot native, legacy;
    drone::Observation o;
    o.protocol = "DJI DroneID";
    o.evidence = "CRC-valid packet";
    o.serial = "SYNTHETIC123";
    o.model = "Mini 2";
    o.confirmed = true;
    o.source = "LIVE / run r1 / session 1 / capture 7 / sample 0";
    o.timestamp = 12.25;
    o.frequency = 2444.5e6;
    o.acquired_at = at(now - 30);
    o.count = 3;
    native.observations.push_back(o);
    o.model = "Mini 3";  // same claimed serial, different claimed model: a conflict to surface
    o.source = "LIVE / run r1 / session 1 / capture 3 / sample 0";
    o.acquired_at = at(now - 40);
    legacy.observations.push_back(o);
    drone::RidJson rid = {{"tracks", drone::RidJson::array({{{"transport", "Beacon"}, {"protocol_version", 2}, {"transmitter", "aa:bb:cc:dd:ee:ff"},
        {"packs", 5}, {"messages", drone::RidJson::array({{{"type", "Basic ID"}, {"reported_id", "SYNTHETIC123"}, {"ua_type_name", "Helicopter / multirotor"},
        {"age_s", 2.0}, {"source", {{"channel_hz", 2437e6}, {"capture_seq", 9}, {"device_time_ns", 20000000000}}}}})}}})}};
    ml::Snapshot model;
    ml::Result r;
    r.candidate = "DJI_Mini_2";
    r.center = 5181.5e6;
    r.completed = at(now - 1);
    r.capture = 7;  // the model analysed the same capture the DJI packet came from
    model.history.push_back(r);
    std::deque<coord::VisitRecord> visits;
    coord::VisitRecord v;
    v.center_hz = 2443.5e6;
    v.usable_half_span_hz = coord::usable_half_span(20e6);
    v.start_s = now - 20;
    v.end_s = now - 19.4;
    v.valid = v.analysed = true;
    visits.push_back(v);  // covered the DJI entry after it was seen: not reacquired
    v.center_hz = 2430e6;  // covers 2437 MHz, not 2444.5 MHz (+/-8.8 MHz usable)
    v.start_s = now - 0.2;
    v.in_progress = true;
    visits.push_back(v);  // covering the Remote ID entry right now
    const auto entries = coordinated::build_entries(native, legacy, rid, model, visits, now, 60);
    require(entries.size() == 4, "One row per detector observation; no merged drone claim");
    require(entries[0].source == "Model (diagnostic)" && entries[1].source == "Wi-Fi Remote ID" && entries[2].source == "DJI native" &&
                entries[3].source == "DJI fallback",
            "Rows sorted newest first");
    require(entries[0].identity.find("not accepted") != std::string::npos &&
                entries[1].identity.find("unauthenticated") != std::string::npos &&
                entries[2].identity.find("unauthenticated") != std::string::npos,
            "Identity claims keep their evidence status");
    require(entries[1].freshness == coord::Freshness::ReceivingNow, "Remote ID entry on the frequency being observed is receiving now");
    require(entries[2].freshness == coord::Freshness::NotReacquired, "DJI entry covered by a later valid visit is not reacquired");
    require(entries[0].freshness == coord::Freshness::RecentBandUnobserved, "Model entry with no later 5 GHz visit stays recent");
    require(std::abs(entries[1].frequency_hz - 2437e6) < 1 && entries[1].count == 5, "Remote ID frequency/count from frame provenance");
    require(entries[2].capture_seq == 7u && entries[3].capture_seq == 3u && entries[1].capture_seq == 9u && entries[0].capture_seq == 7u,
            "Each row keeps the capture its latest evidence came from");
    require(entries[2].event_device_time_s == 12.25 && entries[1].event_device_time_s && std::abs(*entries[1].event_device_time_s - 20) < 1e-9,
            "Evidence device times kept for IQ span export");
    std::set<std::string> keys;
    for (const auto& e : entries) keys.insert(e.key);
    require(keys.size() == entries.size(), "Row keys are unique (stable inspector selection)");
    auto has = [](const std::vector<std::string>& v, const std::string& text) {
        return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.find(text) != std::string::npos; });
    };
    require(has(entries[2].relations, "#2 Wi-Fi Remote ID: same reported identifier") &&
                has(entries[2].relations, "#4 DJI fallback: same reported identifier") &&
                has(entries[1].relations, "#3 DJI native: same reported identifier"),
            "Same reported identifier relates rows across detectors, stated as unauthenticated claims");
    require(has(entries[0].relations, "#3 DJI native: evidence from the same capture #7") &&
                has(entries[2].relations, "#1 Model (diagnostic): evidence from the same capture #7"),
            "Model and DJI evidence from the same capture are related as not independent");
    require(has(entries[2].conflicts, "reported model differs: 'Mini 2'") && has(entries[3].conflicts, "'Mini 3'") &&
                entries[1].conflicts.empty(),
            "Conflicting reported models stay visible on both rows");
    require(entries[0].relations.size() == 1, "No relationship from frequency, product name or timing alone");
    {
        // Native worker output as the detector stores it (boost ptree JSON:
        // every value a string), O4 evidence and Bluetooth Remote ID.
        drone::Snapshot n2 = native;
        const std::string acquired = std::to_string(now - 5);
        auto pkt = [&](const std::string& serial, const std::string& env, const std::string& capture) {
            return std::string("{\"center_hz\":\"5745000000.0\",\"envelope\":\"") + env + "\",\"inner_crc16\":\"true\",\"outer_crc24\":\"true\"," +
                   "\"reported_serial\":\"" + serial + "\",\"reported_model\":\"\",\"telemetry_parsed\":\"" + (serial.empty() ? "false" : "true") +
                   "\",\"timestamp\":\"0.1\",\"receiver_source\":\"LIVE / run r1 / session 1 / capture " + capture +
                   " / sample 0\",\"acquired_steady_s\":\"" + acquired + "\"}";
        };
        n2.native_packets = {pkt("3YTBJ3Q0030J2X", "v2", "11"), pkt("", "infp", "12"), pkt("3YTBJ3Q0030J2X", "v2", "13")};
        n2.o4_windows = 40;
        n2.o4_candidates = 3;
        n2.o4_latest = "{\"candidate\":\"true\",\"reason\":\"candidate\",\"center_hz\":\"5760000000\",\"receiver_source\":\"LIVE / run r1 / session 1 / capture 13 / sample 0\",\"acquired_steady_s\":\"" + acquired + "\"}";
        rfmon::shortrange::Snapshot ble;
        rfmon::shortrange::BleDevice d;
        d.key = "C0:FF:EE:12:34:56 (random)";
        d.address = "C0:FF:EE:12:34:56";
        d.address_kind = "Random static";
        d.packets = 53;
        d.channel_counts = {{38, 41}, {39, 8}};
        d.last_seen = at(now - 3);
        d.last_source = "LIVE / run r1 / capture 14 / 2423.5 MHz";
        d.remote_id["Basic ID / id type 1"] = R"({"type":"Basic ID","reported_id":"SYNTHETIC123","ua_type_name":"Helicopter / multirotor"})";
        d.remote_id["Location"] = R"({"type":"Location","latitude":12.3456789,"longitude":76.54321})";
        ble.ble.push_back(d);
        const auto all = coordinated::build_entries(n2, legacy, rid, model, visits, now, 60, &ble);
        auto find = [&](const std::string& source, const std::string& text) {
            return std::find_if(all.begin(), all.end(), [&](const coordinated::Entry& e) { return e.source == source && (e.identity + e.evidence).find(text) != std::string::npos; });
        };
        const auto air2s = find("DJI native packets", "3YTBJ3Q0030J2X"), infp = find("DJI native packets", "infp");
        const auto o4 = find("DJI native O4 structure", "3 candidate windows of 40"), bt = find("Bluetooth Remote ID", "SYNTHETIC123");
        require(all.size() == entries.size() + 4, "Native packets (2 rows), O4 and Bluetooth Remote ID are listed");
        require(air2s != all.end() && air2s->count == 2 && air2s->capture_seq == 13u && std::abs(air2s->frequency_hz - 5745e6) < 1 &&
                    std::abs(air2s->last_seen_s - (now - 5)) < 0.01 && air2s->evidence.find("CRC16 OK") != std::string::npos,
                "Native packets grouped by reported serial with count, latest capture, centre and acquisition time");
        require(infp != all.end() && infp->identity.find("no serial") != std::string::npos && infp->count == 1,
                "Packet without a serial keeps its own row and says identity was not decoded");
        require(o4 != all.end() && o4->identity.find("no identity") != std::string::npos && o4->count == 3,
                "O4 structure evidence listed as evidence only");
        require(bt != all.end() && bt->count == 53 && std::abs(bt->frequency_hz - 2426e6) < 1 && bt->capture_seq == 14u &&
                    has(bt->relations, "Wi-Fi Remote ID: same reported identifier"),
                "Bluetooth Remote ID row relates to the Wi-Fi Remote ID row with the same reported ID");
    }
    g_entries = entries;
    g_visits = visits;
    g_visits.front().capture_seq = 7;  // the DJI evidence's own visit
    require(rfmon::evidence::capture_from_source("LIVE / run x / session 2 / capture 4711 / sample 64") == 4711u &&
                !rfmon::evidence::capture_from_source("OFFLINE / file / sample 0"),
            "Capture reference parsed from the detector source string");
}

void export_tests() {
    namespace fs = std::filesystem;
    char tmp[] = "/tmp/rfmon-evidence-XXXXXX";
    require(mkdtemp(tmp) != nullptr, "Temporary export root");
    rfmon::evidence::Entry e;
    e.source = "DJI native";
    e.identity = "reported serial X (unauthenticated)";
    e.frequency_hz = 2444.5e6;
    e.capture_seq = 7;
    e.event_device_time_s = 5.1;  // 100,000 samples into the capture below
    e.raw_json = R"({"packet":{"serial":"X"}})";
    e.relations = {"#2 Wi-Fi Remote ID: same reported identifier"};
    Scanner::RetainedCapture c;
    c.record.capture_seq = 7;
    c.record.run_id = "run-1";
    c.record.sample_rate_hz = 1e6;
    c.record.capture_center_hz = 2443.5e6;
    c.record.clock = wifi_security::ClockDomain::UsrpDevice;
    c.record.device_time_ns = 5000000000;
    auto iq = std::make_shared<std::vector<std::complex<float>>>(200000);
    for (size_t i = 0; i < iq->size(); ++i) (*iq)[i] = {float(i), -float(i)};
    c.iq = iq;
    std::deque<coord::VisitRecord> visits;
    coord::VisitRecord v;
    v.capture_seq = 7;
    v.center_hz = 2443.5e6;
    v.usable_half_span_hz = coord::usable_half_span(20e6);
    visits.push_back(v);
    const auto dir = rfmon::evidence::export_entry(tmp, e, visits, "hash123", c, Scanner::now_steady_s());
    nlohmann::json j;
    std::ifstream(fs::path(dir) / "evidence.json") >> j;
    require(j["schema"] == "rfmon-drone-evidence" && j["schedule_hash"] == "hash123" && j["entry"]["capture_seq"] == 7 &&
                j["entry"]["raw"]["packet"]["serial"] == "X" && j["entry"]["relations"].size() == 1 && j["visits_covering_frequency"].size() == 1,
            "Evidence manifest holds entry, raw output, relations, visit and schedule identity");
    require(j["iq"]["exported_first_sample"] == 50000 && j["iq"]["exported_samples"] == 100000 && j["iq"]["capture_seq"] == 7 &&
                j["iq"]["first_exported_device_time_ns"] == 5050000000LL,
            "IQ span is +/-50 ms around the evidence time, with its device-time anchor");
    std::ifstream f(fs::path(dir) / "iq.cf32_le", std::ios::binary);
    std::vector<float> head(2);
    f.read(reinterpret_cast<char*>(head.data()), 8);
    require(fs::file_size(fs::path(dir) / "iq.cf32_le") == 100000 * 8 && head[0] == 50000.f && head[1] == -50000.f,
            "IQ file is the exact sample span, interleaved float32");
    const auto again = rfmon::evidence::export_entry(tmp, e, visits, "hash123", c, Scanner::now_steady_s());
    require(again != dir && fs::exists(fs::path(dir) / "iq.cf32_le"), "A second export never overwrites the first");
    const auto meta_only = rfmon::evidence::export_entry(tmp, e, visits, "hash123", std::nullopt, Scanner::now_steady_s());
    nlohmann::json m;
    std::ifstream(fs::path(meta_only) / "evidence.json") >> m;
    require(m["iq"].is_null() && m["iq_unavailable_reason"].get<std::string>().find("no longer retained") != std::string::npos &&
                !fs::exists(fs::path(meta_only) / "iq.cf32_le"),
            "Without a retained capture the export is metadata only and says why");
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// The Drones page (drones_page.hpp) over example rows: ordering and filters, every state of
// the status strip and table, the details panel, the alert, and each tab.
void drones_page_tests(Scanner& scanner, drone::Addon& drones, ml::Inference& model, coordinated::PanelState& cp,
                       std::string& text) {
    using drone_claims::Detection;
    using drone_claims::Tier;
    const auto now = drone_claims::Clock::now();
    std::map<std::string, Detection> rows;
    {
        Detection d;
        d.id = "pkt"; d.band = "5.8 GHz"; d.group = "30kHz";
        symbol_timing::Result t;
        t.observed = now - std::chrono::seconds(2);
        t.timing_group = "30kHz";
        t.grid[3] = symbol_timing::GridOffset{12345.6, 30e3, .96, 20};
        d.timing = t;
        d.link_low_hz = d.link_high_hz = 5756.5e6;
        d.droneid_offsets = {{1.20, 2897.4, 2414.5e6, now - std::chrono::seconds(9)},
                             {1.22, 2945.7, 2414.5e6, now - std::chrono::seconds(5)},
                             {1.21, 6965.4, 5756.5e6, now - std::chrono::seconds(1)}};
        drone_claims::Packet p;
        p.model = "DJI Air 3S"; p.serial = "1581F6Z0TEST"; p.readable = true; p.version = 2;
        p.observed = now - std::chrono::seconds(1); p.frequency = 5756.5e6;
        p.positions.aircraft = drone_claims::reported_position(10.0004, 20.0004, p.observed, "DroneID");
        p.positions.pilot = drone_claims::reported_position(10.0, 20.0, p.observed, "DroneID");
        d.packet = p;
        drone_claims::UniversalClaim check;
        check.type = "dji_ofdm"; check.captures = check.typed = check.unnamed = check.beyond_distance = 2;
        check.guesses = {{"Air 3S", .71}, {"Air 2S", .20}};
        check.last_score = .71; check.score_limit = .4957; check.last_distance = 22.0; check.distance_limit = 15.951;
        d.classifier_check = check;
        rows[d.id] = d;
    }
    {
        Detection d;
        d.id = "rid:x"; d.band = "2.4 GHz";
        drone_claims::RemoteIdSighting r;
        r.track = "x"; r.reported_id = "1596F3A2TEST"; r.ua_type = "Multirotor"; r.transport = "Wi-Fi beacon";
        r.observed = now - std::chrono::seconds(4); r.frequency = 2437e6;
        d.remote_id.push_back(r);
        rows[d.id] = d;
    }
    {
        Detection d;
        d.id = "link"; d.band = "5.8 GHz"; d.group = "15kHz";
        symbol_timing::Result t;
        t.observed = now - std::chrono::seconds(9);
        d.timing = t;
        d.link_low_hz = 5785e6; d.link_high_hz = 5805e6;
        drone_claims::UniversalClaim u;
        u.type = "dji_ofdm"; u.captures = u.typed = u.unnamed = u.below_score = 3;
        u.guesses = {{"Mini 2", .42}, {"Mini 3", .30}, {"Air 2S", .10}};
        u.last_score = .44; u.score_limit = .4957; u.last_distance = 9.5; u.distance_limit = 15.951;
        d.universal = u;
        rows[d.id] = d;
    }
    {
        Detection d;
        d.id = "clf"; d.band = "5.8 GHz";
        drone_claims::UniversalClaim u;
        u.type = "dji_ofdm"; u.classifier_only = true; u.typed = u.captures = 2;
        u.first_observed = now - std::chrono::seconds(40); u.last_observed = now - std::chrono::seconds(25);
        d.universal = u;
        d.link_low_hz = d.link_high_hz = 5835e6;
        rows[d.id] = d;
    }
    const auto ordered = drones_page::filter_rows(rows, -1, now);
    require(ordered.size() == 4 && ordered[0]->id == "pkt" && ordered[1]->id == "link" && ordered[2]->id == "rid:x" &&
                ordered[3]->id == "clf",
            "Drones page: strongest level first (Confirmed, Family, Reported, Classifier)");
    const auto reported = drones_page::filter_rows(rows, int(Tier::Reported), now);
    require(reported.size() == 1 && reported[0]->id == "rid:x", "Drones page: the level filter keeps one level");
    const auto counts = drones_page::tier_counts(rows);
    require(counts.at(int(Tier::Confirmed)) == 1 && counts.at(int(Tier::Family)) == 1 &&
                counts.at(int(Tier::Reported)) == 1 && counts.at(int(Tier::Classifier)) == 1,
            "Drones page: rows counted per level");
    require(drones_page::evidence_kinds(rows["pkt"]) == std::vector<std::string>{"Link", "DroneID"} &&
                drones_page::evidence_kinds(rows["rid:x"]) == std::vector<std::string>{"Remote ID"} &&
                drones_page::evidence_kinds(rows["clf"]) == std::vector<std::string>{"Classifier"},
            "Drones page: evidence kinds per row");

    // Same drone? (display only) - each rule, and drone numbers in table order.
    {
        auto unreadable = [&](const char* id, double hz, int seconds_ago) {
            Detection d;
            d.id = id; d.band = "5.8 GHz";
            drone_claims::Packet p;
            p.frequency = hz; p.observed = now - std::chrono::seconds(seconds_ago);
            d.packet = p;
            return d;
        };
        std::map<std::string, Detection> g = rows;
        g["same-channel"] = unreadable("same-channel", 5756.5e6, 2);       // 1 s from pkt: likely
        g["near-link"] = unreadable("near-link", 5810e6, 9);               // 5 MHz above the 15 kHz link: possible
        g["far"] = unreadable("far", 5756.5e6, 70);                        // same channel, 69 s apart: unrelated
        Detection rid = g["rid:x"];
        rid.id = "rid:serial";
        rid.remote_id[0].reported_id = "1581F6Z0TEST";                    // pkt's serial: certain
        g[rid.id] = rid;
        const auto grouping = drones_page::group_rows(drones_page::filter_rows(g, -1, now));
        const auto& d = grouping.drone;
        require(d.at("pkt") == 1 && d.at("same-channel") == 1 && d.at("rid:serial") == 1,
                "Same drone: serial match and same DroneID channel join a row to its drone");
        require(d.at("far") != 1 && d.at("near-link") != d.at("link") && d.at("rid:x") != 1,
                "Same drone: unrelated rows and possible links keep their own numbers");
        bool possible = false, certain = false;
        for (const auto& r : grouping.related.at("near-link")) possible = possible || (r.other == "link" && r.strength == drones_page::Relation::Possible);
        for (const auto& r : grouping.related.at("rid:serial")) certain = certain || (r.other == "pkt" && r.strength == drones_page::Relation::Certain);
        require(possible && certain, "Same drone: link strengths recorded");
        require(grouping.drones == g.size() - 2 && grouping.fewest == g.size() - 3, "Same drone: likely and fewest drone counts");
    }

    {
        // Same crystal: a 2.4 GHz link-only row and a second DroneID row join a 5.8 GHz serial row;
        // a link whose grid misses it, and a DroneID packet with another crystal on the same
        // channel in the same second, are marked "probably not" and kept apart.
        std::map<std::string, Detection> g;
        const auto at = [&](int s) { return now - std::chrono::seconds(s); };
        const auto droneid = [&](const char* id, bool readable, double channel, double ppm) {
            Detection d;
            d.id = id; d.band = channel > 3e9 ? "5 GHz" : "2.4 GHz";
            drone_claims::Packet p;
            p.readable = readable; p.model = readable ? "Mini 2" : ""; p.serial = readable ? "5FSCK680119GT5" : "";
            p.version = 2; p.observed = at(5); p.frequency = channel;
            d.packet = p;
            for (int s : {25, 15, 5}) d.droneid_offsets.push_back({ppm, ppm * channel * 1e-6, channel, at(s)});
            g[id] = d;
        };
        const auto link = [&](const char* id, double centre, std::vector<double> grid_hz) {
            Detection d;
            d.id = id; d.band = centre > 3e9 ? "5 GHz" : "2.4 GHz"; d.group = "15kHz";
            symbol_timing::Result t;
            t.observed = at(3); t.timing_group = "15kHz"; t.center = centre;
            d.timing = t;
            d.link_low_hz = centre - 8e6; d.link_high_hz = centre + 8e6;
            int s = 30;
            for (double hz : grid_hz) d.link_grids.push_back({hz, 15e3, centre, .99, at(s -= 8)});
            g[id] = d;
        };
        droneid("serial", true, 5756.5e6, -0.60);
        // 2413.5 MHz x -0.60 ppm = -1448 Hz, plus 5 kHz: 3552 Hz; three captures within 100 Hz.
        link("link24", 2413.5e6, {3600, 3500, 3650});
        // 5806.5 MHz x -0.60 ppm = -3484 Hz; these sit about 1800 Hz off every 5 kHz step.
        link("link58", 5806.5e6, {13316, 13350});
        droneid("other-crystal", false, 5756.5e6, -0.10);   // same channel, same second, 0.5 ppm away
        droneid("same-crystal", false, 2444.5e6, -0.62);    // other band and channel, 0.02 ppm away
        const auto grouping = drones_page::group_rows(drones_page::filter_rows(g, -1, now));
        const auto& d = grouping.drone;
        require(d.at("link24") == d.at("serial") && d.at("same-crystal") == d.at("serial"),
                "Same crystal: a 2.4 GHz link and another DroneID row join the 5.8 GHz serial row");
        require(d.at("link58") != d.at("serial") && d.at("other-crystal") != d.at("serial"),
                "Different crystal: a misfitting link and a same-channel packet stay apart");
        const auto strength = [&](const char* from, const char* to) {
            for (const auto& r : grouping.related.at(from)) if (r.other == to) return std::optional(r);
            return std::optional<drones_page::Relation>();
        };
        const auto l24 = strength("serial", "link24"), l58 = strength("serial", "link58");
        const auto other = strength("serial", "other-crystal"), same = strength("serial", "same-crystal");
        require(l24 && l24->strength == drones_page::Relation::Likely && l24->reason.find("1 in 7") != std::string::npos,
                "Same crystal: link fit is likely and says how often an unrelated link would fit");
        require(l58 && l58->strength == drones_page::Relation::Different && other &&
                    other->strength == drones_page::Relation::Different &&
                    other->reason.find("differ by 0.50 ppm") != std::string::npos,
                "Different crystal: recorded as probably not, overriding the same-channel rule");
        require(same && same->strength == drones_page::Relation::Likely && same->reason.find("agree by 0.02 ppm") != std::string::npos,
                "Same crystal: two DroneID rows within 0.1 ppm");
        require(grouping.drones == 3 && grouping.fewest == 3, "Crystal grouping: three drones, different never joins");
    }

    {
        // The 20 Nov case: a Mini 2 (serial row plus its 15 kHz link) and one Phantom 4 seen as
        // seven Lightbridge rows on three bands. Expect two drones; the Mini 2 and the Lightbridge
        // rows are "probably not" the same; a Lightbridge row heard 60 s before the others stays
        // apart; link clocks that clearly differ split Lightbridge rows.
        std::map<std::string, Detection> g;
        const auto at = [&](int s) { return now - std::chrono::seconds(s); };
        Detection mini;
        mini.id = "serial"; mini.band = "2.4 GHz";
        drone_claims::Packet p;
        p.readable = true; p.model = "Mini 2"; p.serial = "5FSCK680119GT5"; p.version = 2; p.observed = at(4);
        p.frequency = 2414.5e6;
        mini.packet = p;
        mini.droneid_offsets = {{1.18, 1.18 * 2414.5e3, 2414.5e6, at(4)}};
        g[mini.id] = mini;
        const auto link = [&](const char* id, const char* group, double centre, int age, std::vector<double> clocks) {
            Detection d;
            d.id = id; d.band = centre > 3e9 ? "5 GHz" : "2.4 GHz"; d.group = group;
            symbol_timing::Result t;
            t.observed = at(age); t.timing_group = group; t.center = centre;
            d.timing = t;
            d.link_low_hz = centre - 8e6; d.link_high_hz = centre + 8e6;
            const bool lb = std::string(group) == "32.8kHz";
            for (double ppm : clocks) d.link_clocks.push_back({ppm, lb ? 1.0 : 0.1, lb, centre, at(age)});
            g[id] = d;
        };
        link("mini-link", "15kHz", 2418e6, 6, {1.20, 1.15});
        link("lb-24a", "32.8kHz", 2437.5e6, 3, {-4.2, -3.6});
        link("lb-24b", "32.8kHz", 2467e6, 9, {-4.5});
        link("lb-52a", "32.8kHz", 5180e6, 12, {});
        link("lb-52b", "32.8kHz", 5220e6, 15, {-3.9});
        link("lb-58a", "32.8kHz", 5745e6, 20, {-4.0, -4.4});
        link("lb-58b", "32.8kHz", 5785e6, 24, {});
        link("lb-58c", "32.8kHz", 5825e6, 30, {-3.1});
        const auto grouping = drones_page::group_rows(drones_page::filter_rows(g, -1, now));
        const auto& d = grouping.drone;
        require(d.at("mini-link") == d.at("serial"), "Lightbridge case: the Mini 2's link joins its serial row by link clock");
        bool lb_same = true;
        for (const char* id : {"lb-24b", "lb-52a", "lb-52b", "lb-58a", "lb-58b", "lb-58c"}) lb_same = lb_same && d.at(id) == d.at("lb-24a");
        require(lb_same && d.at("lb-24a") != d.at("serial"), "Lightbridge case: seven Lightbridge rows on three bands form one drone");
        require(grouping.drones == 2, "Lightbridge case: two drones, got " + std::to_string(grouping.drones));
        const auto relation = [&](const char* from, const char* to) {
            for (const auto& r : grouping.related.at(from)) if (r.other == to) return std::optional(r);
            return std::optional<drones_page::Relation>();
        };
        const auto not_mini = relation("serial", "lb-24a");
        require(not_mini && not_mini->strength == drones_page::Relation::Different &&
                    not_mini->reason.find("Mini 2 uses OcuSync 2") != std::string::npos,
                "Lightbridge case: a Mini 2 and a Lightbridge link are probably not the same drone");
        const auto lb = relation("lb-24a", "lb-58a");
        require(lb && lb->strength == drones_page::Relation::Likely && lb->reason.find("both Lightbridge links") != std::string::npos &&
                    lb->reason.find("link clocks agree") != std::string::npos,
                "Lightbridge case: Lightbridge rows join, and the reason gives their link clocks");
        const auto mini_link = relation("serial", "mini-link");
        require(mini_link && mini_link->reason.find("link clock agrees with the DroneID crystal") != std::string::npos,
                "Lightbridge case: an OcuSync link clock agreeing with DroneID is shown");

        // One line per drone: the Mini 2 (serial row leads) and the Phantom, with every band heard,
        // and member rows named after their drone's strongest answer.
        const auto ordered = drones_page::filter_rows(g, -1, now);
        const auto lines = drones_page::drone_lines(ordered, grouping, now);
        require(lines.size() == 2 && lines[0].rows.size() + lines[1].rows.size() == 9,
                "One line per drone: two lines over nine rows, got " + std::to_string(lines.size()));
        const auto& mini_line = lines[0].rows.front()->id == "serial" ? lines[0] : lines[1];
        const auto& lb_line = &mini_line == &lines[0] ? lines[1] : lines[0];
        require(mini_line.rows.size() == 2 && mini_line.bands == "2.4 GHz" && lb_line.rows.size() == 7 &&
                    lb_line.bands == "2.4 + 5.2 + 5.8 GHz",
                "One line per drone: rows and bands per line (" + mini_line.bands + " / " + lb_line.bands + ")");
        const auto link_name = drones_page::row_name(g.at("mini-link"), &g.at("serial"), grouping);
        require(link_name.find("Mini 2") == 0 && link_name.find("(link of D") != std::string::npos,
                "One name per drone: the Mini 2's link row is named after the Mini 2: " + link_name);
        require(drones_page::row_name(g.at("serial"), &g.at("serial"), grouping).find("(") == std::string::npos,
                "One name per drone: the lead row keeps its own name");
        // A track opening for a Lightbridge row that joined the Phantom's drone is not a new drone.
        drone_claims::TrackEvent opened;
        opened.event = "opened"; opened.alert = "New drone: DJI Lightbridge";
        opened.track.key = "lb-24a"; opened.track.first_seen = at(2); opened.track.present = true;
        require(drones_page::alert_for_known_drone(opened, g, grouping, now),
                "Alerts: a row joining a drone already shown raises no New drone alert");
        opened.track.first_seen = at(40);
        require(!drones_page::alert_for_known_drone(opened, g, grouping, now),
                "Alerts: the drone's first row still raises its New drone alert");

        {
            // Live 23 Nov 2026: the Phantom's 5.8 and 2.4 GHz rows whose clocks only "possibly"
            // agree (several readings each, sigma about 0.4 per row) still join by the
            // Lightbridge time rule, as rows with no readings would.
            std::map<std::string, Detection> two;
            const auto lb_row = [&](const char* id, double centre, std::vector<double> clocks) {
                Detection d;
                d.id = id; d.band = centre > 3e9 ? "5 GHz" : "2.4 GHz"; d.group = "32.8kHz";
                symbol_timing::Result t;
                t.observed = at(5); t.timing_group = "32.8kHz"; t.center = centre;
                d.timing = t;
                d.link_low_hz = centre - 8e6; d.link_high_hz = centre + 8e6;
                for (double ppm : clocks) d.link_clocks.push_back({ppm, 1.0, true, centre, at(5)});
                two[id] = d;
            };
            lb_row("lb-58", 5730e6, {-4.1, -4.6, -4.3, -4.5, -4.2, -4.5});
            lb_row("lb-24", 2400e6, {-4.4, -4.9, -4.6, -4.8, -4.5, -4.5});
            const auto c = drone_claims::crystal_match(two.at("lb-58"), two.at("lb-24"));
            require(c.kind == drone_claims::CrystalMatch::Possible, "setup: clocks should only possibly agree: " + c.reason);
            const auto gp = drones_page::group_rows(drones_page::filter_rows(two, -1, now));
            require(gp.drones == 1, "Lightbridge rows whose clocks possibly agree join as one drone (got " + std::to_string(gp.drones) + ")");
        }

        // Another Lightbridge aircraft: its clock sits 5 ppm from the Phantom's. Rows without a
        // clock reading (lb-52a, lb-58b) must not chain it into the Phantom's drone.
        link("lb-other", "32.8kHz", 2457e6, 5, {+1.0, +0.6});
        const auto again = drones_page::group_rows(drones_page::filter_rows(g, -1, now));
        bool split = again.drone.at("lb-other") != again.drone.at("lb-24a");
        bool different = false;
        for (const auto& r : again.related.at("lb-other"))
            different = different || (r.other == "lb-24a" && r.strength == drones_page::Relation::Different &&
                                       r.reason.find("link clocks differ") != std::string::npos);
        require(split && different, "Lightbridge case: link clocks 5 ppm apart keep two Lightbridge aircraft apart");
        require(again.drones == 3, "Lightbridge case: Mini 2, Phantom and the other Lightbridge aircraft: three drones");

        // Two Lightbridge rows heard 60 s apart: longer than a row lives, so not joined.
        std::map<std::string, Detection> stale;
        stale["lb-new"] = g["lb-24a"];
        stale["lb-old"] = g["lb-58b"];
        stale["lb-old"].id = "lb-old";
        stale["lb-old"].timing->observed = at(63);
        stale["lb-new"].id = "lb-new";
        const auto apart = drones_page::group_rows(drones_page::filter_rows(stale, -1, now));
        require(apart.drone.at("lb-old") != apart.drone.at("lb-new"), "Lightbridge case: rows 60 s apart stay apart");
    }

    drone_claims::DroneTracks tracks;
    tracks.update(rows, now);
    bool mapped = false;
    for (const auto& [key, track] : tracks.tracks())
        if (track.serial == "1581F6Z0TEST") mapped = drones_page::row_for_track(rows, track) == "pkt";
    require(mapped, "Drones page: an alert's track leads back to its row");

    // Classifier packages: only <dir>/universal_package/universal_v<N>.pt, newest first.
    namespace fs = std::filesystem;
    const fs::path packages = fs::current_path() / "packages";
    for (const char* f : {"2026-01-01/universal_package/universal_v1.pt", "2026-01-01/universal_package/universal_v3.pt",
                          "2026-01-02/universal_package/universal_v2.pt", "2026-01-02/universal_package/universal_dry.pt",
                          "2026-01-03/universal_c2/c2_seed1.pt"}) {
        fs::create_directories((packages / f).parent_path());
        std::ofstream(packages / f) << "x";
    }
    const auto found = drones_page::find_packages(packages.string());
    require(found.size() == 3 && found[0].version == 3 && found[1].version == 2 && found[2].version == 1 &&
                found[0].label.rfind("universal_v3", 0) == 0,
            "Classifier packages: only universal packages listed, newest first");
    require(drones_page::find_packages((packages / "missing").string()).empty(), "Classifier packages: a missing folder lists none");
    std::string package = found[0].path;
    bool reloaded = false;

    drones_page::State page;
    bool classifier_rows = false, setup_opened = false, inference_opened = false;
    drones_page::Inputs in;
    in.rows = &rows; in.tracks = &tracks; in.now = now; in.radio = "USRP X310";
    in.started = true; in.connected = true; in.classifier_only_rows = &classifier_rows;
    in.diagnostics = {"Link timing drops: full 0 | busy 0 | invalid 0 | unavailable 0"};
    drones_page::Actions act;
    act.open_setup = [&] { setup_opened = true; };
    act.open_model_inference = [&] { inference_opened = true; };
    act.reload_classifier = [&] { reloaded = true; };
    drone_claims::LabelledRecorder recorder;
    in.recorder = &recorder; in.recorder_default_dir = (fs::current_path() / "labelled").string();
    in.classifier_package = &package; in.default_package = found[0].path; in.packages_root = packages.string();
    auto page_frame = [&](float width = 1600) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({width, 1000});
        ImGui::Begin("Drones page", nullptr, ImGuiWindowFlags_NoTitleBar);
        ImGui::LogToBuffer(0);
        drones_page::draw(page, cp, scanner, drones, model, nullptr, in, act);
        text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        if (ImGui::GetCurrentContext()->ErrorCountCurrentFrame != 0) throw std::runtime_error("ImGui state error (Drones page)");
        glViewport(0, 0, 1600, 1000);
        glClearColor(.065f, .085f, .115f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    };
    // RFMON_DRONES_PAGE_SNAPSHOT=prefix writes prefix-<name>.ppm images of the page for review.
    const char* snapshot_prefix = std::getenv("RFMON_DRONES_PAGE_SNAPSHOT");
    const auto snapshot = [&](const char* name) {
        if (!snapshot_prefix) return;
        std::vector<unsigned char> px(1600 * 1000 * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, 1600, 1000, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        std::ofstream out(std::string(snapshot_prefix) + "-" + name + ".ppm", std::ios::binary);
        out << "P6\n1600 1000\n255\n";
        for (int y = 999; y >= 0; --y) out.write(reinterpret_cast<const char*>(&px[size_t(y) * 1600 * 3]), 1600 * 3);
    };
    const auto has = [&](const char* s) { return text.find(s) != std::string::npos; };

    page_frame();
    page_frame();
    snapshot("rows");
    require(has("USRP X310") && has("Detecting, coordinated") && has("Cycle") && has("Stop detection"),
            "Drones page: one status strip with receiver, mode, cycle and a single Stop");
    require(has("Drones now") && has("DJI Air 3S") && has("Serial 1581F6Z0TEST") && has("ID 1596F3A2TEST") &&
                has("Classifier-only (development)"),
            "Drones page: one row per drone with level filters and the classifier-only switch");
    require(has("Models") && has("Signal and identity") && has("Reported positions") && has("Show evidence"),
            "Drones page: details panel for the selected row");
    require(has("DroneID offset") && has("+1.210 ppm (latest +6.97 kHz at 5756.5 MHz); 3 packets, spread 0.010 ppm") &&
                has("2.4 GHz +1.210 ppm (2)") && has("difference +0.000 ppm") &&
                has("Link grid") && has("12346 Hz modulo 30.0 kHz (coherence 0.96, 20 blocks)"),
            "Drones page: carrier offsets of the row's DroneID packets and its link grid");
    {
        Detection plain;
        require(drones_page::droneid_offset_text(plain).empty() && drones_page::link_grid_text(plain).empty(),
                "offset texts: nothing measured, nothing shown");
        plain.packet = drone_claims::Packet{};
        plain.timing = symbol_timing::Result{};
        require(drones_page::droneid_offset_text(plain).rfind("not measured", 0) == 0 &&
                    drones_page::link_grid_text(plain).empty(),
                "offset texts: a packet without a listed channel says so; a link without a grid says nothing");
    }
    require(has("Classifier check (DroneID decides this row)") && has("agrees with DroneID") &&
                has("too far from that model's training"),
            "Drones page: a DroneID row shows the classifier as a check, with why it did not name");
    require(has("History") && has("Evidence") && has("Coverage") && has("Diagnostics") && has("Offline review") &&
                has("About these results"),
            "Drones page: tabs and the folded caveats");
    // The selected Air 3S (O4, 30 kHz) row and the 15 kHz link-only row: the model rules the pair out.
    require(has("4 rows") && has("likely 4 drones") && has("Drone") && has("D1") && has("Same drone?") &&
                has("Probably not the same drone as") && has("DJI Air 3S uses O4 (30kHz); this link shows 15kHz"),
            "Drones page: drone numbers, the likely-drones count and the Same drone section");
    {
        std::map<std::string, Detection> two = rows;
        Detection extra;
        extra.id = "pkt-unreadable"; extra.band = "5.8 GHz";
        drone_claims::Packet p;
        p.frequency = 5756.5e6; p.observed = now - std::chrono::seconds(1);
        extra.packet = p;
        two[extra.id] = extra;
        in.rows = &two;
        page.selected = "pkt";
        page_frame();
        page_frame();
        require(has("5 rows") && has("likely 4 drones") && has("2 rows") && has("One line per drone") &&
                    has("Probably the same drone as") && has("same channel (5756.5 MHz)") && !has("D1 · 2 rows"),
                "Drones page: two rows of one drone share one line and say why");
        page.expanded.insert("pkt");
        page_frame();
        page_frame();
        require(has("(DroneID of D1)"), "Drones page: an opened drone line lists its rows, named after the drone");
        snapshot("same-open");
        page.expanded.clear();
        page.per_drone = false;
        page_frame();
        page_frame();
        require(has("D1 · 2 rows"), "Drones page: with one line per drone off, every row shows its drone number");
        snapshot("same");
        page.per_drone = true;
        {
            // The 20 Nov scene on the page: a Mini 2 (serial row and its link) and the Phantom 4
            // as five Lightbridge rows on three bands. Two drone lines.
            std::map<std::string, Detection> scene;
            const auto at = [&](int sec) { return now - std::chrono::seconds(sec); };
            Detection mini;
            mini.id = "serial"; mini.band = "2.4 GHz";
            drone_claims::Packet mp;
            mp.readable = true; mp.model = "Mini 2"; mp.serial = "5FSCK680119GT5"; mp.version = 2;
            mp.observed = at(4); mp.frequency = 2414.5e6;
            mini.packet = mp;
            mini.droneid_offsets = {{1.18, 1.18 * 2414.5e3, 2414.5e6, at(4)}};
            scene[mini.id] = mini;
            const auto link = [&](const char* id, const char* group, double centre, int age, std::vector<double> clocks) {
                Detection d;
                d.id = id; d.band = centre > 3e9 ? "5 GHz" : "2.4 GHz"; d.group = group;
                symbol_timing::Result t;
                t.observed = at(age); t.timing_group = group; t.center = centre;
                d.timing = t;
                d.link_low_hz = centre - 8e6; d.link_high_hz = centre + 8e6;
                const bool lb = std::string(group) == "32.8kHz";
                for (double ppm : clocks) d.link_clocks.push_back({ppm, lb ? 1.0 : 0.1, lb, centre, at(age)});
                d.candidates = drone_claims::candidates_for(group);
                scene[id] = d;
            };
            link("mini-link", "15kHz", 2418e6, 6, {1.20, 1.15});
            link("lb-24a", "32.8kHz", 2437.5e6, 3, {-4.2, -3.6});
            link("lb-24b", "32.8kHz", 2467e6, 9, {-4.5});
            link("lb-52", "32.8kHz", 5220e6, 15, {-3.9});
            link("lb-58a", "32.8kHz", 5745e6, 20, {-4.0, -4.4});
            link("lb-58b", "32.8kHz", 5785e6, 24, {});
            in.rows = &scene;
            page.selected = "lb-24a";
            page_frame();
            page_frame();
            require(has("7 rows") && has("likely 2 drones") && has("5 rows") && has("2.4 + 5.2 + 5.8 GHz") &&
                        has("Link clock") && has("from 2 captures (Lightbridge symbol timing"),
                    "Drones page: the Mini 2 and the Phantom's five Lightbridge rows show as two drones");
            snapshot("lightbridge");
            in.rows = &rows;
            page.selected = "pkt";
            page_frame();
        }
        in.rows = &rows;
        page_frame();
    }
    require(!drones.unified && has("Decoders off") && has("Drone decoders are off"),
            "Drones page: decoders that are off are flagged in the status strip");
    require(!setup_opened && !inference_opened && scanner.active_band() == BAND_COORDINATED && !classifier_rows,
            "Drones page: drawing alone changes nothing");
    if (!tracks.alerts().empty()) {
        require(has("Dismiss"), "Drones page: the newest alert is shown");
        page.dismissed_alert = drones_page::alert_key(tracks.alerts().front());
        page_frame();
        require(!has("Dismiss"), "Drones page: a dismissed alert stays hidden");
    }
    page.tier_filter = int(Tier::Reported);
    page_frame();
    require(has("ID 1596F3A2TEST") && !has("Serial 1581F6Z0TEST") && page.selected == "rid:x",
            "Drones page: filtering moves the selection to a listed row");
    page.tier_filter = -1;
    page.selected = "link";
    page_frame();
    page_frame();
    require(has("Classifier's best guesses (not named)") && has("Mini 2") && has("below the naming limit in 3 of 3"),
            "Drones page: best guesses and the missed naming limit shown for a link-only row");
    require(has("DJI Air 3S · O4") && has("DJI OcuSync 1, OcuSync 2, O3 or O4") && !has("OcuSync family"),
            "Drones page: OcuSync generation shown in the table and details, never a bare OcuSync family");
    snapshot("family");
    page_frame(900);
    snapshot("narrow");
    require(has("Signal and identity") && has("DJI Air 3S"), "Drones page: narrow window stacks the details");
    require(has("Bands: where the receiver looks and where each drone was heard") && !has("label did not fit") &&
                !has("labels did not fit"),
            "Drones page: band bar caption shown and every row label fits");

    const struct { int tab; const char* expect; const char* why; } tabs[] = {
        {drones_page::TabEvidence, "not a count of physical aircraft", "Evidence tab: combined entries with source filter"},
        {drones_page::TabCoverage, "Revisit per centre: configured / measured", "Coverage tab: strip and per-band coverage"},
        {drones_page::TabDiagnostics, "Link timing drops: full 0", "Diagnostics tab: module health and extra lines"},
        {drones_page::TabOffline, "Decode interval", "Offline review tab"},
        {drones_page::TabHistory, "Drone history", "History tab"}};
    for (const auto& t : tabs) {
        page.select_tab = t.tab;
        page_frame();
        page_frame();
        require(has(t.expect), t.why);
    }
    page.select_tab = drones_page::TabCoverage;
    page_frame();
    page_frame();
    require(has("5 GHz Wi-Fi  6 x 50 ms") && has("2.4 GHz  4 x 50 ms"), "Coverage tab: slots shown by their names");
    page.select_tab = drones_page::TabEvidence;
    page_frame();
    page_frame();
    require(has("Sources"), "Evidence tab: per-source filter shown");
    snapshot("evidence");
    page.select_tab = drones_page::TabDiagnostics;
    page_frame();
    page_frame();
    require(has("Modules") && has("Native DJI packets and O4 structure evidence") &&
                has("Drone decoders and classifier (on by default)"),
            "Diagnostics tab: decoder sections and the decoders switch");
    require(has("Classifier package") && has("universal_v3") && has("default") && has("classifier off; used when it starts") &&
                !reloaded && package == found[0].path,
            "Diagnostics tab: classifier package picker shows the chosen package and changes nothing by itself");
    require(has("Training recorder") && has("Start recording") && has("labelled") && !recorder.recording(),
            "Diagnostics tab: training recorder section, not recording until started");
    {
        drone_claims::LabelledRecorderLimits limits;
        limits.min_free_bytes = 0;
        drone_claims::LabelledRecorder live(limits);
        require(live.start(in.recorder_default_dir), "Training recorder: starts in the chosen folder");
        in.recorder = &live;
        page_frame();
        page_frame();
        require(has("Recording labelled captures: 0") && has("Stop recording") && has("Recording to"),
                "Drones page: status strip and Diagnostics show the recorder running");
        live.stop();
        in.recorder = &recorder;
        page_frame();
    }

    const std::map<std::string, Detection> none;
    in.rows = &none;
    page_frame();
    require(has("No drones in recent evidence") && has("not proof that none is there"), "Drones page: empty state while detecting");
    scanner.set_active_band(BAND_WIFI_2G4);
    page_frame();
    require(has("Start detection") && has("Receiving") && has("Detection is not running"),
            "Drones page: one Start when only a single band is received");
    require(scanner.active_band() == BAND_WIFI_2G4, "Drones page: Start is not pressed by drawing");
    in.started = false; in.connected = false;
    page_frame();
    require(has("Receiver not connected") && has("Connect receiver") && !setup_opened,
            "Drones page: disconnected state offers Connect receiver");
    scanner.set_active_band(BAND_COORDINATED);
}
}  // namespace

int main() {
    entry_tests();
    export_tests();
    auto display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    require(eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "Offscreen EGL available");
    EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count;
    require(eglChooseConfig(display, attrs, &config, 1, &count) && count, "EGL config");
    EGLint dims[] = {EGL_WIDTH, 1600, EGL_HEIGHT, 1000, EGL_NONE};
    auto surface = eglCreatePbufferSurface(display, config, dims);
    auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    require(eglMakeCurrent(display, surface, surface, context), "EGL make current");
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1600, 1000};
    io.DeltaTime = 1.f / 60;
    gui::configure_font();
    gui::apply_theme();
    ImGui_ImplOpenGL3_Init("#version 130");

    char tmp[] = "/tmp/rfmon-coordinated-panel-XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    const auto cwd = std::filesystem::current_path();
    std::filesystem::current_path(tmp);
    int rc = 0;
    try {
        Scanner scanner(tmp);
        ScannerTestAccess::set_device_factory(scanner, [](const DeviceProfile&, std::optional<double>) {
            return std::unique_ptr<CaptureDevice>(new QuietRadio());
        });
        auto schedule = coord::default_schedule(2e6);
        for (auto& slot : schedule.slots) slot.dwell_s = 0.05;
        schedule.slots[0].visits[0].sample_rate_hz = 1e6;
        schedule.settle_s = 0.005;
        scanner.set_wifi_sample_rate(2e6);
        scanner.set_coordinated_schedule(schedule);
        drone::Addon drones;
        ml::Inference model;
        coordinated::PanelState state;
        std::string text;
        auto frame = [&] {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowSize({1600, 1000});
            ImGui::Begin("Drones");
            ImGui::LogToBuffer(0);
            coordinated::draw_section(state, scanner, drones, model, true);
            text = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
            ImGui::End();
            ImGui::Render();
            if (ImGui::GetCurrentContext()->ErrorCountCurrentFrame != 0) throw std::runtime_error("ImGui state error");
            glViewport(0, 0, 1600, 1000);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        };
        scanner.set_active_band(BAND_SUB_GHZ);
        frame();
        require(text.find("Start coordinated detection") != std::string::npos && text.find("Configured: cycle") != std::string::npos,
                "Stopped: Start control and configured cycle shown");
        require(scanner.active_band() == BAND_SUB_GHZ, "Rendering alone does not change acquisition");
        scanner.set_active_band(BAND_COORDINATED);
        scanner.start();
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (scanner.coordinated_status().cycle < 2 && std::chrono::steady_clock::now() < end) {
            frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        while (!scanner.coordinated_status().current && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame();
        require(text.find("Stop coordinated detection") != std::string::npos, "Running: Stop control shown");
        require(text.find("Receiving:") != std::string::npos && text.find("-> repeat") != std::string::npos, "Schedule strip with current visit");
        require(text.find("Lane drops") != std::string::npos && text.find("Revisit per centre: configured / measured") != std::string::npos,
                "Per-band coverage table");
        require(text.find("All detection entries") != std::string::npos && text.find("not a count of physical aircraft") != std::string::npos,
                "Combined entries table with its meaning stated");
        require(text.find("drone model is not supported there") != std::string::npos, "Sub-GHz ML limitation disclosed");
        require(text.find("No entry selected.") != std::string::npos && text.find("Retain recent Wi-Fi/drone visit IQ") != std::string::npos,
                "Inspector and opt-in IQ retention control shown");
        require(scanner.iq_retention() == 0, "IQ retention is off unless explicitly enabled");
        // Inspector on a real row: provenance, relationships, conflicts, export.
        state.selected_key = g_entries[2].key;
        state.export_dir = std::string(tmp) + "/evidence";
        {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowSize({1600, 1000});
            ImGui::Begin("Entries");
            ImGui::LogToBuffer(0);
            coordinated::draw_entry_views(state, scanner, g_entries, g_visits, "hash", Scanner::now_steady_s());
            text = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
            ImGui::End();
            ImGui::Render();
            if (ImGui::GetCurrentContext()->ErrorCountCurrentFrame != 0) throw std::runtime_error("ImGui state error");
        }
        require(text.find("Source interval: capture #7, visit") != std::string::npos && text.find("Provisional relationships") != std::string::npos &&
                    text.find("Conflict: reported model differs") != std::string::npos && text.find("Export evidence") != std::string::npos &&
                    text.find("IQ for this entry's capture: not retained") != std::string::npos,
                "Inspector shows source visit, relationships, conflicts, IQ status and export");
        require(text.find("4 ~ !") != std::string::npos, "Table marks rows with relationships and conflicts");
        state.selected_key = "rid/gone";
        frame();
        require(text.find("no longer retained by its detector") != std::string::npos, "A vanished selection is reported, not silently replaced");
        scanner.set_wifi_fixed_channel(6);
        const auto conflict_end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (scanner.coordinated_status().conflict.empty() && std::chrono::steady_clock::now() < conflict_end)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        frame();
        require(text.find("Clear fixed Wi-Fi channel") != std::string::npos, "Conflict shown with its explicit resolution control");
        scanner.set_wifi_fixed_channel(std::nullopt);
        drones_page_tests(scanner, drones, model, state, text);
        scanner.stop();
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        rc = 1;
    }
    std::filesystem::current_path(cwd);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    std::cout << (rc ? "FAILED" : "ALL PASSED") << '\n';
    return rc;
}
