// Offline check of the link clock (crystal ppm from symbol timing) on recorded sc16 IQ. No radio.
//
// Cuts each file into visits of the app's capture length, keeps the 24 blocks the timing monitor
// takes from each (block_start), and runs the live measurement on them (measure_blocks, which
// picks the link from the timing group). With --link it measures that link directly instead.
// Prints one line per visit; the Python reference is air3s_iq_recording/analysis/app_blocks_ppm.py.
//
//   drone_link_clock_check FILE... [--rate 20e6] [--visit 0.512] [--center HZ]
//                                  [--link lightbridge|ocusync_15k]
//   drone_link_clock_check --session DIR [--rows [--skip-mhz F ...] [--shift-mhz F --shift-us X]]
//   drone_link_clock_check --offset-log FILE [--from-line N]
// --offset-log replays a live run's data/drone_offsets.jsonl (link lines with link_clock.symbol_phase_s,
// logged since 21 Nov 2026): per band, at every Lightbridge capture, the frame-timing fit over the
// frame history before it (frame_lifetime, 150 s), with the single-capture mean as prior.
// --session reads a sweep_record.py session (air3s_iq_recording: visits on several centres, each
// with the X310 device time of its first sample), measures every clean visit as one live capture,
// and chains the Lightbridge visits' symbol grid positions (frame_timing.hpp). The Python
// reference is air3s_iq_recording/analysis/frame_phase.py --session DIR --method harmonic.
// --rows also replays the visits through the app's rows (drone_claims::Store and detections, at
// the visits' own device-time spacing) and prints, after each cycle, every row's clock as the
// Drones page would show it and the "same drone?" verdict between rows. --skip-mhz leaves out
// centres (e.g. every second one, so the remaining centres are too far apart to form one row).
// --shift-mhz/--shift-us moves the device time of that centre's visits by X us: a stand-in for a
// second transmitter with the same clock but its own frame timing (test of the identity check).

#include "drones/crystal_match.hpp"
#include "drones/frame_timing.hpp"
#include "drones/symbol_timing_monitor.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace rfmon;

// Reads `count` blocks of a capture of `capture` samples (block_start positions) from an sc16 file.
static bool read_blocks(std::ifstream& in, std::size_t first, std::size_t capture, std::size_t count,
                        std::vector<std::complex<float>>& blocks, std::array<std::size_t, symbol_timing::max_blocks>& starts) {
    std::vector<int16_t> raw(symbol_timing::block_size * 2);
    for (std::size_t b = 0; b < count; ++b) {
        starts[b] = symbol_timing::block_start(capture, b, count);
        in.seekg(static_cast<std::streamoff>((first + starts[b]) * 4));
        if (!in.read(reinterpret_cast<char*>(raw.data()), raw.size() * sizeof(int16_t))) return false;
        for (std::size_t k = 0; k < symbol_timing::block_size; ++k)
            blocks[b * symbol_timing::block_size + k] = {raw[2 * k] / 32768.f, raw[2 * k + 1] / 32768.f};
    }
    return true;
}

static void print_rows(const drone_claims::Store& store, std::chrono::steady_clock::time_point now, double t) {
    const auto rows = drone_claims::detections(store, now);
    std::vector<const drone_claims::Detection*> lb;
    for (const auto& [id, row] : rows)
        if (row.group == "32.8kHz") lb.push_back(&row);
    std::printf("  t=%6.1f s: %zu row(s)\n", t, rows.size());
    for (const auto* row : lb) {
        const auto f = drone_claims::frame_fit(*row);
        const auto m = drone_claims::link_clock_mean(row->link_clocks);
        std::printf("    %-28s %.1f-%.1f MHz: %zu clocks, mean %+.2f +- %.2f", row->id.c_str(), row->link_low_hz / 1e6,
                    row->link_high_hz / 1e6, row->link_clocks.size(), m ? m->ppm : NAN, m ? m->sigma_ppm : NAN);
        if (f) std::printf("; frame timing %+.3f +- %.3f ppm over %zu captures, %.0f s, %.1f us rms", f->ppm, f->sigma_ppm,
                           f->connected, f->span_s, f->rms_s * 1e6);
        else if (m && row->frame_samples.size() >= 2) {
            // Why not: the raw fit with the same prior.
            if (const auto raw = frame_timing::fit(row->frame_samples, m->ppm, std::max(2.5 * m->sigma_ppm, 1.0)))
                std::printf("; frame timing not used: %zu of %zu on the line, %.0f s, %.1f us rms, peak %.2f alias %.2f",
                            raw->connected, raw->used, raw->span_s, raw->rms_s * 1e6, raw->peak, raw->alias);
        }
        std::printf("\n");
    }
    for (std::size_t i = 0; i < lb.size(); ++i)
        for (std::size_t j = i + 1; j < lb.size(); ++j) {
            const auto c = drone_claims::crystal_match(*lb[i], *lb[j]);
            static const char* kinds[] = {"undecided", "possibly same", "likely same", "different"};
            std::printf("    %s vs %s: %s%s%s\n", lb[i]->id.c_str(), lb[j]->id.c_str(), kinds[c.kind],
                        c.reason.empty() ? "" : ": ", c.reason.c_str());
        }
}

