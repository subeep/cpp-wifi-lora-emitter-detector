// Offline replay of a live run's data/drone_offsets.jsonl through the app's rows and the Drones
// page's grouping. No radio, no IQ.
//
// Each logged link capture becomes a symbol_timing::Result again (group, grid, link clock with its
// symbol phase, device time) and each DroneID line a decoded packet, fed to drone_claims::Store in
// log order at the log's own times. The probe's later rules are applied to the logged values where
// the log holds what they need: a 30 kHz group needs 30 kHz grid coherence 0.5. Link clocks are
// taken as logged (they cannot be measured again without IQ). Every --step
// seconds the page state is taken as drawn: detections, filter_rows (all levels), group_rows.
//
// Not in the log, so not replayed: classifier sightings (classifier-only rows are hidden by
// default), Remote ID, captures with no flagged timing at all. Lines from before 21 Nov 2026 have
// no link clock, so their Lightbridge rows can never be confirmed: replay those for structure only.
//
//   drone_log_replay LOG [--from-line N] [--to-line M] [--step S] [--verbose] [--why HH:MM:SS]
// --why prints, for the page state at that time, every pair of rows with its "same drone?" verdict.

#include "drones/crystal_match.hpp"
#include "drones/evidence_claims.hpp"
#include "drones_page.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <nlohmann/json.hpp>

using namespace rfmon;
namespace dc = rfmon::drone_claims;
namespace st = rfmon::symbol_timing;
using Clock = std::chrono::steady_clock;

