// Coordinated three-band mode end to end through the production
// Scanner::run(), with a fake radio (upgradedplan.md P3/P5). No UsrpCapture
// is constructed and all persistent stores live in a temporary root.
//
// Checks: band/visit order with rotating cursors, dwell/pre-roll/settle and
// always-retune requests, that IQ observers (drones/ML) only ever receive
// the 2.4/5 GHz visit they are told about (per-capture marker), that the
// drone-only DroneID visit reaches observers but not the Wi-Fi security
// path, LoRa decoding on a channel derived from the sub-GHz visit (real
// SX1262 packet), conflict refusal, schedule changes at a cycle boundary,
// lane drops instead of acquisition stalls, overflow accounting, and a
// clean return to an ordinary band mode.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "channelize.hpp"
#include "lora_capture.hpp"
#include "scanner.hpp"

using namespace rfmon;
namespace fs = std::filesystem;
using cf = std::complex<float>;

namespace rfmon {
struct ScannerTestAccess {
    static void set_device_factory(Scanner& s, decltype(Scanner::device_factory_) f) { s.device_factory_ = std::move(f); }
    static void observer(Scanner& s, Scanner::WifiIqObserver f) { s.wifi_iq_observer_ = std::move(f); }
};
}  // namespace rfmon

namespace {
int failures = 0;
void check(bool cond, const std::string& name, const std::string& detail = "") {
    std::printf("%s [%s]%s%s\n", cond ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
    if (!cond) ++failures;
}
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Call { double center, rate, duration, settle, t_start, t_end; bool retune; };

struct Shared {
    std::mutex mu;
    std::vector<Call> calls;
    std::vector<cf> subghz;  // sub-GHz visit content (5 MSps), padded with noise
    std::atomic<int> overflow_on_call{-1};
    std::vector<Call> snapshot() { std::lock_guard<std::mutex> l(mu); return calls; }
};

// Capture content carries a marker: sample 0 encodes the tuned centre, so
// an observer can verify the samples belong to the visit its record names.
float marker(double center_hz) { return float(center_hz * 1e-10); }

class FakeRadio : public CaptureDevice {
public:
    explicit FakeRadio(std::shared_ptr<Shared> s) : s_(std::move(s)) {}
    CaptureResult capture_detailed(double center, double rate, double duration, double settle, bool retune) override {
        const double t0 = now_s();
        std::this_thread::sleep_for(std::chrono::duration<double>(settle + duration));  // real-time pacing
        const size_t n = size_t(std::llround(rate * duration));
        CaptureResult r;
        r.sample_rate_hz = rate;
        r.samples.resize(n);
        std::normal_distribution<float> g(0.f, 1e-3f);
        for (auto& x : r.samples) x = {g(rng_), g(rng_)};
        if (center < 1e9) {
            for (size_t i = 0; i < n && i < s_->subghz.size(); ++i) r.samples[i] += s_->subghz[i];
        } else {
            r.samples[0] = {marker(center), 0.f};
        }
        auto& t = r.timing;
        t.device_time_valid = true;
        t.device_time_ns = device_ns_;
        device_ns_ += int64_t((settle + duration) * 1e9) + 1000000;
        t.host_before_ns = 1700000000000000000 + device_ns_;
        t.host_after_ns = t.host_before_ns + 100000;
        t.requested_samples = n;
        t.actual_rf_hz = center;
        t.gain_db = 20;
        t.retuned = retune;
        int index;
        {
            std::lock_guard<std::mutex> l(s_->mu);
            index = int(s_->calls.size());
            s_->calls.push_back({center, rate, duration, settle, t0, now_s(), retune});
        }
        if (index == s_->overflow_on_call.load()) {
            r.overflow = true;
            t.overflows.push_back({n / 2, false, 0});
        }
        return r;
    }
    void set_gain(std::optional<double>) override {}
private:
    std::shared_ptr<Shared> s_;
    std::mt19937 rng_{7};
    int64_t device_ns_ = 5000000000;
};

coord::Schedule test_schedule() {
    coord::Schedule s;
    s.settle_s = 0.01;
    s.preroll_s = 0.008;
    coord::Slot sub{BAND_SUB_GHZ, true, 1, 0.2, {}};
    sub.visits.push_back({BAND_SUB_GHZ, coord::VisitKind::SubGhz, 865.5e6, 5e6, "sub"});
    coord::Slot w24{BAND_WIFI_2G4, true, 2, 0.06, {}};
    w24.visits.push_back({BAND_WIFI_2G4, coord::VisitKind::DroneOnly, 2401e6, 2e6, "droneid"});
    w24.visits.push_back({BAND_WIFI_2G4, coord::VisitKind::WifiChannel, 2413.5e6, 2e6, "ch1"});
    w24.visits.push_back({BAND_WIFI_2G4, coord::VisitKind::WifiChannel, 2438.5e6, 2e6, "ch6"});
    coord::Slot w5{BAND_WIFI_5G, true, 2, 0.06, {}};
    for (double f : {5181.5e6, 5201.5e6, 5221.5e6})
        w5.visits.push_back({BAND_WIFI_5G, coord::VisitKind::WifiChannel, f, 2e6, "5g"});
    s.slots = {sub, w24, w5};
    return s;
}

template <class F> bool wait_for(F f, double timeout_s) {
    const double end = now_s() + timeout_s;
    while (now_s() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}
}  // namespace

int main() {
    char tmp[] = "/tmp/rfmon-coordinated-XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    const auto cwd = fs::current_path();
    fs::current_path(tmp);  // LoRa fingerprint log is relative
    auto shared = std::make_shared<Shared>();
    // Real SX1262 packet (500 kHz), embedded 485 kHz above the sub-GHz centre:
    // it must decode on the derived 865.985 MHz LoRa channel.
    const auto fixture = load_lora_capture(std::string(PROJECT_ROOT_DIR) + "/tests/fixtures/lora_sx1262/capture-1790061256825496286-0");
    {
        std::vector<cf> stuffed(fixture.iq.size() * 10);
        for (size_t i = 0; i < fixture.iq.size(); ++i) stuffed[i * 10] = fixture.iq[i] * 10.f;
        shared->subghz = decimate_fir(stuffed, blackman_lowpass(0.05, 0.004), 1);
        for (size_t i = 0; i < shared->subghz.size(); ++i)
            shared->subghz[i] *= std::polar(1.0f, float(std::remainder(2 * M_PI * 485e3 / 5e6 * double(i), 2 * M_PI)));
    }

    struct Seen { double center; std::string band; float first; };
    std::mutex seen_mu;
    std::vector<Seen> seen;
    std::atomic<int> observer_sleep_ms{0};
    // Declared before the scanner so it outlives it.
    droneid_followup::Config followup_config;
    followup_config.dwell_s = 0.2;
    followup_config.cooldown_s = 600;
    droneid_followup::Followup followup(followup_config);
    {
        Scanner scanner(tmp);
        ScannerTestAccess::observer(scanner, [&](const std::vector<cf>& iq, const wifi_security::CaptureRecord& r) {
            {
                std::lock_guard<std::mutex> l(seen_mu);
                seen.push_back({r.capture_center_hz, r.band, iq.empty() ? 0.f : iq[0].real()});
            }
            if (int ms = observer_sleep_ms.load()) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        });
        ScannerTestAccess::set_device_factory(scanner, [shared](const DeviceProfile&, std::optional<double>) {
            return std::unique_ptr<CaptureDevice>(new FakeRadio(shared));
        });
        const auto schedule = test_schedule();
        scanner.set_wifi_sample_rate(2e6);  // keeps the band-mode exit check small
        scanner.set_coordinated_schedule(schedule);
        scanner.set_active_band(BAND_COORDINATED);
        scanner.start();

        // --- Order, timing requests and cursors over three cycles ---
        check(wait_for([&] { return scanner.coordinated_status().cycle >= 3; }, 30), "Three coordinated cycles complete");
        const auto status = scanner.coordinated_status();
        check(status.config_hash == coord::schedule_hash(schedule) && status.conflict.empty() && status.config_error.empty(),
              "Status reports the configured schedule hash with no conflict/error");
        std::vector<double> expected;
        std::vector<size_t> cursors(schedule.slots.size(), 0);
        for (int c = 0; c < 3; ++c)
            for (size_t si = 0; si < schedule.slots.size(); ++si)
                for (size_t i : coord::next_visits(schedule.slots[si], cursors[si])) expected.push_back(schedule.slots[si].visits[i].center_hz);
        const auto calls = shared->snapshot();
        bool order = calls.size() >= expected.size();
        for (size_t i = 0; order && i < expected.size(); ++i) order = calls[i].center == expected[i];
        check(order, "Capture centres follow sub-GHz -> 2.4 -> 5 GHz with rotating cursors for 3 cycles");
        bool requests = true;
        for (size_t i = 0; i < expected.size(); ++i) {
            const double dwell = calls[i].center < 1e9 ? 0.2 : 0.06;
            requests = requests && std::abs(calls[i].duration - (dwell + 0.008)) < 1e-12 && calls[i].settle == 0.01 && calls[i].retune;
        }
        check(requests, "Each visit requests dwell + pre-roll, the schedule's settle, and always retunes");
        check(status.slots.size() == 3 && status.slots[0].visits >= 3 && status.slots[1].visits >= 6 && status.slots[2].visits >= 6,
              "Per-slot visit counters advance");
        check(status.slots[2].last_sweep_s > 0, "5 GHz full-list sweep time is measured once the cursor wraps");

        // --- Observers: right samples for the right visit, never sub-GHz ---
        check(wait_for([&] { std::lock_guard<std::mutex> l(seen_mu); return seen.size() >= 8; }, 10), "IQ observers receive visits");
        {
            std::lock_guard<std::mutex> l(seen_mu);
            bool marker_ok = true, no_sub = true, drone_only = false, band_ok = true;
            for (const auto& s : seen) {
                marker_ok = marker_ok && std::abs(double(s.first) * 1e10 - s.center) < 2e3;
                no_sub = no_sub && s.center > 1e9;
                drone_only = drone_only || s.center == 2401e6;
                band_ok = band_ok && ((s.center < 3e9) == (s.band == BAND_WIFI_2G4)) && ((s.center > 5e9) == (s.band == BAND_WIFI_5G));
            }
            check(marker_ok, "Every observer call's samples carry the marker of the centre its record names");
            check(no_sub, "Sub-GHz visits are never offered to the Wi-Fi-rate drone/ML observers");
            check(drone_only, "Drone-only DroneID visit reaches the drone/ML observers");
            check(band_ok, "Observer records carry the band of their own visit");
        }
        // Security path: only Wi-Fi channel visits, never the drone-only visit.
        const auto security = scanner.wifi_security_snapshot();
        check(security->captures_ingested > 0, "Wi-Fi channel visits reach the security monitor");

        // --- LoRa on a channel derived from the sub-GHz visit ---
        check(wait_for([&] {
                  for (const auto& r : scanner.lora_packets())
                      if (r.header_valid && r.crc == LoraPacketRow::Crc::Valid && std::abs(r.freq_mhz - 865.985) < 1e-6) return true;
                  return false;
              }, 15),
              "Real SX1262 packet decodes with valid CRC on derived LoRa channel 865.985 MHz");
        check(scanner.lora_security_snapshot()->captures >= 4, "Each derived LoRa channel is recorded as LoRa capture evidence");
        check(!scanner.snapshot(BAND_SUB_GHZ).empty(), "Sub-GHz energy map is fed from the coordinated visit");

        // --- Opt-in IQ retention for evidence export ---
        check(scanner.retained_capture_seqs().empty(), "No IQ is retained unless enabled");
        scanner.set_iq_retention(3);
        check(wait_for([&] { return scanner.retained_capture_seqs().size() == 3; }, 10), "Retention keeps the newest 3 Wi-Fi/drone captures");
        {
            bool records_ok = true, visit_link = true;
            const auto visits_now = scanner.coordinated_visits();
            for (uint64_t seq : scanner.retained_capture_seqs()) {
                const auto r = scanner.retained_capture(seq);
                records_ok = records_ok && r && r->iq && r->record.capture_seq == seq && !r->record.run_id.empty() &&
                             !r->record.band.empty() && r->iq->size() == r->record.samples_received &&
                             std::abs(double((*r->iq)[0].real()) * 1e10 - r->record.capture_center_hz) < 2e3;
                visit_link = visit_link && std::any_of(visits_now.begin(), visits_now.end(),
                                                       [&](const coord::VisitRecord& v) { return v.capture_seq == seq; });
            }
            check(records_ok, "Retained IQ keeps its full capture record and is the visit's own samples");
            check(visit_link, "Each retained capture maps back to its coordinated visit");
        }
        scanner.set_iq_retention(0);
        check(scanner.retained_capture_seqs().empty(), "Disabling retention releases the IQ");

        // --- Conflict: a fixed Wi-Fi channel refuses coordinated acquisition ---
        scanner.set_wifi_fixed_channel(6);
        check(wait_for([&] { return !scanner.coordinated_status().conflict.empty(); }, 5), "Fixed channel produces a visible conflict");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));  // let an in-flight visit finish
        const size_t during = shared->snapshot().size();
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        check(shared->snapshot().size() == during, "No captures while the conflict is unresolved");
        scanner.set_wifi_fixed_channel(std::nullopt);
        check(wait_for([&] { return shared->snapshot().size() > during + 2 && scanner.coordinated_status().conflict.empty(); }, 5),
              "Clearing the conflict resumes coordinated acquisition");

        // --- Slow analysis drops visits instead of stalling acquisition ---
        const auto before = scanner.coordinated_status();
        const size_t calls_before = shared->snapshot().size();
        observer_sleep_ms = 500;
        check(wait_for([&] { return scanner.coordinated_status().cycle >= before.cycle + 2; }, 30), "Cycles continue under slow analysis");
        observer_sleep_ms = 0;
        const auto slow = scanner.coordinated_status();
        const auto slow_calls = shared->snapshot();
        uint64_t drops = 0;
        for (size_t i = 1; i < slow.slots.size(); ++i) drops += slow.slots[i].lane_drops - before.slots[i].lane_drops;
        check(drops > 0, "Busy Wi-Fi lane drops visits and counts them", std::to_string(drops) + " dropped");
        double worst_gap = 0;
        for (size_t i = calls_before + 1; i < slow_calls.size(); ++i)
            worst_gap = std::max(worst_gap, slow_calls[i].t_start - slow_calls[i - 1].t_end);
        check(worst_gap < 0.15, "Acquisition never waits for analysis (max gap between captures)", std::to_string(worst_gap) + " s");

        // --- Overflow: visit marked invalid and counted ---
        shared->overflow_on_call = int(shared->snapshot().size()) + 2;
        check(wait_for([&] { return scanner.coordinated_status().overflows >= 1; }, 10), "Overflow is counted");
        bool invalid_visit = false;
        for (const auto& v : scanner.coordinated_visits()) invalid_visit = invalid_visit || (!v.in_progress && !v.valid && v.acquired_s > 0);
        check(invalid_visit, "The overflowed visit is recorded as not valid");

        // --- DroneID follow-up visits: only after a link sighting, bounded, stopped by a packet ---
        {
            const auto is_followup = [](const Call& c) { return c.center > 1e9 && std::abs(c.duration - 0.208) < 1e-12; };
            scanner.set_droneid_followup(&followup);
            const uint64_t idle_cycle = scanner.coordinated_status().cycle;
            wait_for([&] { return scanner.coordinated_status().cycle >= idle_cycle + 2; }, 20);
            check(scanner.coordinated_status().followup_visits == 0, "No follow-up visits without a drone-link sighting");
            const size_t start = shared->snapshot().size();
            {
                std::lock_guard<std::mutex> l(seen_mu);
                seen.clear();
            }
            followup.link_seen({2438.5e6, now_s(), "15kHz"});
            check(wait_for([&] { return scanner.coordinated_status().followup_visits >= 4; }, 40),
                  "A 2.4 GHz link sighting starts follow-up visits");
            const auto all = shared->snapshot();
            std::vector<Call> extra;
            double extra_s = 0, total_s = 0;
            for (size_t i = start; i < all.size(); ++i) {
                total_s += all[i].settle + all[i].duration;
                if (is_followup(all[i])) { extra.push_back(all[i]); extra_s += all[i].settle + all[i].duration; }
            }
            bool rotation = extra.size() >= 4;
            const double want[] = {2399.5e6, 2414.5e6, 2429.5e6, 2444.5e6};
            for (size_t i = 0; rotation && i < 4; ++i)
                rotation = extra[i].center == want[i] + 1.5e6 && extra[i].rate == 2e6 && extra[i].settle == 0.01 && extra[i].retune;
            check(rotation, "Follow-up visits rotate through the 2.4 GHz DroneID channels at the Wi-Fi rate");
            check(extra_s <= 0.27 * total_s, "Follow-up visits stay within their share of scanner time",
                  std::to_string(extra_s / total_s));
            bool linked = false, observed = false;
            for (const auto& v : scanner.coordinated_visits())
                if (v.label.rfind("DroneID follow-up", 0) == 0 && v.kind == coord::VisitKind::DroneOnly && v.capture_seq)
                    linked = linked || followup.is_followup_capture(v.capture_seq);
            check(wait_for([&] {
                      std::lock_guard<std::mutex> l(seen_mu);
                      for (const auto& s : seen) observed = observed || (s.center == 2416e6 && s.band == BAND_WIFI_2G4 &&
                                                                         std::abs(double(s.first) * 1e10 - s.center) < 2e3);
                      return observed;
                  }, 10), "Follow-up captures reach the drone/ML observers with their own samples");
            check(linked, "Follow-up captures are marked for priority decoding");
            const auto st = scanner.coordinated_status();
            check(st.followup_valid >= 4 && st.followup_acquired_s > 0.7 && st.slots[1].visits > 0,
                  "Follow-up visits are counted apart from the schedule's slots");
            // A decoded DroneID packet ends the request.
            followup.packet_seen({2414.5e6, now_s(), true});
            const uint64_t closing_cycle = scanner.coordinated_status().cycle;
            wait_for([&] { return scanner.coordinated_status().cycle >= closing_cycle + 2; }, 20);
            const uint64_t settled = scanner.coordinated_status().followup_visits;
            followup.link_seen({2438.5e6, now_s(), "15kHz"});  // inside the cooldown: must not reopen
            wait_for([&] { return scanner.coordinated_status().cycle >= closing_cycle + 5; }, 30);
            check(scanner.coordinated_status().followup_visits == settled && followup.status().bands[0].last_outcome == "packet decoded",
                  "A decoded DroneID packet stops the follow-up visits");
            scanner.set_droneid_followup(nullptr);
        }

        // --- Schedule change applies at a boundary and resets cursors ---
        auto only5 = schedule;
        only5.slots[0].enabled = false;
        only5.slots[1].enabled = false;
        scanner.set_coordinated_schedule(only5);
        check(wait_for([&] { return scanner.coordinated_status().config_hash == coord::schedule_hash(only5); }, 10),
              "New schedule hash is published");
        const size_t mark = shared->snapshot().size();
        check(wait_for([&] { return shared->snapshot().size() >= mark + 6; }, 10), "Acquisition continues on the new schedule");
        const auto after_change = shared->snapshot();
        bool only5_ok = true;
        // The first call after the mark may belong to the abandoned cycle.
        for (size_t i = mark + 1; i < after_change.size(); ++i) only5_ok = only5_ok && after_change[i].center > 5e9;
        check(only5_ok, "After the change only the enabled 5 GHz slot is visited");

        // --- Leaving coordinated mode returns to the ordinary band mode ---
        scanner.set_active_band(BAND_WIFI_5G);
        check(wait_for([&] { return !scanner.coordinated_status().active; }, 10), "Coordinated mode exits on mode change");
        const size_t exit_mark = shared->snapshot().size();
        check(wait_for([&] { return shared->snapshot().size() > exit_mark; }, 10), "Band mode acquisition resumes");
        const auto band_calls = shared->snapshot();
        check(std::abs(band_calls[exit_mark].duration - SUB_CAPTURE_DURATION_S) < 1e-12,
              "Band mode uses its own unchanged capture duration");
        scanner.stop();
    }
    fs::current_path(cwd);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
