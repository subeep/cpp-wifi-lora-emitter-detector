// Offline check of the carrier-offset measurements on a recorded X310 session. No radio.
//
// For each clean capture of a session (background_rotate.py "captures", the Air 3S recorder's
// "segments", or a training-recorder session), it runs the live link-timing measurement
// (symbol_timing::measure_blocks, the same 24 blocks the app takes) and prints the grid position
// of every flagged spacing. With --droneid it also decodes DJI DroneID at each documented channel
// in the capture and prints its offset in Hz and ppm.
//
// The summary groups link grids by capture centre and spacing (circular mean and spread, since a
// grid position wraps at one spacing) and DroneID offsets by serial and band. A steady transmitter
// gives a small spread; that is what this checks.
//
//   drone_carrier_offset_check --session DIR [--droneid] [--max N] [--min-coherence C] [--json OUT]

#include "drones/symbol_timing_monitor.hpp"
#ifdef RFMON_CARRIER_CHECK_DRONEID
#include "drones/droneid_packet.hpp"
#include "droneid/dji.h"
#endif
#include <nlohmann/json.hpp>

#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace rfmon;

namespace {
struct Circular {  // values on a circle of one spacing
    double period{};
    std::complex<double> sum{};
    std::size_t count{};
    void add(double v) { sum += std::polar(1., 2 * M_PI * v / period); ++count; }
    double mean() const { double m = std::arg(sum) / (2 * M_PI) * period; return m < 0 ? m + period : m; }
    // Circular standard deviation in the same units: sqrt(-2 ln R) * period / 2 pi.
    double spread() const {
        const double r = std::min(1., std::abs(sum) / count);
        return r > 0 ? std::sqrt(-2 * std::log(r)) * period / (2 * M_PI) : INFINITY;
    }
};
struct Linear {
    double sum{}, squares{};
    std::size_t count{};
    void add(double v) { sum += v; squares += v * v; ++count; }
    double mean() const { return sum / count; }
    double spread() const { return count > 1 ? std::sqrt(std::max(0., (squares - sum * sum / count) / (count - 1))) : 0; }
};
}

int main(int argc, char** argv) try {
    fs::path session;
    std::string json_out;
    bool droneid = false;
    std::size_t max = 0;
    double min_coherence = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if (a == "--session") session = next();
        else if (a == "--json") json_out = next();
        else if (a == "--droneid") droneid = true;
        else if (a == "--max") max = std::stoul(next());
        else if (a == "--min-coherence") min_coherence = std::stod(next());
        else throw std::runtime_error("unknown option " + a);
    }
    if (session.empty()) throw std::runtime_error(
        "usage: drone_carrier_offset_check --session DIR [--droneid] [--max N] [--min-coherence C] [--json OUT]");
#ifndef RFMON_CARRIER_CHECK_DRONEID
    if (droneid) throw std::runtime_error("built without the DJI decoder (RFMON_ENABLE_DJI_DRONEID)");
