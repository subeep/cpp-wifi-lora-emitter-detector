#include "cyclostationary/shadow_worker.hpp"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <regex>
#include <unistd.h>
#include <fcntl.h>
#include <sys/resource.h>

using namespace rfmon::cyclo;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f, const char* message) { try { f(); } catch (const std::exception&) { return; } throw std::runtime_error(message); }
template<class F> void until(F f) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!f()) { if (std::chrono::steady_clock::now() > end) throw std::runtime_error("process wait timed out"); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}
std::string read_file(const std::string& name) { std::ifstream f(name); return {std::istreambuf_iterator<char>(f), {}}; }
void protocol_tests(const std::vector<std::complex<float>>& iq) {
    const auto plan = plan_sample_tiles(iq.size());
    auto input = encode_request(171, 20e6, plan, iq); const auto decoded = decode_request(input);
    check(decoded.job_id == 171 && decoded.tiles.size() == 1 && decoded.tiles[0].iq == iq, "portable request round trip");
    auto overflowing = plan; overflowing.tiles[0].source_offset = std::numeric_limits<std::size_t>::max();
    rejects([&] { encode_request(171, 20e6, overflowing, iq); }, "overflowing source span encoded");
    auto malformed = input; std::fill_n(malformed.begin() + 20, 8, 255);
    rejects([&] { decode_request(malformed); }, "overflowing source span decoded");
    TileMeasurement measured; measured.source = plan.tiles[0];
    SpectralConfig c; c.sample_rate_hz = 20e6; c.max_frames = 256;
    measured.spectral = measure_spectral_correlation(iq, c);
    measured.ofdm = measure_ofdm_structure(iq, 20e6, wlan_ofdm_hypotheses(20e6));
    measured.roi = measure_rois(iq, 20e6);
    measured.chirps = measure_chirp_structure(iq, 20e6);
    const auto bytes = encode_result(171, {measured}); const auto restored = decode_result(bytes, 171, 20e6, plan);
    check(restored[0].spectral.peaks.size() == measured.spectral.peaks.size(), "validated result round trip");
    rejects([&] { decode_result(bytes, 172, 20e6, plan); }, "wrong job id accepted");
    auto wrong_plan = plan; ++wrong_plan.tiles[0].source_offset;
    rejects([&] { decode_result(bytes, 171, 20e6, wrong_plan); }, "wrong source coordinates accepted");
    auto bad = measured; bad.spectral.peaks[0].coherence_squared = 1.01;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "out-of-range coherence accepted");
    bad = measured; bad.spectral.mean_i = NAN;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "non-finite result accepted");
    bad = measured; bad.roi.regions[0].samples = iq.size() + 1;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "escaping ROI accepted");
    bad = measured; bad.roi.regions[0].spectral_samples = 1;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented PSD coverage accepted");
    bad = measured; bad.roi.high_threshold_to_mean += 1;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "inconsistent ROI threshold accepted");
    bad = measured; bad.roi.regions[0].energy_fraction = NAN;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "non-finite ROI energy accepted");
    bad = measured; bad.chirps[0].peak_offset = iq.size();
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "escaping chirp position accepted");
    bad = measured; bad.chirps[0].positions_examined += 1;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented chirp search coverage accepted");
    bad = measured; bad.chirps[0].slope_hz_per_second *= -1;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "unexpected chirp hypothesis accepted");
    bad = measured; bad.chirps[0].peak_coherence_squared = NAN;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "non-finite chirp result accepted");
    bad = measured; bad.chirps[0].status = "insufficient_samples";
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented chirp status accepted");
    bad = measured; bad.chirps[0].frequency_pairs = iq.size();
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented phase support accepted");
    bad = measured; bad.chirps[0].frequency_rmse_hz = INFINITY;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "non-finite frequency residual accepted");
    bad = measured; bad.chirps[0].frequency_mean_hz = NAN;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "non-finite frequency center accepted");
    auto tone = iq;
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = 0.001f * tone[i] + std::complex<float>(std::polar(1.0, 2 * 3.14159265358979323846 * 64 * (i % 512) / 512));
    auto with_band = measured; with_band.roi = measure_rois(tone, 20e6);
    check(!with_band.roi.regions[0].bands.empty(), "band validation fixture has no interval");
    decode_result(encode_result(171, {with_band}), 171, 20e6, plan);
    bad = with_band; bad.roi.regions[0].bands[0].low_hz = -20e6;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "outside-Nyquist interval accepted");
    bad = with_band; bad.roi.regions[0].bands[0].contains_dc = true;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented DC membership accepted");
    bad = with_band; bad.roi.regions[0].bands[0].edge_bin = true;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented edge flag accepted");
    for (std::size_t n : {std::size_t(0), std::size_t(8), bytes.size() - 1}) {
        auto truncated = bytes; truncated.resize(n);
        rejects([&] { decode_result(truncated, 171, 20e6, plan); }, "truncated result accepted");
    }
    auto tail = bytes; tail.push_back(0);
    rejects([&] { decode_result(tail, 171, 20e6, plan); }, "trailing result bytes accepted");
    auto header = encode_header(MessageType::result, reply_byte_limit + 1);
    rejects([&] { decode_header(header, reply_byte_limit); }, "oversized message header accepted");
    header = encode_header(MessageType::result, 0); header[4] = wire_version + 1;
    rejects([&] { decode_header(header, reply_byte_limit); }, "wrong wire version accepted");
    header[4] = 2;
    rejects([&] { decode_header(header, reply_byte_limit); }, "old worker wire version accepted");
    // Maximum retained ROI/band metadata plus full spectra across four tiles
    // must remain inside the unchanged reply budget and decode consistently.
    std::vector<std::complex<float>> dense(65536);
    for (std::size_t i = 0; i < dense.size(); ++i) {
        dense[i] = 0.001f * iq[i % iq.size()];
        if (i >= 2048 && ((i - 2048) / 4096) < 12 && (i - 2048) % 4096 < 1024)
            for (int bin : {-64, 32, 96})
                dense[i] += std::complex<float>(std::polar(1.0, 2 * 3.14159265358979323846 * bin * (i % 512) / 512));
    }
    TileMeasurement full; full.spectral = measure_spectral_correlation(dense, c);
    full.ofdm = measure_ofdm_structure(dense, 20e6, wlan_ofdm_hypotheses(20e6)); full.roi = measure_rois(dense, 20e6);
    full.chirps = measure_chirp_structure(dense, 20e6);
    check(full.roi.regions.size() == 8 && full.roi.regions[0].bands.size() == 3, "maximum metadata fixture not populated");
    const auto complete_plan = plan_sample_tiles(capture_sample_budget);
    std::vector<TileMeasurement> complete;
    for (std::size_t i = 0; i < complete_plan.count; ++i) { full.source = complete_plan.tiles[i]; complete.push_back(full); }
    const auto largest = encode_result(172, complete);
    check(largest.size() <= reply_byte_limit && decode_result(largest, 172, 20e6, complete_plan).size() == 4,
          "full ROI metadata exceeds bounded IPC profile");
    std::mt19937 rng(581);
    for (int trial = 0; trial < 500; ++trial) {
        WireBytes random(std::size_t(trial % 300)); for (auto& v : random) v = rng() & 255;
        rejects([&] { decode_result(random, 171, 20e6, plan); }, "random result accepted");
    }
}
void sampling_tests() {
    for (std::size_t size : {2048ul, 5000ul, 65536ul, 100003ul, 262144ul, 20000000ul,
                             std::numeric_limits<std::size_t>::max()}) {
        const auto p = plan_sample_tiles(size);
        check(p.count >= 1 && p.count <= 4 && p.samples <= 262144, "tile budget exceeded");
        std::size_t last_end = 0;
        for (std::size_t t = 0; t < p.count; ++t) {
            const auto& tile = p.tiles[t];
            check(tile.samples >= 2048 && tile.samples <= 65536 && tile.source_offset >= last_end &&
                  tile.source_offset <= size - tile.samples, "tile range overlaps or escapes source");
            last_end = tile.source_offset + tile.samples;
        }
        check(last_end == size, "last bounded tile misses capture end");
    }
    check(plan_sample_tiles(2047).count == 0, "short prefix must abstain");
    std::size_t calls = 0, last_index = 0;
    const auto capped = collect_burst_hints(4000, 20000000, true, [&](std::size_t i) {
        ++calls; last_index = i; return SampleTile{100000 + i * 4000, 12000};
    });
    check(calls == 512 && last_index == 3999 && capped.count == 512 && capped.detector_capped,
          "raw hints bounded and distributed over detector output");
    const auto extreme = collect_burst_hints(std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(), false, [](std::size_t i) { return SampleTile{i, 1}; });
    check(extreme.examined == 512 && extreme.count == 512, "descriptor sampling index overflow");
    const std::array<SampleTile, 5> gaps = {{{100, 0}, {9000, 2000}, {10000, 1},
        {std::numeric_limits<std::size_t>::max(), 100}, {2000, 1000}}};
    const auto valid = collect_burst_hints(gaps.size(), 10000, false, [&](std::size_t i) { return gaps[i]; });
    check(valid.count == 1 && valid.rejected == 4, "invalid or gap-crossing hints must be rejected");
    const auto short_plan = select_sample_tiles(10000, valid);
    check(short_plan.plan.samples == 10000 && short_plan.burst_windows == 0,
          "fully covered short capture should remain unchanged");
    std::mt19937_64 rng(87132);
    for (int trial = 0; trial < 1000; ++trial) {
        const std::size_t prefix = trial == 0 ? std::numeric_limits<std::size_t>::max() : 262145 + rng() % 1000000000;
        const auto hints = collect_burst_hints(512, prefix, false, [&](std::size_t) {
            const auto offset = rng() % prefix;
            return SampleTile{offset, std::min(std::size_t(1 + rng() % 200000), prefix - offset)};
        });
        const auto selected = select_sample_tiles(prefix, hints);
        check(selected.plan.count <= 4 && selected.plan.samples <= capture_sample_budget &&
              selected.burst_windows <= 2, "guided selection budget exceeded");
        std::size_t end = 0, total = 0;
        for (std::size_t i = 0; i < selected.plan.count; ++i) {
            const auto& b = selected.plan.tiles[i];
            check(b.samples >= 2048 && b.samples <= tile_sample_limit && b.source_offset >= end &&
                  b.source_offset <= prefix - b.samples, "guided selection overlap or out-of-range window");
            end = b.source_offset + b.samples; total += b.samples;
        }
        check(selected.plan.tiles[0].source_offset == 0 && end == prefix && total == selected.plan.samples,
              "guided selection lost context endpoints or sample accounting");
    }
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 6, "requires real worker and four broken worker paths");
        std::mt19937 rng(111); std::normal_distribution<float> normal;
        std::vector<std::complex<float>> iq(5000); for (auto& z : iq) z = {normal(rng), normal(rng)};
        protocol_tests(iq); sampling_tests();
        WorkerOptions options; options.executable = argv[1];
        ProcessClient client(options);
        const int sentinel = open("/dev/null", O_RDONLY); check(sentinel >= 3, "sentinel descriptor failed");
        client.start([] { return false; }); const int pid = client.pid();
        const auto limits = read_file("/proc/" + std::to_string(pid) + "/limits");
        check(std::regex_search(limits, std::regex("Max address space\\s+134217728\\s+134217728")) &&
              std::regex_search(limits, std::regex("Max cpu time\\s+30\\s+30")) &&
              std::regex_search(limits, std::regex("Max open files\\s+32\\s+32")) &&
              std::regex_search(limits, std::regex("Max core file size\\s+0\\s+0")),
              "worker limits missing");
        std::size_t handles = 0;
        for (const auto& fd : std::filesystem::directory_iterator("/proc/" + std::to_string(pid) + "/fd")) {
            ++handles; check(std::stoi(fd.path().filename().string()) <= 3, "unrelated parent descriptor inherited");
        }
        check(handles == 4, "worker descriptor profile changed"); close(sentinel);
        const auto status = read_file("/proc/" + std::to_string(pid) + "/status");
        check(std::regex_search(status, std::regex("NoNewPrivs:\\s+1")) &&
              std::regex_search(status, std::regex("Threads:\\s+1")) && getpriority(PRIO_PROCESS, pid) >= 10,
              "worker privilege/priority/thread profile changed");
        const auto result = client.analyze(19, 20e6, plan_sample_tiles(iq.size()), iq, [] { return false; });
        check(result.size() == 1 && result[0].spectral.quality == "measured", "real process measurement missing");
        client.terminate(); check(!std::filesystem::exists("/proc/" + std::to_string(pid)), "worker was not reaped");
        for (int fixture = 2; fixture < argc; ++fixture) {
            WorkerOptions faults; faults.executable = argv[fixture]; faults.deadline_ms = 150; faults.restart_limit = 1;
            ShadowWorker worker(faults); check(worker.set_enabled(true), "fault test starts");
            CaptureContext c; c.band_ghz = 2; c.sample_rate_hz = 20e6; c.capture_center_hz = 2438.5e6;
            c.source_samples = iq.size(); c.submission_epoch = worker.epoch();
            until([&] { return worker.try_submit(iq, iq.size(), c); });
            until([&] { return worker.stats().discarded > 0; });
            check(!worker.snapshot() && worker.enabled(), "fault published a result or skipped allowed recovery");
            until([&] { return worker.try_submit(iq, iq.size(), c); });
            until([&] { return !worker.enabled(); });
            const auto stats = worker.stats();
            check(stats.failures == 2 && stats.worker_restarts == 1 && !worker.snapshot(), "restart limit not honored");
            if (fixture == 3) check(stats.worker_timeouts == 2, "hung worker missed deadline");
            if (fixture >= 4) check(stats.protocol_errors == 2, "bad reply not rejected");
            const auto start = std::chrono::steady_clock::now(); worker.set_enabled(false);
            check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "fault shutdown unbounded");
        }
        // A fault followed by a healthy executable must recover on the next
        // queued capture, with the same bounded supervisor instance.
        const auto directory = std::filesystem::temp_directory_path() / ("cyclo-recovery-" + std::to_string(getpid()));
        std::filesystem::create_directory(directory);
        const auto link = directory / "worker";
        std::filesystem::create_symlink(argv[2], link);
        {
            WorkerOptions recovery; recovery.executable = link.string();
            ShadowWorker recovering(recovery); recovering.set_enabled(true);
            CaptureContext context; context.band_ghz = 2; context.sample_rate_hz = 20e6; context.capture_center_hz = 2438.5e6;
            context.source_samples = iq.size(); context.submission_epoch = recovering.epoch();
            until([&] { return recovering.try_submit(iq, iq.size(), context); });
            until([&] { return recovering.stats().discarded > 0; });
            std::filesystem::remove(link); std::filesystem::create_symlink(argv[1], link);
            context.capture_sequence = 7;
            until([&] { return recovering.try_submit(iq, iq.size(), context); });
            until([&] { return bool(recovering.snapshot()); });
            check(recovering.snapshot()->capture.capture_sequence == 7 && recovering.stats().worker_restarts == 1 &&
                  recovering.snapshot()->tiles.front().spectral.quality == "measured", "worker did not recover after fault");
            // Eight valid requests cause routine recycling without using the
            // fault allowance. A ninth must still return a valid measurement.
            const auto launches = recovering.stats().worker_launches;
            for (int request = 0; request < 9; ++request) {
                const auto before = recovering.stats().measured; ++context.capture_sequence;
                until([&] { return recovering.try_submit(iq, iq.size(), context); });
                until([&] { return recovering.stats().measured > before; });
            }
            check(recovering.stats().worker_launches > launches && recovering.stats().worker_restarts == 1,
                  "routine recycle consumes fault allowance");
            recovering.set_enabled(false);
        }
        std::filesystem::remove_all(directory);
        WorkerOptions hung; hung.executable = argv[3]; hung.deadline_ms = 5000;
        ShadowWorker worker(hung); worker.set_enabled(true);
        CaptureContext c; c.band_ghz = 2; c.sample_rate_hz = 20e6; c.capture_center_hz = 2438.5e6;
        c.source_samples = iq.size(); c.submission_epoch = worker.epoch();
        until([&] { return worker.try_submit(iq, iq.size(), c); });
        until([&] { return worker.stats().worker_pid > 0; }); worker.invalidate();
        until([&] { return worker.stats().discarded > 0; });
        check(worker.stats().failures == 0 && worker.enabled(), "cancelled stale job consumed fault allowance");
        c.submission_epoch = worker.epoch();
        until([&] { return worker.try_submit(iq, iq.size(), c); });
        until([&] { return worker.stats().worker_pid > 0; });
        const auto stop_start = std::chrono::steady_clock::now(); worker.set_enabled(false);
        check(std::chrono::steady_clock::now() - stop_start < std::chrono::seconds(1), "stop waits for hung request deadline");
        std::cout << "process crash/hang/bad-reply/limits/descriptor/sampling checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
