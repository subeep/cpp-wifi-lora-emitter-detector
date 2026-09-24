// One ordered background worker: jobs run one at a time, strictly in push
// order, on a dedicated thread. Used by Scanner to process Wi-Fi step N while
// the scan thread already captures step N+1 (security plan, package B:
// capture/processing overlap).
//
// Contract:
//  - FIFO, exactly one job running at a time.
//  - push(job, max_outstanding) blocks until fewer than `max_outstanding`
//    jobs are queued or running, so a job "counts" until it has run AND been
//    destroyed. With max_outstanding = 1, push returning means the previous
//    job's resources (e.g. its IQ buffers) are gone. This is the ONLY
//    backpressure point; the producer must not hold any mutex while calling
//    push() or wait_idle().
//  - Jobs run and are destroyed with the lane's mutex NOT held.
//  - fn must never call push()/wait_idle()/close() on the same lane.
//  - No exception handling on purpose: the worker loop is noexcept, so an
//    exception escaping fn terminates the process - exactly what an exception
//    escaping the scanner's own thread did before the lane existed. A dead
//    lane can therefore never leave the producer blocked forever.
//  - close() drains every queued job, then joins. Idempotent; the destructor
//    calls it.
#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace rfmon {

template <class Job>
class SerialLane {
public:
    struct Stats {
        uint64_t jobs_done = 0;
        size_t high_water = 0;          // max queued + running seen at push time
        uint64_t producer_waits = 0;    // pushes that had to wait
        double producer_wait_s_total = 0, producer_wait_s_max = 0;
    };

    explicit SerialLane(std::function<void(Job&)> fn, std::function<void()> on_start = {})
        : fn_(std::move(fn)), on_start_(std::move(on_start)), worker_([this] { loop(); }) {}

    ~SerialLane() { close(); }
    SerialLane(const SerialLane&) = delete;
    SerialLane& operator=(const SerialLane&) = delete;

    // Returns the seconds spent waiting for room.
    double push(Job job, size_t max_outstanding) {
        assert(max_outstanding >= 1);
        assert(std::this_thread::get_id() != worker_id_.load());
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(mu_);
        assert(!closing_);
        bool waited = false;
        while (q_.size() + running_ >= max_outstanding) {
            waited = true;
            done_cv_.wait(lock);
        }
        const double wait_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (waited) {
            ++stats_.producer_waits;
            stats_.producer_wait_s_total += wait_s;
            if (wait_s > stats_.producer_wait_s_max) stats_.producer_wait_s_max = wait_s;
        }
        q_.push_back(std::move(job));
        if (q_.size() + running_ > stats_.high_water) stats_.high_water = q_.size() + running_;
        lock.unlock();
        work_cv_.notify_one();
        return waited ? wait_s : 0.0;
    }

    // Blocks until every pushed job has run and been destroyed.
    void wait_idle() {
        assert(std::this_thread::get_id() != worker_id_.load());
        std::unique_lock<std::mutex> lock(mu_);
        done_cv_.wait(lock, [&] { return q_.empty() && running_ == 0; });
    }

    // Drains all queued jobs, then joins. Idempotent.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            closing_ = true;
        }
        work_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mu_);
        return stats_;
    }

private:
    void loop() noexcept {
        worker_id_.store(std::this_thread::get_id());
        if (on_start_) on_start_();
        std::unique_lock<std::mutex> lock(mu_);
        for (;;) {
            work_cv_.wait(lock, [&] { return closing_ || !q_.empty(); });
            if (q_.empty()) return;  // closing and fully drained
            {
                Job job = std::move(q_.front());
                q_.pop_front();
                running_ = 1;
                lock.unlock();
                fn_(job);
            }  // job destroyed here, unlocked
            lock.lock();
            running_ = 0;
            ++stats_.jobs_done;
            done_cv_.notify_all();
        }
    }

    std::function<void(Job&)> fn_;
    std::function<void()> on_start_;
    mutable std::mutex mu_;
    std::condition_variable work_cv_, done_cv_;
    std::deque<Job> q_;
    size_t running_ = 0;
    bool closing_ = false;
    Stats stats_;
    std::atomic<std::thread::id> worker_id_{};
    std::thread worker_;  // last: starts only after every other member is constructed
};

}  // namespace rfmon
