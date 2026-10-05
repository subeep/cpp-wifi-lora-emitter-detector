#pragma once
#include "shadow_worker.hpp"

namespace rfmon::cyclo {
// Display-only evidence panel; caller owns the separate opt-in control.
void draw_shadow_panel(const std::shared_ptr<const ShadowResult>& result,
                       const ShadowStats& stats, int active_band_ghz);
}