static int session_main(const std::string& dir, bool rows_replay, const std::vector<double>& skip_mhz,
                        double shift_mhz, double shift_us) {
    drone_claims::Store store;
    const auto base = std::chrono::steady_clock::now();
    double first_device_s = NAN;
    std::uint64_t seq = 0;
    int last_cycle = -1;
    auto last_at = base;
    double last_t = 0;
    std::ifstream js(dir + "/session.json");
    if (!js) { std::fprintf(stderr, "cannot open %s/session.json\n", dir.c_str()); return 1; }
    const auto session = nlohmann::json::parse(js);
    const double rate = session.at("rate_hz");
    symbol_timing::Probe probe;
    std::vector<frame_timing::Sample> samples;
    double w = 0, wp = 0;
    std::size_t ocusync = 0, ocusync_mixed = 0, ocusync_clock = 0;
    for (const auto& v : session.at("visits")) {
        if (v.at("status") != "ok" || !v.at("device_ns").is_number()) continue;
        if (std::any_of(skip_mhz.begin(), skip_mhz.end(), [&](double f) {
                return std::abs(f * 1e6 - v.at("centre_hz").get<double>()) < 1e5; })) continue;
        const std::size_t n = v.at("samples");
        const std::size_t count = std::min(symbol_timing::max_blocks, n / symbol_timing::block_size);
        std::vector<std::complex<float>> blocks(count * symbol_timing::block_size);
        std::array<std::size_t, symbol_timing::max_blocks> starts{};
        std::ifstream in(dir + "/" + v.at("file").get<std::string>(), std::ios::binary);
        if (!in || !read_blocks(in, 0, n, count, blocks, starts)) { std::fprintf(stderr, "cannot read %s\n", v.at("file").get<std::string>().c_str()); return 1; }
        symbol_timing::Result r;
        r.rate = rate; r.center = v.at("centre_hz"); r.blocks = count; r.capture_samples = n;
        r.device_ns = v.at("device_ns");
        if (shift_mhz > 0 && std::abs(shift_mhz * 1e6 - r.center) < 1e5) r.device_ns += std::llround(shift_us * 1e3);
        if (!symbol_timing::measure_blocks(probe, blocks.data(), r)) continue;
        if (rows_replay) {
            if (std::isnan(first_device_s)) first_device_s = r.device_ns * 1e-9;
            const double t = r.device_ns * 1e-9 - first_device_s;
            const int cycle = v.value("cycle", 0);
            const auto at = base + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(t));
            if (cycle != last_cycle && last_cycle >= 0) print_rows(store, at - std::chrono::milliseconds(1), t);
            last_cycle = cycle;
            r.run_id = "sweep"; r.radio_session = 1; r.capture_seq = ++seq; r.observed = at;
            store.timing(r);
            last_at = at;
            last_t = t;
        }
        const std::string group = r.timing_group ? r.timing_group : "-";
        if (group.find("15kHz") != std::string::npos) {
            ++ocusync;
            if (r.fractions[symbol_timing::lightbridge] > 0 || r.fractions[symbol_timing::lightbridge_third] > 0) ++ocusync_mixed;
            if (r.link_clock) ++ocusync_clock;
        }
        const double device_s = r.device_ns * 1e-9;
        std::printf("%s %8.1f MHz t=%8.3f s group=%-12s", v.at("file").get<std::string>().c_str(), r.center / 1e6,
                    device_s, group.c_str());
        if (const auto& e = r.link_clock) {
            std::printf(" %s %+7.2f +-%.2f", link_clock::name(e->link),
                        e->ppm, e->sigma_ppm);
            if (e->has_symbol_phase && link_clock::usable(*e)) {
                std::printf("  grid %6.2f us", frame_timing::grid_position(device_s, e->symbol_phase_s) * 1e6);
                samples.push_back({"sweep", 1, device_s, e->symbol_phase_s});
                w += 1 / (e->sigma_ppm * e->sigma_ppm);
                wp += e->ppm / (e->sigma_ppm * e->sigma_ppm);
            }
        }
        std::printf("\n");
    }
    if (rows_replay && !std::isnan(first_device_s)) {
        std::printf("\nrows after the last visit:\n");
        print_rows(store, last_at, last_t);
    }
    std::printf("\nOcuSync captures %zu: %zu also showed Lightbridge timing (no OcuSync clock), %zu with a clock\n",
                ocusync, ocusync_mixed, ocusync_clock);
    if (!(w > 0)) { std::printf("no usable Lightbridge captures\n"); return 0; }
    const double prior = wp / w, prior_sigma = 1 / std::sqrt(w);
    std::printf("Lightbridge: %zu usable captures, mean single-capture clock %+.3f +- %.3f ppm\n", samples.size(), prior,
                prior_sigma);
    // Also from a weak prior, as a row would have after its first captures.
    for (const double ps : {prior_sigma, 2.5}) {
        const auto f = frame_timing::fit(samples, prior, ps);
        if (!f) continue;
        std::printf("frame timing (prior sigma %.2f): %zu of %zu connected over %.1f s, rms %.2f us, clock %+.4f +- %.4f ppm%s\n",
                    ps, f->connected, f->used, f->span_s, f->rms_s * 1e6, f->ppm, f->sigma_ppm, f->good() ? "" : " (not good)");
    }
    return 0;
}