#endif
    std::ifstream meta_in(session / "session.json");
    if (!meta_in) throw std::runtime_error("no session.json in " + session.string());
    const auto meta = Json::parse(meta_in);
    const double rate = meta.at("rate_hz").get<double>();
    const auto& list = meta.contains("captures") ? meta.at("captures") : meta.at("segments");

    symbol_timing::Probe probe;
    std::vector<std::complex<float>> blocks(symbol_timing::max_blocks * symbol_timing::block_size);
    std::map<std::pair<long long, std::string>, Circular> grids;     // (centre kHz, hypothesis)
    std::map<std::pair<std::string, int>, Linear> offsets;           // (serial, 0 = 2.4 / 1 = 5 GHz)
    Json rows = Json::array();
    std::size_t done = 0, skipped = 0;
    for (const auto& c : list) {
        if (max && done >= max) break;
        if (c.value("status", "ok") != "ok") { ++skipped; continue; }
        const fs::path file = session / c.at("file").get<std::string>();
        const double centre = c.contains("actual_rf_hz") && c.contains("actual_dsp_hz")
            ? c["actual_rf_hz"].get<double>() - c["actual_dsp_hz"].get<double>() : c.at("centre_hz").get<double>();
        std::ifstream in(file, std::ios::binary);
        if (!in) { ++skipped; continue; }
        in.seekg(0, std::ios::end);
        const std::size_t n = static_cast<std::size_t>(in.tellg()) / 4;
        in.seekg(0);
        std::vector<int16_t> raw(2 * n);
        in.read(reinterpret_cast<char*>(raw.data()), raw.size() * 2);
        std::vector<std::complex<float>> iq(n);
        for (std::size_t i = 0; i < n; ++i) iq[i] = {raw[2 * i] / 32768.f, raw[2 * i + 1] / 32768.f};
        if (n < symbol_timing::block_size || !symbol_timing::supported_rate(rate)) { ++skipped; continue; }

        symbol_timing::Result r;
        r.center = centre;
        r.rate = rate;
        r.blocks = std::min(symbol_timing::max_blocks, n / symbol_timing::block_size);
        r.capture_samples = n;
        for (std::size_t b = 0; b < r.blocks; ++b)
            std::copy_n(iq.data() + symbol_timing::block_start(n, b, r.blocks), symbol_timing::block_size,
                        blocks.data() + b * symbol_timing::block_size);
        if (!symbol_timing::measure_blocks(probe, blocks.data(), r)) { ++skipped; continue; }
        Json row{{"file", file.filename().string()}, {"centre_hz", centre},
                 {"group", r.timing_group ? Json(r.timing_group) : Json(nullptr)}, {"grid", Json::array()}};
        if (c.contains("model")) row["label_model"] = c["model"];
        if (const auto& lc = r.link_clock)
            row["link_clock"] = {{"link", link_clock::name(lc->link)},
                                 {"ppm", lc->ppm}, {"sigma_ppm", lc->sigma_ppm}, {"usable", link_clock::usable(*lc)}};
        for (std::size_t i = 0; i < r.grid.size(); ++i) {
            const auto& g = r.grid[i];
            if (!g) continue;
            row["grid"].push_back({{"hypothesis", symbol_timing::hypotheses[i].name}, {"hz", g->hz},
                                   {"coherence", g->coherence}, {"blocks", g->blocks}});
            if (g->coherence < min_coherence) continue;
            auto& acc = grids[{std::llround(centre / 1e3), symbol_timing::hypotheses[i].name}];
            acc.period = g->spacing_hz;
            acc.add(g->hz);
        }
#ifdef RFMON_CARRIER_CHECK_DRONEID
        if (droneid) {
            row["droneid"] = Json::array();
            std::vector<double> channels;
            for (double ch : drone::droneid::channels_24) if (std::abs(ch - centre) <= rate * .45 - 5e6) channels.push_back(ch);
            for (double ch : drone::droneid::channels_58) if (std::abs(ch - centre) <= rate * .45 - 5e6) channels.push_back(ch);
            for (double ch : channels) {
                droneid::dji::DecodeOptions o;
                o.sample_rate = rate;
                o.freq_offset = centre - ch;  // as the live worker: that channel to baseband
                for (const auto& burst : droneid::dji::decode_samples(iq, o)) {
                    if (!burst.crc_ok) continue;
                    droneid::dji::Telemetry t;
                    const bool parsed = droneid::dji::parse_frame(burst.payload, t);
                    const auto info = drone::droneid::classify(burst.payload, droneid::dji::inspect_packet(burst.payload).format);
                    const double ppm = burst.cfo_hz / ch * 1e6;
                    const std::string serial = parsed && !t.serial.empty() ? t.serial : "(unreadable)";
                    row["droneid"].push_back({{"channel_hz", ch}, {"offset_hz", burst.cfo_hz}, {"ppm", ppm},
                                              {"serial", serial}, {"model", info.model}, {"sample", burst.sample_index}});
                    offsets[{serial, ch >= 3e9}].add(ppm);
                }
            }
        }
#endif
        std::cout << row.dump() << '\n';
        rows.push_back(std::move(row));
        ++done;
    }

    std::printf("\n%zu captures measured, %zu skipped\n", done, skipped);
    std::printf("\nLink grid by capture centre and spacing (coherence >= %.2f): captures, circular mean, spread\n", min_coherence);
    Json summary_grids = Json::array();
    for (const auto& [key, acc] : grids) {
        std::printf("  %9.3f MHz  %-18s %4zu  %9.1f Hz  %8.1f Hz  (period %.1f kHz)\n", key.first / 1e3,
                    key.second.c_str(), acc.count, acc.mean(), acc.spread(), acc.period / 1e3);
        summary_grids.push_back({{"centre_hz", key.first * 1e3}, {"hypothesis", key.second}, {"captures", acc.count},
                                 {"mean_hz", acc.mean()}, {"spread_hz", acc.spread()}, {"period_hz", acc.period}});
    }
    Json summary_offsets = Json::array();
    if (droneid) {
        std::printf("\nDroneID offset by serial and band: packets, mean ppm, spread ppm\n");
        for (const auto& [key, acc] : offsets) {
            std::printf("  %-20s %-7s %4zu  %+9.4f  %8.4f\n", key.first.c_str(), key.second ? "5 GHz" : "2.4 GHz",
                        acc.count, acc.mean(), acc.spread());
            summary_offsets.push_back({{"serial", key.first}, {"band", key.second ? "5 GHz" : "2.4 GHz"},
                                       {"packets", acc.count}, {"mean_ppm", acc.mean()}, {"spread_ppm", acc.spread()}});
        }
    }
    if (!json_out.empty()) {
        std::ofstream out(json_out);
        out << Json{{"session", session.string()}, {"captures", rows}, {"link_grids", summary_grids},
                    {"droneid_offsets", summary_offsets}}.dump(1) << '\n';
        if (!out) throw std::runtime_error("could not write " + json_out);
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
