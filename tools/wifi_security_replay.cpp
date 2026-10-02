// Offline runner for the passive Wi-Fi security monitor (package A step 4).
//
// Feeds inputs through the same SecurityState the live scanner's monitor
// uses, with no radio, no live identity writes and no GUI:
//   *.ndjson / *.ndjson.1  a monitor recording (Scanner::set_wifi_security_recording)
//   *.json                 a receive-only IQ manifest (wifi_capture_cli), decoded
//                          with the scanner's own per-burst pipeline
//                          (wifi_burst_pipeline.hpp)
// and prints the resulting snapshot as JSON. Output depends only on the
// inputs and their order - run it twice, get the same bytes - unless
// --measure-processing asks for wall-clock processing times.
//
//   wifi_security_replay [--recent] [--frames OUT.ndjson] [--record OUT.ndjson]
//                        [--threshold-db DB] [--measure-processing] INPUT...
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "config.hpp"
#include "security/wifi_security_io.hpp"
#include "security/wifi_security_state.hpp"
#include "wifi_burst_pipeline.hpp"
#include "security/wifi_iq_input.hpp"

using nlohmann::json;
using namespace rfmon;
using namespace rfmon::wifi_security;

namespace {

struct Options {
    bool recent = false, measure = false;
    std::string frames_path, record_path, export_state_path;
    double threshold_db = DEFAULT_DETECTION_THRESHOLD_DB;
};

struct Sinks {
    std::ofstream frames, record;
};

void emit_frame(Sinks& sinks, const SecurityState& state) {
    if (!sinks.frames.is_open()) return;
    const ProcessedFrame& p = state.snapshot().recent.back();
    json f = to_json(p.event);
    f["summary"] = describe(p.frame);
    f["repeated_content"] = p.repeated_content;
    sinks.frames << f.dump() << "\n";
}

void ingest_frame(SecurityState& state, Sinks& sinks, const FrameEvent& e) {
    if (sinks.record.is_open()) sinks.record << to_json(e).dump() << "\n";
    if (state.ingest(e)) emit_frame(sinks, state);
}

void replay_recording(const std::filesystem::path& path, SecurityState& state, Sinks& sinks) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            json j = json::parse(line);
            const std::string kind = j.value("kind", "");
            if (kind == "frame") ingest_frame(state, sinks, frame_from_json(j));
            else if (kind == "capture") {
                CaptureRecord c = capture_from_json(j);
                if (sinks.record.is_open()) sinks.record << to_json(c).dump() << "\n";
                state.ingest(c);
            } else if (kind == "loss") {
                state.ingest(loss_from_json(j));
                if (sinks.record.is_open()) sinks.record << j.dump() << "\n";
            } else if (kind == "input_rejected") {
                state.note_input_rejected();
                if (sinks.record.is_open()) sinks.record << j.dump() << "\n";
            }
            else if (kind == "command") {
                ControlCommand c = command_from_json(j);
                if (sinks.record.is_open()) sinks.record << to_json(c).dump() << "\n";
                state.ingest(c);
            }
            else if (kind != "header") throw std::runtime_error("unknown record kind");
        } catch (const std::exception&) {
            state.note_input_rejected();
            if (sinks.record.is_open()) sinks.record << "{\"kind\":\"input_rejected\"}\n";
        }
    }
}

