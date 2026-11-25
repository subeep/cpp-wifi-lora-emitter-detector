// Coordinated three-band mode, radio-free pure parts (upgradedplan.md P1):
// schedule defaults/validation/estimates, rotating cursors, configuration
// hash, coverage-aware freshness, and channel derivation - including a real
// archived SX1262 LoRa packet that must still decode after being placed in a
// 5 MSps sub-GHz capture and derived back to 500 kHz.
#include <algorithm>
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
    const char* names[] = {"Sub-GHz", "2.4 GHz sweep", "2.4 GHz Wi-Fi", "5.8 GHz", "5.2 GHz", "5.8 GHz gaps", "5.2 GHz gaps"};
    const int per_cycle[] = {1, 4, 1, 4, 1, 2, 1};
    const std::size_t sizes[] = {1, 7, 8, 7, 4, 5, 6};
    bool layout = s.slots.size() == 7;
    std::string got;
    for (std::size_t i = 0; layout && i < 7; ++i) {
        layout = s.slots[i].name == names[i] && s.slots[i].visits_per_cycle == per_cycle[i] &&
                 s.slots[i].visits.size() == sizes[i] && s.slots[i].dwell_s == 0.512 &&
                 s.slots[i].band == (i == 0 ? BAND_SUB_GHZ : i < 3 ? BAND_WIFI_2G4 : BAND_WIFI_5G);
        if (!layout) got = s.slots[i].name + " " + std::to_string(s.slots[i].visits.size());
    }
    check(layout, "Split C: Sub-GHz 1, 2.4 sweep 4 of 7, 2.4 Wi-Fi 1 of 8, 5.8 4 of 7, 5.2 1 of 4, 5.8 gaps 2 of 5, "
                  "5.2 gaps 1 of 6 per cycle, 512 ms each", got);
    std::vector<double> sweep, rest;
    for (const auto& v : s.slots[1].visits) sweep.push_back(v.center_hz);
    for (const auto& v : s.slots[2].visits) rest.push_back(v.center_hz);
    const auto ch = [](int c) { return wifi_2g4_channels().at(c) + WIFI_CHANNEL_CAPTURE_OFFSET_HZ; };
    check(sweep == std::vector<double>{2401e6, ch(1), ch(4), ch(7), ch(10), ch(13), sweep.back()} &&
              near(sweep.back(), 2482.3e6, 1e3) && s.slots[1].visits.back().kind == coord::VisitKind::DroneOnly,
          "2.4 GHz sweep: DroneID 2399.5, Wi-Fi ch 1, 4, 7, 10, 13 and the 2481.1-2483.5 MHz gap visit");
    check(rest == std::vector<double>{ch(2), ch(3), ch(5), ch(6), ch(8), ch(9), ch(11), ch(12)},
          "2.4 GHz Wi-Fi: the other eight channels, each centred");
    const double half = coord::usable_half_span(20e6);
    bool alone = true;
    for (double f = 2400e6; f <= 2483.5e6; f += 25e3)
        alone = alone && std::any_of(sweep.begin(), sweep.end(), [&](double c) { return std::abs(f - c) <= half; });
    for (double id : drone::droneid::channels_24)
        alone = alone && std::any_of(sweep.begin(), sweep.end(), [&](double c) {
                    return std::abs(id - c) <= coord::droneid_decode_reach_hz(20e6) && std::abs(id - c) > 40e3;
                });
    check(alone, "2.4 GHz sweep alone covers 2400-2483.5 MHz and decodes every 2.4 GHz DroneID channel");
    check(s.slots[3].visits.size() == 7 && s.slots[3].visits[5].center_hz == 5758e6 && s.slots[3].visits[6].center_hz == 5778e6 &&
              s.slots[3].visits[5].kind == coord::VisitKind::DroneOnly && s.slots[3].visits[0].center_hz == 5746.5e6,
          "5.8 GHz: Wi-Fi ch 149-165, then DroneID 5756.5 and 5776.5 MHz (channel + 1.5 MHz)");
    check(s.slots[4].visits.front().center_hz == 5181.5e6 && s.slots[4].visits.back().center_hz == 5241.5e6,
          "5.2 GHz: Wi-Fi ch 36-48");
    bool gaps = true;
    for (std::size_t i : {std::size_t(5), std::size_t(6)})
        for (const auto& v : s.slots[i].visits)
            gaps = gaps && v.kind == coord::VisitKind::DroneOnly && v.sample_rate_hz == 20e6 &&
                   ((i == 5) == (v.center_hz > 5.5e9));
    check(gaps, "Gap slots hold only drone-only visits of their own band");

    const auto e = coord::estimate(s);
    check(near(e.valid_s, 14 * 0.512, 1e-9), "Valid observation per cycle is 14 x 0.512 s", std::to_string(e.valid_s));
    check(near(e.overhead_s, 14 * (0.008 + RETUNE_SETTLE_S), 1e-9), "Overhead is 14 x (pre-roll + settle)");
    check(near(e.cycle_s, e.valid_s + e.overhead_s, 1e-12), "Cycle = valid + overhead (no inference term)");
    const double revisit[] = {1, 1.75, 8, 1.75, 4, 2.5, 6};
    bool rev = true;
    for (std::size_t i = 0; i < 7; ++i) rev = rev && near(e.slots[i].sweep_s, revisit[i] * e.cycle_s, 1e-9);
    check(rev, "Average revisit per centre = centres / visits per cycle x cycle (1, 1.75, 8, 1.75, 4, 2.5, 6 cycles)");
    auto renamed = s;
    renamed.slots[4].name = "other";
    check(coord::schedule_hash(renamed) == coord::schedule_hash(s), "A slot's display name is not part of the hash");
    const auto wide = coord::default_schedule(56e6);
    bool capped = true;
    for (const auto& slot : wide.slots) capped = capped && slot.visits_per_cycle <= int(slot.visits.size());
    check(capped && wide.slots[1].visits.size() < 7, "56 MSps: fewer sweep centres, never more visits per cycle than centres");

    // Rotating cursor resumes where the previous cycle stopped and wraps.
    size_t cursor = 0;
    auto a = coord::next_visits(s.slots[3], cursor);
    auto b = coord::next_visits(s.slots[3], cursor);
    check(a == std::vector<size_t>({0, 1, 2, 3}) && b == std::vector<size_t>({4, 5, 6, 0}) && cursor == 1,
          "5.8 GHz cursor: 0-3, then 4,5,6,0; resumes at 1");
    coord::Slot one = s.slots[0];
    one.visits_per_cycle = 2;
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
    bad.slots[3].visits[0].band = BAND_WIFI_2G4;
    check(!coord::validate(bad, 20e6).empty(), "A visit in the wrong band slot is rejected");
}

