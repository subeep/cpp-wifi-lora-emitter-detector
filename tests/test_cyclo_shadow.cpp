#include "cyclostationary/shadow_worker.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace rfmon::cyclo;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void until(F f) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!f()) {
        if (std::chrono::steady_clock::now() >= end) throw std::runtime_error("worker wait timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
}
int main(int argc, char** argv) {
    try {
        rfmon::CaptureTiming t;
        check(continuous_capture_prefix(t, 30000, false) == 30000, "ungapped capture prefix preserved");
        check(continuous_capture_prefix(t, 30000, true) == 0, "overflow without position abstains");
        t.overflows = {{5000, true, 1234}, {15000, true, 9876}};
        check(continuous_capture_prefix(t, 30000, true) == 5000, "resume timestamps do not concatenate gap-separated data");
        t.overflows[0].at_sample = 0;
        check(continuous_capture_prefix(t, 30000, true) == 0, "gap at first sample abstains");
        t.overflows[0].at_sample = 16000;
        check(continuous_capture_prefix(t, 30000, true) == 0, "unordered gaps abstain");
        t.overflows = {{30001, false, 0}};
        check(continuous_capture_prefix(t, 30000, true) == 0, "out-of-range gap abstains");
        std::mt19937 rng(1117); std::normal_distribution<float> normal;
        std::vector<std::complex<float>> iq(300000);
        for (auto& z : iq) z = {normal(rng), normal(rng)};
        // Reference CP structure only in the final 65536 samples. A prefix-
        // only adapter misses this; the new last window must recover it.
        std::vector<std::complex<float>> late;
        while (late.size() < tile_sample_limit) {
            std::vector<std::complex<float>> symbol(64);
            for (auto& z : symbol) z = {normal(rng), normal(rng)};
            late.insert(late.end(), symbol.end() - 16, symbol.end());
            late.insert(late.end(), symbol.begin(), symbol.end());
        }
        std::copy_n(late.begin(), tile_sample_limit, iq.end() - tile_sample_limit);
        const auto original = iq;
        WorkerOptions options;
        if (argc == 2) options.executable = argv[1];
        ShadowWorker worker(options);
        CaptureContext c; c.band_ghz = 2; c.sample_rate_hz = 20e6; c.capture_center_hz = 2438.5e6;
        c.source_samples = iq.size(); c.capture_sequence = 1;
        check(!worker.enabled() && !worker.snapshot(), "default is disabled/empty");
        check(!worker.try_submit(iq, iq.size(), c) && worker.stats().submitted == 0, "disabled submission is a no-op");
        check(worker.set_enabled(true), "worker starts"); c.submission_epoch = worker.epoch();
        until([&] { return worker.try_submit(iq, iq.size(), c); });
        until([&] { return bool(worker.snapshot()); });
        const auto first = worker.snapshot();
        check(first->copied_samples == 262144 && first->capture.source_samples == 300000, "copied IQ cap recorded");
        check(first->tiles.size() == 4 && first->tiles.front().spectral.quality == "measured" &&
              first->tiles.front().ofdm.size() == 5, "both estimators ran independently on distributed tiles");
        check(first->tiles.back().source.source_offset + first->tiles.back().source.samples == iq.size(),
              "end-of-capture window included");
        check(first->tiles.back().ofdm[0].holdout_contrast > 0.95 &&
              first->tiles.front().ofdm[0].holdout_contrast < 0.1,
              "late CP structure recovered without concatenating separated windows");
        check(worker.stats().worker_pid > 0, "DSP runs in separate process");
        check(iq == original, "producer IQ unchanged");
        // Two isolated reference transmissions between the old distributed
        // windows. Reusing raw hints must recover both independently.
        std::vector<std::complex<float>> sparse(1000000);
        for (auto& z : sparse) z = {0.01f * normal(rng), 0.01f * normal(rng)};
        const std::array<SampleTile, 2> burst_ranges = {{{450000, 12000}, {750000, 8000}}};
        for (const auto& b : burst_ranges) std::copy_n(late.begin(), b.samples, sparse.begin() + b.source_offset);
        const auto sparse_original = sparse;
        auto sc = c; sc.source_samples = sparse.size(); sc.capture_sequence = 10;
        until([&] { return worker.try_submit(sparse, sparse.size(), sc); });
        until([&] { return worker.snapshot() && worker.snapshot()->capture.capture_sequence == 10; });
        for (const auto& tile : worker.snapshot()->tiles)
            check(tile.ofdm[0].holdout_contrast < 0.1, "unguided windows unexpectedly covered hidden reference bursts");
        const auto hints = collect_burst_hints(2, sparse.size(), false, [&](std::size_t i) { return burst_ranges[i]; });
        sc.capture_sequence = 11;
        until([&] { return worker.try_submit(sparse, sparse.size(), sc, hints); });
        until([&] { return worker.snapshot() && worker.snapshot()->capture.capture_sequence == 11; });
        const auto guided = worker.snapshot();
        check(guided->selection.burst_windows == 2 && guided->tiles.size() == 4 && guided->copied_samples <= 262144,
              "two raw bursts selected within unchanged IQ budget");
        for (std::size_t i = 0; i < guided->tiles.size(); ++i) if (guided->selection.burst_guided[i])
        {
            check(guided->tiles[i].ofdm[0].holdout_contrast > 0.8, "hinted reference CP did not persist on holdout");
            check(guided->tiles[i].roi.samples_examined == guided->tiles[i].source.samples &&
                  guided->tiles[i].roi.status == "contrast_regions", "child ROI measurement missing from guided reference burst");
        }
        check(sparse == sparse_original, "burst guidance mutated producer IQ");
        worker.invalidate();
        check(!worker.snapshot() && first->capture.capture_sequence == 1, "invalidation clears live snapshot, old immutable snapshot survives");
        check(!worker.try_submit(iq, iq.size(), c), "late capture from old epoch rejected");
        c.submission_epoch = worker.epoch(); c.capture_sequence = 2; c.capture_overflow = true;
        until([&] { return worker.try_submit(iq, 5000, c); });
        until([&] { return bool(worker.snapshot()); });
        check(worker.snapshot()->copied_samples == 5000, "known continuous prefix respected");
        check(!worker.try_submit(iq, 0, c) && !worker.try_submit(iq, iq.size() + 1, c), "invalid prefixes rejected");
        // This is beyond the FFT estimator's 4881-sample span for a 5000
        // sample copy: spectral work succeeds, then the CP stage fails.
        auto bad = iq; bad[4999] = {NAN, 0};
        auto bad_context = c; bad_context.capture_sequence = 99;
        const auto before_bad = worker.snapshot();
        const auto failures_before = worker.stats().failures;
        const auto discarded_before = worker.stats().discarded;
        until([&] { return worker.try_submit(bad, 5000, bad_context); });
        until([&] { return worker.stats().failures > failures_before && worker.stats().discarded > discarded_before; });
        check(worker.snapshot() == before_bad, "failed measurement never publishes partially built result");
        c.capture_sequence = 3;
        until([&] { return worker.try_submit(iq, 5000, c); });
        until([&] { return worker.snapshot() && worker.snapshot()->capture.capture_sequence == 3; });
        check(worker.enabled(), "measurement exception contained; worker continues");
        // Flood is deliberately faster than the consumer. No queue-space wait.
        for (int i = 0; i < 200; ++i) worker.try_submit(iq, iq.size(), c);
        const auto flood = worker.stats();
        check(flood.high_water <= 8 && flood.dropped_busy > 0, "pool bounded and overload drops");
        check(worker.set_enabled(false) && !worker.snapshot(), "stop joins and clears snapshot");
        check(worker.set_enabled(true), "worker restarts"); c.submission_epoch = worker.epoch();
        until([&] { return worker.try_submit(iq, 5000, c); });
        until([&] { return bool(worker.snapshot()); });
        worker.set_enabled(false);
        // Exercise stop/restart while a producer races with the lifecycle thread.
        std::atomic<bool> done{false};
        std::thread producer([&] {
            auto local = c;
            while (!done.load()) {
                local.submission_epoch = worker.epoch(); worker.try_submit(iq, 5000, local);
                std::this_thread::yield();
            }
        });
        for (int i = 0; i < 5; ++i) { worker.set_enabled(true); worker.invalidate(); worker.set_enabled(false); }
        done = true; producer.join();
        const auto stats = worker.stats();
        check(stats.high_water <= 8 && !stats.enabled, "racing lifecycle remains bounded and stops");
        std::cout << "shadow worker checks passed; max accepted submission " << stats.max_submit_ms
                  << " ms; busy drops " << stats.dropped_busy << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