void replay_iq(const std::filesystem::path& manifest, uint64_t seq, SecurityState& state, Sinks& sinks,
               const Options& opt) {
    auto input=load_wifi_iq_capture(manifest.string());
    const auto& iq=input.iq;
    const size_t n=iq.size();
    CaptureRecord rec=wifi_iq_record(input,manifest.filename().string(),seq);
    const double rate=rec.sample_rate_hz,center=rec.capture_center_hz,channel=rec.channel_hz;

    const auto t0 = std::chrono::steady_clock::now();
    const bool try_dsss = rec.band == BAND_WIFI_2G4;
    // Same limits and pipeline as the scanner: detect up to the security
    // limit, process in parallel, results in burst order.
    auto bursts = wifi::detect_bursts(iq.data(), iq.size(), rate, opt.threshold_db, WIFI_SECURITY_MAX_BURSTS_PER_CAPTURE);
    rec.bursts_detected = bursts.size();
    rec.burst_cap = WIFI_SECURITY_MAX_BURSTS_PER_CAPTURE;
    rec.burst_cap_reached = bursts.size() >= WIFI_SECURITY_MAX_BURSTS_PER_CAPTURE;
    rec.analysed_samples = rec.burst_cap_reached && !bursts.empty() ? bursts.back().start + bursts.back().length : n;
    const auto works = wifi::process_bursts(iq.data(), iq.size(), bursts, rate, center, channel, try_dsss);
    for (size_t bi = 0; bi < bursts.size(); ++bi) {
        const auto& b = bursts[bi];
        const wifi::BurstWork& w = works[bi];
        const auto& cls = w.cls;
        if (cls.outcome == wifi::BurstClassification::Outcome::Unknown) { ++rec.bursts_unknown; continue; }
        if (cls.outcome == wifi::BurstClassification::Outcome::Narrowband) { ++rec.bursts_narrowband; continue; }
        if (bi >= WIFI_MAX_BURSTS_PER_CAPTURE) ++rec.bursts_beyond_identity_limit;
        const std::vector<uint8_t>* mpdu = nullptr;
        std::string phy;
        int rate_mbps = 0;
        bool sec_only = false;
        if (w.ofdm) {
            ++rec.bursts_ofdm;
            ++rec.ofdm_decode_attempts;
            if (w.ofdm->fcs_valid) { ++rec.ofdm_fcs_valid; mpdu = &w.ofdm->mpdu; phy = "OFDM"; rate_mbps = w.ofdm->rate_mbps; }
        } else {
            ++rec.bursts_dsss;
            if (!w.dsss.policy.decode) { ++rec.dsss_not_attempted; continue; }
            ++rec.dsss_decode_attempts;
            if (w.dsss.policy.security_only) ++rec.dsss_security_only_attempts;
            if (w.dsss.result->fcs_valid) {
                ++rec.dsss_fcs_valid;
                mpdu = &w.dsss.result->mpdu; phy = "DSSS"; rate_mbps = 1; sec_only = w.dsss.policy.security_only;
            }
        }
        if (!mpdu) continue;
        FrameEvent e = make_frame_event(rec, b.start, b.length, phy, rate_mbps, sec_only, *mpdu, true);
        e.power_db = cls.power_db;
        e.bandwidth_hz = cls.bandwidth_hz;
        e.duration_us = cls.duration_s * 1e6;
        e.confidence = cls.result.confidence;
        ingest_frame(state, sinks, e);
        ++rec.events_submitted;
    }
    rec.processed = true;
    if (opt.measure) rec.processing_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (sinks.record.is_open()) sinks.record << to_json(rec).dump() << "\n";
    state.ingest(rec);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options opt;
        std::vector<std::filesystem::path> inputs;
        for (int a = 1; a < argc; ++a) {
            std::string s = argv[a];
            auto value = [&]() -> std::string {
                if (a + 1 >= argc) throw std::runtime_error("missing value for " + s);
                return argv[++a];
            };
            if (s == "--recent") opt.recent = true;
            else if (s == "--measure-processing") opt.measure = true;
            else if (s == "--frames") opt.frames_path = value();
            else if (s == "--record") opt.record_path = value();
            else if (s == "--export-state") opt.export_state_path = value();
            else if (s == "--threshold-db") opt.threshold_db = std::stod(value());
            else if (s == "-h" || s == "--help") {
                std::cout << "usage: wifi_security_replay [--recent] [--frames OUT.ndjson] [--record OUT.ndjson]\n"
                             "       [--export-state OUT.json] [--threshold-db DB] [--measure-processing] INPUT...\n"
                             "INPUT: monitor recording (.ndjson[.1]) or IQ manifest (.json)\n";
                return 0;
            } else inputs.emplace_back(s);
        }
        if (inputs.empty()) throw std::runtime_error("no inputs (see --help)");
        if (!std::isfinite(opt.threshold_db) || opt.threshold_db < 0 || opt.threshold_db > 50)
            throw std::runtime_error("invalid threshold");
        Sinks sinks;
        for (auto [path, stream] : {std::pair{&opt.frames_path, &sinks.frames}, std::pair{&opt.record_path, &sinks.record}}) {
            if (path->empty()) continue;
            if (std::filesystem::exists(*path)) throw std::runtime_error(*path + " already exists");
            stream->open(*path);
            if (!*stream) throw std::runtime_error("cannot open " + *path);
        }
        if (sinks.record.is_open()) sinks.record << header_json("offline", "wifi_security_replay").dump() << "\n";

        SecurityState state;
        uint64_t seq = 0;
        for (const auto& in : inputs) {
            const std::string name = in.filename().string();
            const bool recording = name.find(".ndjson") != std::string::npos;
            if (recording) replay_recording(in, state, sinks);
            else replay_iq(in, ++seq, state, sinks, opt);
        }
        std::cout << snapshot_json(state.snapshot(), opt.recent).dump(2) << "\n";
        if (!opt.export_state_path.empty()) {
            // Baselines/incidents learned offline, in the live monitor's state.json format.
            std::ofstream out(opt.export_state_path);
            out << state.export_persistent().dump() << "\n";
            if (!out) throw std::runtime_error("cannot write " + opt.export_state_path);
        }
        for (auto* s : {&sinks.frames, &sinks.record})
            if (s->is_open() && !s->flush()) throw std::runtime_error("output write failed");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "wifi_security_replay: " << e.what() << "\n";
        return 1;
    }
}