static const char* group_name(const std::string& g) {
    for (const char* name : {"15kHz", "30kHz", "15kHz+30kHz", "32.8kHz"})
        if (g == name) return name;
    return nullptr;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s LOG [--from-line N] [--to-line M] [--step S] [--verbose]\n", argv[0]); return 2; }
    std::size_t from = 1, to = SIZE_MAX;
    double step = 5;
    bool verbose = false;
    std::string why;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--from-line" && i + 1 < argc) from = std::stoul(argv[++i]);
        else if (a == "--to-line" && i + 1 < argc) to = std::stoul(argv[++i]);
        else if (a == "--step" && i + 1 < argc) step = std::stod(argv[++i]);
        else if (a == "--verbose") verbose = true;
        else if (a == "--why" && i + 1 < argc) why = argv[++i];
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    std::ifstream in(argv[1]);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    const auto base = Clock::now();
    double first_ms = NAN, next_report = NAN;
    dc::Store store;
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> histogram;  // (drones, fewest) -> reports
    std::size_t reports = 0, lines = 0, links = 0, packets = 0, idle = 0;
    double last_line_ms = NAN;
    const auto at = [&](double ms) {
        return base + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms - first_ms));
    };
    const auto report = [&](double ms) {
        // No capture logged for a row lifetime: the receiver was reconnecting or stopped.
        if (std::isnan(last_line_ms) || ms - last_line_ms > 45e3) { ++idle; return; }
        const auto now = at(ms);
        store.expire(now);
        const auto rows = dc::detections(store, now);
        const auto ordered = drones_page::filter_rows(rows, -1, now);
        const auto g = drones_page::group_rows(ordered);
        ++histogram[{g.drones, g.fewest}];
        ++reports;
        const std::time_t tt = static_cast<std::time_t>(ms / 1000);
        char hhmmss[16];
        std::strftime(hhmmss, sizeof hhmmss, "%H:%M:%S", std::localtime(&tt));
        if (!why.empty() && why == hhmmss) {
            static const char* kinds[] = {"undecided", "possibly same", "likely same", "different"};
            for (std::size_t i = 0; i < ordered.size(); ++i) {
                const auto* r = ordered[i];
                const auto m = dc::link_clock_mean(r->link_clocks);
                std::printf("  row %s: group %s, %zu DroneID offsets, %zu link clocks%s, %zu grids\n", r->id.c_str(),
                            r->group.c_str(), r->droneid_offsets.size(), r->link_clocks.size(),
                            m ? (" (mean " + std::to_string(m->ppm) + ")").c_str() : "", r->link_grids.size());
            }
            for (std::size_t i = 0; i < ordered.size(); ++i)
                for (std::size_t k = i + 1; k < ordered.size(); ++k) {
                    const auto c = dc::crystal_match(*ordered[i], *ordered[k]);
                    std::printf("  %s vs %s: %s %s\n", ordered[i]->id.c_str(), ordered[k]->id.c_str(), kinds[c.kind], c.reason.c_str());
                }
        }
        if (verbose) {
            std::printf("%s  %zu rows, likely %zu drone%s (perhaps %zu):", hhmmss, ordered.size(), g.drones,
                        g.drones == 1 ? "" : "s", g.fewest);
            for (const auto* r : ordered) {
                const auto d = g.drone.find(r->id);
                std::printf("  [D%d %s %s]", d == g.drone.end() ? 0 : d->second,
                            r->packet && r->packet->readable ? r->packet->model.c_str() : r->group.c_str(), r->band.c_str());
            }
            std::printf("\n");
        }
    };
    std::string line;
    for (std::size_t n = 1; std::getline(in, line); ++n) {
        if (n < from) continue;
        if (n > to) break;
        const auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.contains("unix_ms")) continue;
        const double ms = j.at("unix_ms").get<double>();
        if (std::isnan(first_ms)) { first_ms = ms; next_report = ms + step * 1e3; }
        while (ms >= next_report) { report(next_report); next_report += step * 1e3; }
        ++lines;
        last_line_ms = ms;
        const std::string kind = j.value("kind", "");
        if (kind == "link") {
            st::Result r;
            r.run_id = j.at("run");
            r.radio_session = j.at("session");
            r.capture_seq = j.at("capture");
            r.center = j.at("center_hz");
            r.rate = j.value("sample_rate_hz", 20e6);
            r.blocks = j.value("blocks", 24);
            r.device_ns = j.value("device_ns", int64_t{0});
            r.observed = at(ms);
            r.timing_group = j["group"].is_string() ? group_name(j["group"]) : nullptr;
            for (const auto& gr : j.at("grid")) {
                const std::string h = gr.at("hypothesis");
                for (std::size_t i = 0; i < st::hypotheses.size(); ++i)
                    if (h == st::hypotheses[i].name)
                        r.grid[i] = st::GridOffset{gr.at("hz"), gr.at("spacing_hz"), gr.at("coherence"), gr.at("blocks")};
            }
            if (r.timing_group && std::string(r.timing_group).find("30kHz") != std::string::npos &&
                (!r.grid[st::lte_30k] || r.grid[st::lte_30k]->coherence < st::min_30k_grid_coherence))
                r.timing_group = std::string(r.timing_group) == "15kHz+30kHz" ? "15kHz" : nullptr;
            if (j.contains("link_clock")) {
                const auto& c = j["link_clock"];
                const std::string name = c.value("link", "");
                const bool lb = name == "lightbridge";
                // The logged clock is what the app measured then (since 23 Nov, an OcuSync clock
                // from a capture with Lightbridge blocks comes from its other blocks, live only).
                {
                    link_clock::Estimate e{lb ? link_clock::Link::lightbridge
                                              : name == "ocusync_30k" ? link_clock::Link::ocusync_30k : link_clock::Link::ocusync_15k,
                                           c.at("ppm"), c.at("sigma_ppm"), c.value("blocks", std::size_t{24})};
                    if (c.contains("symbol_phase_s")) { e.has_symbol_phase = true; e.symbol_phase_s = c["symbol_phase_s"]; }
                    r.link_clock = e;
                }
            }
            store.timing(r);
            ++links;
        } else if (kind == "droneid") {
            dc::Packet p;
            p.model = j["model"].is_string() ? j["model"].get<std::string>() : "";
            p.serial = j["serial"].is_string() ? j["serial"].get<std::string>() : "";
            p.readable = j.value("readable", false);
            p.encrypted = j.value("encrypted", false);
            p.observed = at(ms);
            p.version = 2;
            p.frequency = j.at("center_hz");
            if (j["offset_hz"].is_number()) p.offset_hz = j["offset_hz"].get<double>();
            if (j["channel_hz"].is_number()) p.channel_hz = j["channel_hz"];
            if (j["offset_ppm"].is_number()) p.offset_ppm = j["offset_ppm"].get<double>();
            store.decoded({j.at("run"), j.at("session"), j.at("capture"), j.at("center_hz")}, std::move(p));
            ++packets;
        }
    }
    if (!std::isnan(next_report)) report(next_report);
    std::printf("%zu lines (%zu link captures, %zu DroneID packets), %zu page states every %.0f s "
                "(%zu more with nothing logged for 45 s left out):\n", lines, links, packets, reports, step, idle);
    for (const auto& [k, n] : histogram)
        std::printf("  likely %zu drone%s (perhaps %zu): %zu (%.0f%%)\n", k.first, k.first == 1 ? "" : "s", k.second, n,
                    100.0 * n / std::max<std::size_t>(reports, 1));
    return 0;
}