// Every frequency of the drone bands inside some visit's usable span, and every documented
// DroneID channel within the decoder's reach of some centre (off its DC notch).
std::vector<double> drone_centres(const coord::Schedule& s) {
    std::vector<double> out;
    for (const auto& slot : s.slots)
        if (slot.band != BAND_SUB_GHZ) for (const auto& v : slot.visits) out.push_back(v.center_hz);
    return out;
}
bool covers_bands(const std::vector<double>& centres, double rate, std::string& missed) {
    const double half = coord::usable_half_span(rate);
    for (const auto& band : coord::drone_bands)
        for (double f = band.low_hz; f <= band.high_hz; f += 25e3) {
            const bool in = std::any_of(centres.begin(), centres.end(), [&](double c) { return std::abs(f - c) <= half; });
            if (!in) { char b[64]; std::snprintf(b, sizeof b, "%.3f MHz", f / 1e6); missed = b; return false; }
        }
    return true;
}
bool reaches_droneid(const std::vector<double>& centres, double rate, std::string& missed) {
    const double reach = coord::droneid_decode_reach_hz(rate);
    std::vector<double> channels(std::begin(drone::droneid::channels_24), std::end(drone::droneid::channels_24));
    channels.insert(channels.end(), std::begin(drone::droneid::channels_58), std::end(drone::droneid::channels_58));
    for (double ch : channels)
        if (!std::any_of(centres.begin(), centres.end(), [&](double c) { return std::abs(ch - c) <= reach && std::abs(ch - c) > 40e3; })) {
            missed = std::to_string(ch / 1e6) + " MHz";
            return false;
        }
    return true;
}

