// Training recorder (drones/labelled_recorder.hpp): which captures are labelled, and the saved
// session (files, sizes, sample scaling, session.json fields read by drone_session_replay).
#include "drones/labelled_recorder.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace rfmon;
using namespace rfmon::drone_claims;
namespace fs = std::filesystem;

static void require(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
    std::cout << "PASS " << why << '\n';
}

static Detection confirmed(const std::string& id, const std::string& model, std::vector<uint64_t> links, uint64_t packet) {
    Detection d;
    d.id = id; d.band = "5 GHz"; d.group = "15kHz";
    d.link_low_hz = d.link_high_hz = 5778e6;
    for (auto c : links) d.link_captures.push_back({"run", 1, c, 5778e6});
    Packet p;
    p.model = model; p.serial = "SER-" + model; p.readable = true; p.frequency = 5778e6;
    d.packet = p;
    d.packet_key = {"run", 1, packet, 5778e6};
    return d;
}

int main() try {
    std::map<std::string, Detection> rows;
    rows["a"] = confirmed("a", "Mini 2", {10, 11}, 12);
    Detection unreadable = confirmed("b", "", {13}, 14);
    unreadable.packet->readable = false;
    rows["b"] = unreadable;
    std::size_t ambiguous = 0;
    auto chosen = labelled_captures(rows, &ambiguous);
    require(chosen.size() == 3 && chosen[0].key.capture == 10 && chosen[0].evidence == "link" &&
                chosen[2].key.capture == 12 && chosen[2].evidence == "droneid" && chosen[0].model == "Mini 2",
            "Labelled: link and DroneID captures of a row DroneID names; unreadable rows give none");
    rows["c"] = confirmed("c", "Air 3S", {11, 16}, 15);
    chosen = labelled_captures(rows, &ambiguous);
    bool has11 = false;
    for (const auto& c : chosen) has11 = has11 || c.key.capture == 11;
    require(!has11 && ambiguous == 1 && chosen.size() == 4, "Labelled: a capture two models claim is skipped");

    char tmp[] = "/tmp/rfmon-labelled-XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    LabelledRecorder::Limits limits;
    limits.min_free_bytes = 0;
    limits.max_session_bytes = 1 << 20;
    LabelledRecorder recorder(limits);
    require(recorder.start(tmp) && recorder.recording(), "Recorder: starts a session folder");
    std::size_t fetched = 0;
    const auto fetch = [&](uint64_t seq) -> std::optional<LabelledRecorder::Retained> {
        ++fetched;
        if (seq == 15) return std::nullopt;  // already evicted
        LabelledRecorder::Retained r;
        r.record.run_id = "run"; r.record.capture_seq = seq; r.record.radio_session = 1;
        r.record.sample_rate_hz = 20e6; r.record.actual_rf_hz = 5780e6; r.record.actual_dsp_hz = 2e6;
        r.record.capture_center_hz = 5778e6; r.record.host_before_ns = 1795000000000000000LL; r.record.band = "5 GHz";
        auto iq = std::make_shared<std::vector<std::complex<float>>>(1000, std::complex<float>(.5f, -1.f));
        r.iq = iq;
        return r;
    };
    recorder.offer(rows, fetch);
    recorder.offer(rows, fetch);  // the same rows again: nothing new
    recorder.stop();
    const auto st = recorder.status();
    require(st.saved == 3 && st.missed == 1 && st.per_model.at("Mini 2") == 2 && st.per_model.at("Air 3S") == 1 &&
                fetched == 4 && st.bytes == 3 * 4000,
            "Recorder: each capture saved once; an evicted one counted as missed");
    std::ifstream in(fs::path(st.dir) / "session.json");
    const auto session = nlohmann::json::parse(in);
    require(session.at("rate_hz") == 20e6 && session.at("captures").size() == 3 && session.at("run_id") == "run",
            "Recorder: session.json has the rate and every capture");
    const auto& c0 = session.at("captures")[0];
    require(c0.at("status") == "ok" && c0.at("centre_hz") == 5778e6 && c0.at("samples") == 1000 &&
                c0.at("model") == "Mini 2" && c0.at("serial") == "SER-Mini 2" && c0.at("evidence") == "link" &&
                c0.at("host_start").get<std::string>().rfind("2026-11-18T", 0) == 0 &&
                c0.at("host_start").get<std::string>().find("+00:00") != std::string::npos,
            "Recorder: capture entry carries the replay fields (file, status, centre, samples, host_start) and the label");
    const auto file = fs::path(st.dir) / c0.at("file").get<std::string>();
    require(fs::file_size(file) == 4000, "Recorder: one sc16 file of 4 bytes per sample");
    std::ifstream raw(file, std::ios::binary);
    int16_t iqv[2];
    raw.read(reinterpret_cast<char*>(iqv), sizeof iqv);
    require(iqv[0] == 16384 && iqv[1] == -32768, "Recorder: samples scaled to int16 full scale 32768");

    // Size limit: the next capture would pass it, so recording stops with a reason.
    LabelledRecorder::Limits tight = limits;
    tight.max_session_bytes = 6000;
    LabelledRecorder small(tight);
    require(small.start(tmp), "Recorder: second session starts");
    small.offer(rows, fetch);
    small.stop();
    require(small.status().saved == 1 && !small.status().error.empty() && !small.recording(),
            "Recorder: stops with a reason at the session size limit");
    fs::remove_all(tmp);
    std::cout << "ALL PASSED\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
}
