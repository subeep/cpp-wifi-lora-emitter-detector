#pragma once
#include "sample_tiles.hpp"
#include <algorithm>

namespace rfmon::cyclo {
inline constexpr std::size_t burst_hint_limit = 512;
struct BurstHints {
    std::array<SampleTile, burst_hint_limit> ranges{};
    std::size_t reported = 0, examined = 0, count = 0, rejected = 0;
    bool detector_capped = false;
};
// Reuse existing raw detector ranges, including unknown/narrowband outcomes.
// Inspect at most 512 evenly indexed descriptors; never scan IQ here.
template<class Getter>
BurstHints collect_burst_hints(std::size_t reported, std::size_t prefix,
                              bool detector_capped, Getter get) noexcept {
    BurstHints h; h.reported = reported; h.detector_capped = detector_capped;
    h.examined = std::min(reported, burst_hint_limit);
    for (std::size_t i = 0; i < h.examined; ++i) {
        const auto divisor = h.examined > 1 ? h.examined - 1 : 1;
        const auto span = reported ? reported - 1 : 0;
        const auto index = (span / divisor) * i + (span % divisor) * i / divisor;
        const auto b = get(index);
        if (!b.samples || b.source_offset >= prefix || b.samples > prefix - b.source_offset) {
            ++h.rejected; continue;
        }
        h.ranges[h.count++] = b;
    }
    return h;
}
struct TileSelection {
    TilePlan plan;
    std::array<bool, tile_limit> burst_guided{};
    std::size_t burst_windows = 0;
    // Counts concern the detector's returned ranges, not all RF activity.
    std::size_t hints_reported = 0, hints_examined = 0, hints_eligible = 0, hints_rejected = 0;
    bool detector_capped = false;
};
inline TileSelection select_sample_tiles(std::size_t prefix, const BurstHints& hints = {}) noexcept {
    TileSelection s; s.plan = plan_sample_tiles(prefix);
    s.hints_reported = hints.reported; s.hints_examined = hints.examined;
    s.hints_eligible = std::min(hints.count, burst_hint_limit); s.hints_rejected = hints.rejected;
    s.detector_capped = hints.detector_capped;
    // Short prefixes keep their existing plan. With a long prefix,
    // retain first/last context and replace at most two interior windows.
    if (prefix <= capture_sample_budget || !s.hints_eligible) return s;
    const auto baseline = s.plan;
    s.plan = {}; s.plan.count = 2;
    s.plan.tiles[0] = baseline.tiles[0]; s.plan.tiles[1] = baseline.tiles[baseline.count - 1];
    s.plan.samples = s.plan.tiles[0].samples + s.plan.tiles[1].samples;
    const auto add = [&](SampleTile b, bool guided) {
        if (s.plan.count >= tile_limit || !b.samples || b.source_offset >= prefix ||
            b.samples > prefix - b.source_offset || b.samples > tile_sample_limit) return false;
        for (std::size_t j = 0; j < s.plan.count; ++j) {
            const auto& t = s.plan.tiles[j];
            if (b.source_offset < t.source_offset + t.samples && t.source_offset < b.source_offset + b.samples)
                return false;
        }
        const auto n = s.plan.count++;
        s.plan.tiles[n] = b; s.burst_guided[n] = guided; s.plan.samples += b.samples;
        if (guided) ++s.burst_windows;
        return true;
    };
    for (std::size_t target = 0; target < 2; ++target) {
        // Search from the 1/3 and 2/3 descriptor positions, wrapping once.
        const auto start = (s.hints_eligible - 1) * (target + 1) / 3;
        for (std::size_t i = 0; i < s.hints_eligible; ++i) {
            const auto& b = hints.ranges[(start + i) % s.hints_eligible];
            if (!b.samples || b.source_offset >= prefix || b.samples > prefix - b.source_offset) continue;
            const auto length = std::min(tile_sample_limit, std::max(std::size_t(8192),
                std::min(b.samples, tile_sample_limit - 4096) + 4096));
            const auto middle = b.source_offset + b.samples / 2;
            const auto offset = std::min(prefix - length, middle > length / 2 ? middle - length / 2 : 0);
            if (add({offset, length}, true)) break;
        }
    }
    for (std::size_t i = 1; i + 1 < baseline.count; ++i) add(baseline.tiles[i], false);
    // Insertion sort preserves the small fixed storage and origin flags.
    for (std::size_t i = 1; i < s.plan.count; ++i)
        for (std::size_t j = i; j && s.plan.tiles[j].source_offset < s.plan.tiles[j - 1].source_offset; --j) {
            std::swap(s.plan.tiles[j], s.plan.tiles[j - 1]);
            std::swap(s.burst_guided[j], s.burst_guided[j - 1]);
        }
    return s;
}
} // namespace rfmon::cyclo