void coverage_tests() {
    using Gap = std::pair<double, double>;
    const auto gaps = coord::uncovered(0, 40e6, {10e6, 30e6}, 5e6);
    check(gaps.size() == 3 && gaps[0] == Gap{0, 5e6} && gaps[1] == Gap{15e6, 25e6} && gaps[2] == Gap{35e6, 40e6},
          "uncovered(): the band edges and the space between two spans");
    check(coord::uncovered(0, 10e6, {5e6}, 5e6).empty() && coord::uncovered(0, 10e6, {}, 5e6).size() == 1 &&
              coord::uncovered(0, 10e6, {5e6}, 4.9995e6).empty(),
          "uncovered(): nothing left when one span reaches both edges (slivers under 1 kHz ignored); all of it with none");

    // Why the drone slots exist: the Wi-Fi channel visits alone leave gaps and miss 5.8 GHz DroneID.
    std::vector<double> wifi_only;
    for (const auto& [ch, f] : wifi_2g4_channels()) wifi_only.push_back(f + WIFI_CHANNEL_CAPTURE_OFFSET_HZ);
    for (const auto& [ch, f] : wifi_5g_channels()) wifi_only.push_back(f + WIFI_CHANNEL_CAPTURE_OFFSET_HZ);
    wifi_only.push_back(coord::DRONEID_2399_5_HZ + WIFI_CHANNEL_CAPTURE_OFFSET_HZ);
    std::string missed;
    check(!covers_bands(wifi_only, 20e6, missed) && !reaches_droneid(wifi_only, 20e6, missed) && missed == "5756.500000 MHz",
          "The old 2.4 + 5 GHz lists leave gaps and cannot decode 5756.5 MHz DroneID", missed);

    for (double rate : {20e6, 25e6, 56e6}) {
        const auto s = coord::default_schedule(rate);
        const auto centres = drone_centres(s);
        char name[96];
        std::snprintf(name, sizeof name, "%.0f MSps: 2400-2483.5, 5150-5250 and 5725-5850 MHz all covered", rate / 1e6);
        missed.clear();
        check(covers_bands(centres, rate, missed), name, missed);
        std::snprintf(name, sizeof name, "%.0f MSps: every documented DroneID channel decodable on some visit", rate / 1e6);
        missed.clear();
        check(reaches_droneid(centres, rate, missed), name, missed);
        std::size_t wifi = 0;
        for (const auto& slot : s.slots) for (const auto& v : slot.visits) wifi += v.kind == coord::VisitKind::WifiChannel;
        std::snprintf(name, sizeof name, "%.0f MSps: the 22 Wi-Fi channel visits are unchanged; additions are drone-only", rate / 1e6);
        check(wifi == 22, name, std::to_string(wifi));
    }
    std::size_t added25 = 0;
    for (const auto& slot : coord::default_schedule(25e6).slots)
        for (const auto& v : slot.visits) added25 += v.kind == coord::VisitKind::DroneOnly;
    check(added25 == 1 + 12, "25 MSps: wider captures need fewer added visits (12 + DroneID 2399.5)", std::to_string(added25));
    const auto low = coord::default_schedule(2e6);
    check(low.slots.size() == 3 && low.slots[1].visits.size() == 14 && low.slots[2].visits.size() == 9,
          "Below 10 MSps no drone slots are added (it would take dozens of visits)");
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
    coverage_tests();
    freshness_tests();
    channel_tests();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
