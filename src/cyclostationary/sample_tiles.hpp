#pragma once
#include <array>
#include <cstddef>

namespace rfmon::cyclo {
inline constexpr std::size_t tile_limit = 4, tile_sample_limit = 65536, capture_sample_budget = 262144;
struct SampleTile { std::size_t source_offset = 0, samples = 0; };
struct TilePlan { std::array<SampleTile, tile_limit> tiles{}; std::size_t count = 0, samples = 0; };
// Spread independent windows over the known continuous prefix. No scanning,
// heap allocation or phase concatenation; source gaps remain outside the plan.
inline TilePlan plan_sample_tiles(std::size_t prefix) noexcept {
    TilePlan p;
    if (prefix < 2048) return p;
    const auto budget = prefix < capture_sample_budget ? prefix : capture_sample_budget;
    p.count = (budget + tile_sample_limit - 1) / tile_sample_limit;
    const auto length = budget / p.count;
    p.samples = p.count * length;
    for (std::size_t i = 0; i < p.count; ++i) {
        // Avoid multiplication of an unbounded input prefix by the tile index.
        const auto span = prefix - length, divisor = p.count > 1 ? p.count - 1 : 1;
        const auto offset = p.count > 1 ? (span / divisor) * i + (span % divisor) * i / divisor : 0;
        p.tiles[i] = {offset, length};
    }
    return p;
}
} // namespace rfmon::cyclo
