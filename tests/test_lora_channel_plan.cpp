// LoRa listen channel planning (src/lora_channel_plan.hpp).
#include "lora_channel_plan.hpp"

#include <cstdio>
#include <string>

using namespace rfmon;
using namespace rfmon::lora_plan;

namespace {
int failures = 0;
void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s [%s] %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    failures += !ok;
}
// Single-channel captures are decoded at their own rate, centred on the channel.
bool covers(const Capture& c) {
    if (!c.wideband) return c.channels_hz.size() == 1 && c.center_hz == c.channels_hz[0];
    for (double f : c.channels_hz)
        if (std::abs(f - c.center_hz) + 250e3 > usable_half_span(c.rate_hz) + 1) return false;
    return true;
}
}  // namespace

int main() {
    {  // Lab default: four channels in one wideband capture, centre clear of every channel.
        auto p = plan(LORA_LISTEN_CHANNELS_HZ, 500e3, 20e6, true);
        check(p.size() == 1 && p[0].wideband && p[0].channels_hz.size() == 4, "lab_default_one_capture",
              std::to_string(p.size()));
        if (!p.empty()) {
            check(covers(p[0]), "lab_default_covered");
            check(p[0].dc_clearance_hz >= 100e3, "lab_default_dc_clear", std::to_string(p[0].dc_clearance_hz));
            check(p[0].rate_hz <= 5e6, "lab_default_smallest_rate", std::to_string(p[0].rate_hz));
            const double ratio = p[0].rate_hz / 500e3;
            check(std::abs(ratio - std::round(ratio)) < 1e-9, "rate_divides_decode_rate");
        }
    }
    {  // Rotation mode: one capture per channel, centred on it.
        auto p = plan(LORA_LISTEN_CHANNELS_HZ, 500e3, 20e6, false);
        check(p.size() == 4 && !p[0].wideband && p[0].center_hz == p[0].channels_hz[0], "rotation_mode");
    }
    {  // A rate ceiling below every wideband rate falls back to rotation.
        auto p = plan(LORA_LISTEN_CHANNELS_HZ, 500e3, 1e6, true);
        check(p.size() == 4 && !p[0].wideband, "low_rate_ceiling_rotates");
    }
    {  // Widely spread channels split into several captures, each covered.
        auto p = plan({865.0625e6, 865.4025e6, 868.1e6, 868.3e6, 915e6}, 500e3, 4e6, true);
        bool all = true;
        size_t channels = 0;
        for (const auto& c : p) { all = all && covers(c); channels += c.channels_hz.size(); }
        check(p.size() == 3 && all && channels == 5, "spread_channels_split", std::to_string(p.size()));
    }
    {  // Every regional preset is fully covered by its plan.
        bool ok = true;
        for (const auto& preset : presets()) {
            size_t n = 0;
            for (const auto& c : plan(preset.channels_hz, 500e3, 20e6, true)) { ok = ok && covers(c); n += c.channels_hz.size(); }
            ok = ok && n == normalise(preset.channels_hz).size();
        }
        check(ok, "presets_covered");
    }
    {  // Parsing and formatting.
        std::string err;
        auto ch = parse_channels_mhz("866.9, 865.0625;865.0625  868.1", &err);
        check(ch.size() == 3 && ch[0] == 865.0625e6 && err.empty(), "parse_sorts_and_dedups", format_channels_mhz(ch));
        check(parse_channels_mhz("866.9, abc", &err).empty() && !err.empty(), "parse_rejects_text", err);
        check(parse_channels_mhz("12", &err).empty(), "parse_rejects_out_of_range", err);
        check(format_channels_mhz({865.0625e6, 866.9e6}) == "865.0625, 866.9000", "format");
    }
    {  // Suggestions: LoRa-like energy off-plan, merged, ranked by hits.
        std::vector<Observation> obs = {
            {866.9e6, 125e3, -40, 10, true},      // already planned
            {867.1003e6, 125e3, -50, 3, true},
            {867.12e6, 125e3, -45, 4, true},      // merges with the one above
            {869.525e6, 250e3, -60, 9, true},
            {868.0e6, 1e6, -30, 50, false},       // not LoRa-like
        };
        auto s = suggest(obs, LORA_LISTEN_CHANNELS_HZ);
        // 867.12 snaps to 867.125 and, being stronger, sets the merged centre.
        check(s.size() == 2 && s[0].freq_hz == 869.525e6 && s[1].freq_hz == 867.125e6 && s[1].hits == 7,
              "suggestions", s.empty() ? "none" : std::to_string(s[0].freq_hz));
    }
    {  // Preferred 20 MHz: one capture, also keeping the 863-868 MHz energy-scan range.
        PlanOptions o;
        o.max_rate = 20e6;
        o.preferred_rate = 20e6;
        o.cover_lo = 863e6;
        o.cover_hi = 868e6;
        auto p = plan(LORA_LISTEN_CHANNELS_HZ, o);
        check(p.size() == 1 && p[0].rate_hz == 20e6 && p[0].covers_range && covers(p[0]) && p[0].dc_clearance_hz >= 100e3,
              "lab_default_at_20mhz", p.empty() ? "none" : std::to_string(p[0].center_hz));
        check(single_capture_covers(p, 863e6, 868e6), "20mhz_replaces_energy_scan");
        auto broad = plan(presets().back().channels_hz, o);
        check(broad.size() == 1 && broad[0].channels_hz.size() == 13 && covers(broad[0]) && broad[0].covers_range,
              "in865_eu868_in_one_20mhz_capture");
        auto lone = plan({866.9e6}, o);
        check(lone.size() == 1 && lone[0].wideband && lone[0].rate_hz == 20e6 && lone[0].covers_range, "lone_channel_still_wideband");
        o.max_rate = 5e6;  // e.g. a long capture window: falls back to the narrowest fit
        auto fallback = plan(LORA_LISTEN_CHANNELS_HZ, o);
        check(fallback.size() == 1 && fallback[0].rate_hz <= 5e6 && !single_capture_covers(fallback, 863e6, 868e6),
              "20mhz_falls_back_when_not_allowed", std::to_string(fallback[0].rate_hz));
        auto far = plan({865.0625e6, 915e6}, o = PlanOptions{500e3, 20e6, true, 20e6, 863e6, 868e6});
        check(far.size() == 2 && far[0].covers_range && !far[1].covers_range, "far_channels_split_and_range_kept_where_possible");
    }
    if (failures) {
        std::printf("%d channel plan check(s) failed\n", failures);
        return 1;
    }
    std::printf("All LoRa channel plan checks passed\n");
}
