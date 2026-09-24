// Contract tests for src/serial_lane.hpp, the ordered worker that lets the
// scanner process Wi-Fi step N while it captures step N+1. No radio needed.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "serial_lane.hpp"

using namespace rfmon;
using namespace std::chrono_literals;

namespace {
int failures = 0;
void check(bool cond, const std::string& name, const std::string& detail = "") {
    std::printf("%s [%s]%s%s\n", cond ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    if (!cond) ++failures;
}

// A job whose destructor is observable, to prove push(max=1) only returns
// once the previous job's resources are gone.
struct Tracked {
    int id = -1;
    std::shared_ptr<std::atomic<int>> alive;
    Tracked() = default;
    Tracked(int i, std::shared_ptr<std::atomic<int>> a) : id(i), alive(std::move(a)) { ++*alive; }
    Tracked(Tracked&& o) noexcept : id(o.id), alive(std::move(o.alive)) { o.id = -1; }
    Tracked& operator=(Tracked&& o) noexcept {
        release();
        id = o.id; alive = std::move(o.alive); o.id = -1;
        return *this;
    }
    ~Tracked() { release(); }
    // Slow release, like freeing hundreds of MB of IQ: widens the window in
    // which a lane that signalled "done" before destroying the job is caught.
    void release() {
        if (alive && id >= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); --*alive; }
        alive.reset();
    }
};
}  // namespace

int main() {
    // 1. Strict FIFO, one job at a time, under random producer/consumer delays.
    {
        std::vector<int> order;
        std::atomic<int> running{0}, max_running{0};
        std::mt19937 rng_c(7);
        SerialLane<int> lane([&](int& v) {
            int r = ++running;
            max_running = std::max(max_running.load(), r);
            if (rng_c() % 50 == 0) std::this_thread::sleep_for(50us);
            order.push_back(v);  // single consumer: no lock needed
            --running;
        });
        std::mt19937 rng_p(11);
        for (int i = 0; i < 10000; ++i) {
            if (rng_p() % 64 == 0) std::this_thread::sleep_for(30us);
            lane.push(i, 3);
        }
        lane.wait_idle();
        bool fifo = order.size() == 10000;
        for (int i = 0; fifo && i < 10000; ++i) fifo = order[size_t(i)] == i;
        check(fifo, "fifo_10000_jobs");
        check(max_running.load() == 1, "one_job_at_a_time", std::to_string(max_running.load()));
        auto st = lane.stats();
        check(st.jobs_done == 10000 && st.high_water <= 3, "bounded_outstanding", "high water " + std::to_string(st.high_water));
    }

    // 2. push(max=1) returns only after the previous job was destroyed.
    {
        auto alive = std::make_shared<std::atomic<int>>(0);
        int max_alive = 0;
        std::atomic<bool> slow{true};
        SerialLane<Tracked> lane([&](Tracked&) { if (slow) std::this_thread::sleep_for(2ms); });
        for (int i = 0; i < 50; ++i) {
            lane.push(Tracked(i, alive), 1);
            // At most the job just pushed (queued or running) is alive.
            max_alive = std::max(max_alive, alive->load());
        }
        lane.wait_idle();
        check(max_alive <= 1 && alive->load() == 0, "previous_job_destroyed_before_push_returns",
              "max alive " + std::to_string(max_alive));
    }

    // 3. A marker pushed with max=2 does not wait while one job is running.
    {
        std::atomic<bool> release{false}, started{false};
        SerialLane<int> lane([&](int& v) {
            if (v == 0) { started = true; while (!release) std::this_thread::sleep_for(1ms); }
        });
        lane.push(0, 1);
        while (!started) std::this_thread::sleep_for(1ms);
        const auto t0 = std::chrono::steady_clock::now();
        const double waited = lane.push(1, 2);
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(waited == 0.0 && dt < 0.05, "marker_does_not_wait", std::to_string(dt));
        // A third push with max=1 must wait for both; release after a delay.
        std::thread rel([&] { std::this_thread::sleep_for(30ms); release = true; });
        const double w3 = lane.push(2, 1);
        rel.join();
        check(w3 >= 0.02, "full_lane_makes_producer_wait", std::to_string(w3));
        auto st = lane.stats();
        check(st.producer_waits >= 1 && st.producer_wait_s_max >= 0.02, "wait_stats_recorded");
    }

    // 4. wait_idle returns only after the last job; close drains; idempotent.
    {
        std::atomic<int> done{0};
        auto lane = std::make_unique<SerialLane<int>>([&](int&) { std::this_thread::sleep_for(3ms); ++done; });
        for (int i = 0; i < 5; ++i) lane->push(i, 10);
        lane->wait_idle();
        check(done == 5, "wait_idle_waits_for_all");
        for (int i = 0; i < 7; ++i) lane->push(i, 10);
        lane->close();
        check(done == 12, "close_drains_queue", std::to_string(done.load()));
        lane->close();
        lane.reset();  // destructor closes again
        check(done == 12, "close_idempotent_and_destructor_safe");
    }

    // 5. on_start runs on the worker thread, before the first job.
    {
        std::thread::id start_id, job_id;
        std::atomic<bool> started_first{false};
        SerialLane<int> lane([&](int&) { job_id = std::this_thread::get_id(); },
                             [&] { start_id = std::this_thread::get_id(); started_first = true; });
        lane.push(1, 1);
        lane.wait_idle();
        check(started_first && start_id == job_id && start_id != std::this_thread::get_id(),
              "on_start_on_worker_thread");
    }

    // 6. Destroying a lane with queued work processes it (no silent drop).
    {
        std::atomic<int> done{0};
        {
            SerialLane<int> lane([&](int&) { std::this_thread::sleep_for(1ms); ++done; });
            for (int i = 0; i < 20; ++i) lane.push(i, 100);
        }
        check(done == 20, "destructor_drains", std::to_string(done.load()));
    }

    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
