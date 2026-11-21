// E1: offline replay of a recorded X310 session through the app's own classifier path. No radio.
//
// Each clean capture of a `background_rotate.py` session (session.json + sc16 files) goes through
// the same code as a live capture: `ml::Inference::observe` picks the three dispersed 8 ms
// intervals and the production worker decides the capture; `drone_claims::ClassifierSightings`
// then applies the classifier-only row rule. Captures are fed one at a time, and each decision is
// given the capture's recorded time, so the row windows (200 s / 300 s) see the session's real
// spacing rather than the replay's speed. No link, DroneID or Remote ID rows exist here, so every
// classifier row counts (the scorecard's false rows per hour on no-drone background).
//
//   drone_session_replay --python PY --worker universal_worker.py --model PACKAGE --session DIR
//                        [--from-minute M] [--to-minute M] [--skip-2g4-from-minute M] [--json OUT]
// --skip-2g4-from-minute leaves out 2.4 GHz captures from that minute on, as the Python split does
// for the 13 Nov hour (a Phantom's Lightbridge link at 2430-2470 MHz from minute 18).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "drones/universal_claim.hpp"
#include "ml_inference.hpp"

using namespace rfmon;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
// ISO 8601 with offset ("2026-11-16T11:49:17.905340+00:00") to Unix seconds.
double unix_seconds(const std::string& s) {
    std::tm tm{};
    double frac = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        throw std::runtime_error("bad timestamp: " + s);
    tm.tm_year -= 1900; tm.tm_mon -= 1;
    auto p = s.find('.', 19);
    size_t end = 19;
    if (p == 19) { end = s.find_first_of("+-Z", 20); frac = std::stod("0" + s.substr(19, end - 19)); }
    double offset = 0;
    if (end < s.size() && (s[end] == '+' || s[end] == '-')) {
        int h = 0, m = 0;
        std::sscanf(s.c_str() + end + 1, "%d:%d", &h, &m);
        offset = (s[end] == '+' ? 1 : -1) * (h * 3600 + m * 60);
    }
    return double(timegm(&tm)) + frac - offset;
}

std::vector<std::complex<float>> read_sc16(const std::string& path, size_t samples) {
    std::vector<int16_t> raw(2 * samples);
    std::ifstream f(path, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size() * 2))) throw std::runtime_error("short IQ file " + path);
    std::vector<std::complex<float>> iq(samples);
    for (size_t i = 0; i < samples; ++i) iq[i] = {raw[2 * i] / 32768.f, raw[2 * i + 1] / 32768.f};
    return iq;
}
}  // namespace

