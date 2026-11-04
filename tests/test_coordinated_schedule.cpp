// Coordinated three-band mode, radio-free pure parts (upgradedplan.md P1):
// schedule defaults/validation/estimates, rotating cursors, configuration
// hash, coverage-aware freshness, and channel derivation - including a real
// archived SX1262 LoRa packet that must still decode after being placed in a
// 5 MSps sub-GHz capture and derived back to 500 kHz.
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include "channelize.hpp"
#include "coordinated_schedule.hpp"
#include "lora_capture.hpp"
#include "lora_observation.hpp"

using namespace rfmon;
using cf = std::complex<float>;

namespace {
int failures = 0;
void check(bool cond, const std::string& name, const std::string& detail = "") {
    std::printf("%s [%s]%s%s\n", cond ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    if (!cond) ++failures;
}
bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Power of a complex tone at `f` (cycles/sample) by direct correlation.
double tone_power(const std::vector<cf>& x, double f, size_t skip) {
    std::complex<double> acc = 0;
    for (size_t i = skip; i + skip < x.size(); ++i)
        acc += std::complex<double>(x[i]) * std::polar(1.0, -2 * M_PI * f * double(i));
    const double n = double(x.size() - 2 * skip);
    return std::norm(acc / n);
}

std::vector<cf> tone(size_t n, double f) {
    std::vector<cf> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = std::polar(1.0f, float(std::remainder(2 * M_PI * f * double(i), 2 * M_PI)));
    return x;
}

// Zero-stuff by `up`, low-pass, and shift by offset: places a narrow capture
// inside a wider one, the inverse of derive_channel().
std::vector<cf> embed(const std::vector<cf>& narrow, size_t up, double offset_cycles_per_sample) {
    std::vector<cf> stuffed(narrow.size() * up);
    for (size_t i = 0; i < narrow.size(); ++i) stuffed[i * up] = narrow[i] * float(up);
    auto y = decimate_fir(stuffed, blackman_lowpass(0.5 / double(up), 0.04 / double(up)), 1);
    for (size_t i = 0; i < y.size(); ++i)
        y[i] *= std::polar(1.0f, float(std::remainder(2 * M_PI * offset_cycles_per_sample * double(i), 2 * M_PI)));
    return y;
}

void schedule_tests() {
    const auto s = coord::default_schedule(20e6);
    check(coord::validate(s, 20e6).empty(), "Default schedule is valid at the X310's 20 MSps", coord::validate(s, 20e6));
    check(s.slots.size() == 3 && s.slots[0].band == BAND_SUB_GHZ && s.slots[1].band == BAND_WIFI_2G4 &&
              s.slots[2].band == BAND_WIFI_5G,
          "Band order is sub-GHz -> 2.4 GHz -> 5 GHz");
    check(s.slots[0].visits_per_cycle == 2 && s.slots[1].visits_per_cycle == 4 && s.slots[2].visits_per_cycle == 6 &&
              s.slots[0].dwell_s == 0.512 && s.slots[1].dwell_s == 0.512 && s.slots[2].dwell_s == 0.512,
          "Plan's initial dwell profile: 2/4/6 visits x 512 ms");
    check(s.slots[1].visits.size() == 14 && s.slots[1].visits[0].kind == coord::VisitKind::DroneOnly &&
              near(s.slots[1].visits[0].center_hz, 2401e6, 1),
          "2.4 GHz list: DroneID 2399.5 MHz drone-only visit (offset centre) + channels 1-13");
    check(s.slots[2].visits.size() == 9, "5 GHz list keeps the existing 9 non-DFS channels");

    const auto e = coord::estimate(s);
    check(near(e.valid_s, 6.144, 1e-9), "Valid observation per cycle is 6.144 s", std::to_string(e.valid_s));
    check(near(e.overhead_s, 12 * (0.008 + RETUNE_SETTLE_S), 1e-9), "Overhead is 12 x (pre-roll + settle)");
    check(near(e.cycle_s, e.valid_s + e.overhead_s, 1e-12), "Cycle = valid + overhead (no inference term)");
    check(e.slots[0].cycles_per_sweep == 1 && e.slots[1].cycles_per_sweep == 4 && e.slots[2].cycles_per_sweep == 2,
          "Full sweeps: sub-GHz 1 cycle, 2.4 GHz 4 cycles (14/4), 5 GHz 2 cycles (9/6)");
    check(near(e.slots[1].sweep_s, 3.5 * e.cycle_s, 1e-9) && near(e.slots[2].sweep_s, 1.5 * e.cycle_s, 1e-9),
          "Average revisit per centre = centres / visits per cycle x cycle (2.4 GHz 3.5, 5 GHz 1.5 cycles)");

    // Rotating cursor resumes where the previous cycle stopped and wraps.
    size_t cursor = 0;
    auto a = coord::next_visits(s.slots[2], cursor);
    auto b = coord::next_visits(s.slots[2], cursor);
    check(a == std::vector<size_t>({0, 1, 2, 3, 4, 5}) && b == std::vector<size_t>({6, 7, 8, 0, 1, 2}) && cursor == 3,
          "5 GHz cursor: 0-5, then 6,7,8,0,1,2; resumes at 3");
    coord::Slot one = s.slots[0];
    cursor = 0;
    check(coord::next_visits(one, cursor) == std::vector<size_t>({0, 0}), "Short list repeats its centre within a cycle");
    one.enabled = false;
    check(coord::next_visits(one, cursor).empty(), "Disabled slot plans no visits");

    // Hash identifies the configuration; any acquisition change alters it.
    auto changed = s;
    changed.slots[1].dwell_s = 0.256;
    check(coord::schedule_hash(s) == coord::schedule_hash(coord::default_schedule(20e6)) &&
              coord::schedule_hash(s) != coord::schedule_hash(changed),
          "Schedule hash is stable and changes with dwell");

    auto bad = s;
    bad.slots[0].visits[0].sample_rate_hz = 5.2e6;
    check(!coord::validate(bad, 20e6).empty(), "Sub-GHz rate that is not a multiple of 500 kHz is rejected");
    bad = s;
    bad.slots[1].dwell_s = 0.01;
    check(!coord::validate(bad, 20e6).empty(), "Dwell below 50 ms is rejected");
    bad = s;
    for (auto& slot : bad.slots) slot.enabled = false;
    check(!coord::validate(bad, 20e6).empty(), "All slots disabled is rejected");
    check(!coord::validate(coord::default_schedule(56e6), 20e6).empty(), "A rate above the receiver limit is rejected");
    bad = s;
    bad.slots[2].visits[0].band = BAND_WIFI_2G4;
    check(!coord::validate(bad, 20e6).empty(), "A visit in the wrong band slot is rejected");
}

void freshness_tests() {
    std::deque<coord::VisitRecord> visits;
    auto visit = [&](double centre, double start, double end, bool valid, bool analysed, bool in_progress = false) {
        coord::VisitRecord v;
        v.center_hz = centre;
        v.usable_half_span_hz = coord::usable_half_span(20e6);
        v.start_s = start;
        v.end_s = end;
        v.valid = valid;
        v.analysed = analysed;
        v.in_progress = in_progress;
        visits.push_back(v);
    };
    const double f = 2437e6;
    visit(2438.5e6, 10, 10.6, true, true);  // the visit the entry was seen in
    visit(5181.5e6, 11, 11.6, true, true);  // another band: does not cover f
    check(coord::freshness(f, 10.3, 12, visits, 60) == coord::Freshness::RecentBandUnobserved,
          "Seen, receiver elsewhere since: recent, not lost");
    visit(2438.5e6, 20, 20.6, true, true, /*in_progress=*/true);
    check(coord::freshness(f, 10.3, 20.2, visits, 60) == coord::Freshness::ReceivingNow,
          "Covering visit in progress: receiving now");
    visits.back().in_progress = false;
    check(coord::freshness(f, 10.3, 21.0, visits, 60) == coord::Freshness::RecentBandUnobserved,
          "Later covering visit inside the result grace period is not yet a failed reacquisition");
    check(coord::freshness(f, 10.3, 23.0, visits, 60) == coord::Freshness::NotReacquired,
          "Later valid analysed covering visit without new evidence: not reacquired");
    visits.back().analysed = false;
    check(coord::freshness(f, 10.3, 23.0, visits, 60) == coord::Freshness::InsufficientCoverage,
          "Later covering visit dropped by its lane: insufficient coverage, not departure");
    check(coord::freshness(f, 10.3, 100, visits, 60) == coord::Freshness::Expired, "Past the retention limit: expired");
    check(coord::freshness(2412e6, 10.3, 23.0, visits, 60) == coord::Freshness::RecentBandUnobserved,
          "A visit 26.5 MHz away does not count as observing the entry's frequency");
}

void channel_tests() {
    // Tone 485 kHz above a 5 MSps capture centre, plus a strong interferer
    // just outside the 500 kHz channel. After derivation the tone sits at DC.
    const double in_rate = 5e6, out_rate = 500e3;
    auto wide = tone(500000, 485e3 / in_rate);
    const auto interferer = tone(500000, (485e3 + 300e3) / in_rate);
    for (size_t i = 0; i < wide.size(); ++i) wide[i] = wide[i] * 0.1f + interferer[i];
    const auto y = derive_channel(wide, in_rate, 485e3, out_rate);
    check(y.size() == wide.size() / 10, "Derived length = input / decimation");
    const double wanted = tone_power(y, 0.0, 400);
    // The interferer at +300 kHz from the channel centre would alias to
    // -200 kHz in a 500 kHz-rate channel without filtering.
    const double alias = tone_power(y, -200e3 / out_rate, 400);
    check(near(10 * std::log10(wanted), 10 * std::log10(0.01), 0.3), "Wanted tone keeps its power (unity gain)",
          std::to_string(10 * std::log10(wanted)) + " dB");
    check(10 * std::log10(alias / wanted) < -40, "Out-of-channel interferer 20 dB stronger is suppressed > 40 dB after",
          std::to_string(10 * std::log10(alias / wanted)) + " dB re wanted");
    // Sample alignment: an impulse at input k*D lands at output k.
    std::vector<cf> impulse(100000);
    impulse[50000] = 1;
    const auto yi = derive_channel(impulse, in_rate, 0, out_rate);
    size_t peak = 0;
    for (size_t i = 1; i < yi.size(); ++i) if (std::abs(yi[i]) > std::abs(yi[peak])) peak = i;
    check(peak == 5000, "Filter delay removed: input sample 50000 maps to output 5000", std::to_string(peak));

    // Real SX1262 packet (500 kHz capture) embedded at +485 kHz in 5 MSps.
    const auto fixture = load_lora_capture(std::string(PROJECT_ROOT_DIR) + "/tests/fixtures/lora_sx1262/capture-1790061256825496286-0");
    const auto reference = analyze_lora_capture(fixture.iq, fixture.sample_rate_hz, 888e6);
    std::string reference_payload;
    for (const auto& r : reference) if (r.header_valid && r.crc == LoraPacketRow::Crc::Valid) { reference_payload = r.payload_hex; break; }
    check(!reference_payload.empty(), "Fixture decodes directly with a valid CRC");
    const auto wide_lora = embed(fixture.iq, 10, 485e3 / in_rate);
    const auto derived = derive_channel(wide_lora, in_rate, 485e3, out_rate);
    const auto rows = analyze_lora_capture(derived, out_rate, 888e6);
    bool same = false;
    for (const auto& r : rows) same = same || (r.header_valid && r.crc == LoraPacketRow::Crc::Valid && r.payload_hex == reference_payload);
    check(same, "Same packet decodes with valid CRC after 5 MSps embedding and channel derivation");
}
}  // namespace

int main() {
    schedule_tests();
    freshness_tests();
    channel_tests();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