static int offset_log_main(const std::string& path, std::size_t from_line) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    struct Capture { double device_s; frame_timing::Sample sample; drone_claims::LinkClockSample clock; bool low_band; };
    std::vector<Capture> caps;
    std::string line;
    for (std::size_t n = 1; std::getline(in, line); ++n) {
        if (n < from_line) continue;
        const auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || j.value("kind", "") != "link" || !j.contains("link_clock") || !j["group"].is_string() ||
            j["group"] != "32.8kHz") continue;
        const auto& c = j["link_clock"];
        if (!c.value("usable", false) || !c.contains("symbol_phase_s")) continue;
        const double device_s = j.at("device_ns").get<double>() * 1e-9;
        caps.push_back({device_s, {j.at("run"), j.at("session"), device_s, c.at("symbol_phase_s")},
                        {c.at("ppm"), c.at("sigma_ppm"), true, j.at("center_hz"), {}}, j.at("center_hz").get<double>() < 3e9});
    }
    for (const bool low : {true, false}) {
        std::printf("%s GHz Lightbridge captures:\n", low ? "2.4" : "5");
        for (std::size_t i = 0; i < caps.size(); ++i) {
            if (caps[i].low_band != low) continue;
            std::vector<frame_timing::Sample> s;
            std::vector<drone_claims::LinkClockSample> clocks;
            for (std::size_t k = 0; k <= i; ++k)
                if (caps[k].low_band == low && caps[i].device_s - caps[k].device_s <= std::chrono::duration<double>(drone_claims::frame_lifetime).count() &&
                    caps[k].sample.run == caps[i].sample.run && caps[k].sample.radio_session == caps[i].sample.radio_session) {
                    s.push_back(caps[k].sample);
                    clocks.push_back(caps[k].clock);
                }
            const auto m = drone_claims::link_clock_mean(clocks);
            std::printf("  t=%8.1f s %7.1f MHz reading %+6.2f; mean %+6.2f over %zu", caps[i].device_s, caps[i].clock.centre_hz / 1e6,
                        caps[i].clock.ppm, m ? m->ppm : NAN, clocks.size());
            if (m)
                if (const auto f = frame_timing::fit(s, m->ppm, std::max(2.5 * m->sigma_ppm, drone_claims::frame_prior_min_sigma_ppm)))
                    std::printf("; frame timing %+.3f +- %.3f ppm, %zu of %zu on the line, %.0f s, %.1f us, margin %.2f%s",
                                f->ppm, f->sigma_ppm, f->connected, f->used, f->span_s, f->rms_s * 1e6, f->peak - f->alias,
                                f->good() ? " GOOD" : "");
            std::printf("\n");
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--offset-log")
        return offset_log_main(argv[2], argc >= 5 && std::string(argv[3]) == "--from-line" ? std::stoul(argv[4]) : 1);
    if (argc >= 3 && std::string(argv[1]) == "--session") {
        bool rows = false;
        std::vector<double> skip;
        double shift_mhz = 0, shift_us = 0;
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--rows") rows = true;
            else if (a == "--shift-mhz" && i + 1 < argc) shift_mhz = std::stod(argv[++i]);
            else if (a == "--shift-us" && i + 1 < argc) shift_us = std::stod(argv[++i]);
            else if (a == "--skip-mhz") while (i + 1 < argc && argv[i + 1][0] != '-') skip.push_back(std::stod(argv[++i]));
            else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
        }
        return session_main(argv[2], rows, skip, shift_mhz, shift_us);
    }
    double rate = 20e6, visit = .512, center = 2.44e9;
    std::optional<link_clock::Link> forced;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&] { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); } return std::string(argv[++i]); };
        if (a == "--rate") rate = std::stod(next());
        else if (a == "--visit") visit = std::stod(next());
        else if (a == "--center") center = std::stod(next());
        else if (a == "--link") {
            const auto v = next();
            if (v == "lightbridge") forced = link_clock::Link::lightbridge;
            else if (v == "ocusync_15k") forced = link_clock::Link::ocusync_15k;
            else if (v == "ocusync_30k") forced = link_clock::Link::ocusync_30k;
            else { std::fprintf(stderr, "unknown link %s\n", v.c_str()); return 2; }
        } else files.push_back(a);
    }
    if (files.empty()) { std::fprintf(stderr, "usage: %s FILE... [--rate R] [--visit S] [--link L]\n", argv[0]); return 2; }
    symbol_timing::Probe probe;
    const auto visit_samples = static_cast<std::size_t>(visit * rate);
    const std::size_t count = std::min(symbol_timing::max_blocks, visit_samples / symbol_timing::block_size);
    std::vector<std::complex<float>> blocks(count * symbol_timing::block_size);
    std::vector<int16_t> raw(symbol_timing::block_size * 2);
    for (const auto& path : files) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
        const std::size_t total = static_cast<std::size_t>(in.tellg()) / 4;
        const char* name = std::strrchr(path.c_str(), '/') ? std::strrchr(path.c_str(), '/') + 1 : path.c_str();
        for (std::size_t first = 0; first + visit_samples <= total; first += visit_samples) {
            std::array<std::size_t, symbol_timing::max_blocks> starts{};
            for (std::size_t b = 0; b < count; ++b) {
                starts[b] = symbol_timing::block_start(visit_samples, b, count);
                in.seekg(static_cast<std::streamoff>((first + starts[b]) * 4));
                in.read(reinterpret_cast<char*>(raw.data()), raw.size() * sizeof(int16_t));
                for (std::size_t k = 0; k < symbol_timing::block_size; ++k)
                    blocks[b * symbol_timing::block_size + k] = {raw[2 * k] / 32768.f, raw[2 * k + 1] / 32768.f};
            }
            std::optional<link_clock::Estimate> e;
            const char* group = "-";
            if (forced) {
                e = link_clock::measure(*forced, blocks.data(), symbol_timing::block_size, starts.data(), count, rate);
            } else {
                symbol_timing::Result r;
                r.rate = rate; r.center = center; r.blocks = count; r.capture_samples = visit_samples;
                if (!symbol_timing::measure_blocks(probe, blocks.data(), r)) { std::printf("%s t=%5.2fs invalid IQ\n", name, first / rate); continue; }
                group = r.timing_group ? r.timing_group : "-";
                e = r.link_clock;
            }
            if (e)
                std::printf("%s t=%5.2fs group=%s link=%s ppm=%+8.3f +-%.3f%s\n", name, first / rate, group,
                            link_clock::name(e->link), e->ppm, e->sigma_ppm,
                            link_clock::usable(*e) ? "" : " (not usable)");
            else
                std::printf("%s t=%5.2fs group=%s no estimate\n", name, first / rate, group);
        }
    }
}