int main(int argc, char** argv) {
    std::string python, worker, model, session_dir, json_out;
    double from_minute = 0, to_minute = 1e9, skip_2g4_from = 1e9;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) throw std::runtime_error("missing value for " + a); return argv[++i]; };
        if (a == "--python") python = next();
        else if (a == "--worker") worker = next();
        else if (a == "--model") model = next();
        else if (a == "--session") session_dir = next();
        else if (a == "--from-minute") from_minute = std::stod(next());
        else if (a == "--to-minute") to_minute = std::stod(next());
        else if (a == "--json") json_out = next();
        else if (a == "--skip-2g4-from-minute") skip_2g4_from = std::stod(next());
        else { std::fprintf(stderr, "usage: %s --python PY --worker W --model PKG --session DIR [--from-minute M] [--to-minute M] [--json OUT]\n", argv[0]); return 2; }
    }
    if (python.empty() || worker.empty() || model.empty() || session_dir.empty()) { std::fprintf(stderr, "missing required argument\n"); return 2; }
    try {
        json session;
        std::ifstream(session_dir + "/session.json") >> session;
        const double rate = session.at("rate_hz");
        std::vector<json> caps;
        for (const auto& c : session.at("captures")) if (c.at("status") == "ok") caps.push_back(c);
        if (caps.empty()) throw std::runtime_error("no clean captures");
        double t0 = 1e300;
        for (const auto& c : caps) t0 = std::min(t0, unix_seconds(c.at("host_start")));

        ml::Inference engine;
        ml::Options options{python, worker, model, 30000};
        if (!engine.start(options)) throw std::runtime_error("worker start failed");
        for (auto deadline = Clock::now() + std::chrono::seconds(120); !engine.snapshot().ready;) {
            const auto s = engine.snapshot();
            if (s.state == "Failed" || Clock::now() > deadline) throw std::runtime_error("worker load failed: " + s.error);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!engine.snapshot().universal) throw std::runtime_error("not a universal package");

        drone_claims::ClassifierSightings sightings;
        const Clock::time_point base{std::chrono::hours(1000)};  // replay clock: session time from here
        const std::string run_id = "replay:" + session_dir.substr(session_dir.find_last_of('/') + 1);
        struct Row { std::string id, type; double first = 0, last = 0; bool named = false; size_t captures = 0; std::vector<std::string> models; };
        std::vector<Row> rows;
        std::map<std::string, size_t> open;  // row id -> its latest entry in `rows`
        json decisions = json::array();
        size_t replayed = 0, typed = 0, named = 0, timeouts = 0, skipped_2g4 = 0;
        double first_min = 1e300, last_min = -1e300;
        Clock::time_point last_submit{};
        for (size_t k = 0; k < caps.size(); ++k) {
            const auto& c = caps[k];
            const double minute = (unix_seconds(c.at("host_start")) - t0) / 60;
            if (minute < from_minute || minute >= to_minute) continue;
            if (double(c.at("centre_hz")) < 3e9 && minute >= skip_2g4_from) { ++skipped_2g4; continue; }
            const size_t n = c.at("samples");
            auto iq = read_sc16(session_dir + "/" + c.at("file").get<std::string>(), n);
            wifi_security::CaptureRecord rec;
            rec.run_id = run_id; rec.source = "offline:" + session_dir; rec.radio_session = 1; rec.capture_seq = k + 1;
            rec.band = double(c.at("centre_hz")) < 3e9 ? "2.4 GHz" : "5.8 GHz";
            rec.capture_center_hz = rec.actual_rf_hz = c.at("centre_hz"); rec.actual_dsp_hz = 0;
            rec.requested_rate_hz = rec.sample_rate_hz = rate; rec.samples_requested = rec.samples_received = n;
            rec.device = "X310 (recorded)";
            // One job at a time: observe() drops a capture while one is queued or within 500 ms of the last.
            const auto jobs = engine.snapshot().submitted_jobs;
            for (int attempt = 0; engine.snapshot().submitted_jobs == jobs; ++attempt) {
                if (attempt > 200) throw std::runtime_error("capture never accepted by the engine");
                std::this_thread::sleep_until(last_submit + std::chrono::milliseconds(510));
                engine.observe(iq, rec);
                if (engine.snapshot().submitted_jobs == jobs) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            last_submit = Clock::now();
            const ml::Result* last = nullptr;
            ml::Snapshot snap;
            for (auto deadline = Clock::now() + std::chrono::seconds(60);;) {
                snap = engine.snapshot();
                if (snap.state == "Failed") throw std::runtime_error("worker failed: " + snap.error);
                for (const auto& r : snap.history)
                    if (r.capture == rec.capture_seq && r.interval_index + 1 == r.interval_count) { last = &r; break; }
                if (last || Clock::now() > deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!last) { ++timeouts; continue; }
            const auto now = base + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(minute * 60));
            for (auto& r : snap.history) if (r.capture == rec.capture_seq) r.completed = now;
            ++replayed;
            first_min = std::min(first_min, minute); last_min = std::max(last_min, minute);
            const auto& d = last->capture_decision;
            json models = json::array();
            for (const auto& m : d.models) models.push_back({{"name", m.name}, {"score", m.score}});
            decisions.push_back({{"file", c.at("file")}, {"minute", minute}, {"centre_mhz", std::llround(rec.capture_center_hz / 1e6)},
                                 {"present", d.present}, {"intervals", d.intervals}, {"offsets", json::array()},
                                 {"type", d.type}, {"best_type", d.best_type}, {"type_score", d.type_score},
                                 {"named", d.named}, {"models", models}, {"reason", d.reason}});
            for (const auto& r : snap.history)
                if (r.capture == rec.capture_seq) decisions.back()["offsets"].push_back(r.offset_samples);
            if (!d.type.empty()) ++typed;
            if (d.named) ++named;
            sightings.ingest(snap, now);
            std::map<std::string, drone_claims::Detection> shown;
            sightings.add_rows(shown, snap, now, true);
            for (const auto& [id, row] : shown) {
                const auto& u = *row.universal;
                const double first = std::chrono::duration<double>(u.first_observed - base).count() / 60;
                const double latest = std::chrono::duration<double>(u.last_observed - base).count() / 60;
                // The app forgets sightings older than the merge time, so a long row's first capture
                // moves forward; it is still the same row while its run overlaps the one shown before.
                auto it = open.find(id);
                if (it == open.end() || first > rows[it->second].last) {
                    rows.push_back({id, u.type, first, latest});
                    it = open.insert_or_assign(id, rows.size() - 1).first;
                }
                auto& x = rows[it->second];
                x.last = latest; x.captures = std::max(x.captures, u.captures); x.named = x.named || u.accepted;
                if (u.accepted) { x.models.clear(); for (const auto& m : u.models) x.models.push_back(m.name); }
            }
            if (replayed % 100 == 0)
                std::fprintf(stderr, "replayed %zu (minute %.1f): typed %zu, named %zu, rows %zu\n", replayed, minute, typed, named, rows.size());
        }
        engine.stop();
        json row_list = json::array();
        size_t named_rows = 0;
        for (const auto& r : rows) {
            named_rows += r.named;
            row_list.push_back({{"id", r.id}, {"type", r.type}, {"first_minute", r.first}, {"last_minute", r.last},
                                {"captures_max_held", r.captures}, {"named", r.named}, {"models", r.models}});
        }
        const double hours = replayed > 1 ? (last_min - first_min) / 60 : 0;
        json out = {{"schema", "rfmon.drone_session_replay.v1"}, {"session", session_dir}, {"model", model},
                    {"from_minute", from_minute}, {"to_minute", to_minute < 1e8 ? json(to_minute) : json(nullptr)},
                    {"path", "ml::Inference (3 dispersed intervals) -> universal_worker -> drone_claims::ClassifierSightings"},
                    {"skip_2g4_from_minute", skip_2g4_from < 1e8 ? json(skip_2g4_from) : json(nullptr)},
                    {"summary", {{"captures", replayed}, {"skipped_2g4", skipped_2g4}, {"timeouts", timeouts}, {"typed_captures", typed}, {"named_captures", named},
                                 {"rows", rows.size()}, {"named_rows", named_rows}, {"hours", hours}}},
                    {"rows", row_list}, {"captures", decisions}};
        std::printf("%s\n", out["summary"].dump().c_str());
        if (!json_out.empty()) std::ofstream(json_out) << out.dump(1) << '\n';
        return timeouts ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
