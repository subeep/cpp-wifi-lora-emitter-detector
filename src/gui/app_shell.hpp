#pragma once
#include "imgui.h"
#include <functional>
#include <string>

namespace rfmon::gui {
enum class Page { Overview, Wifi24, Wifi5, Lora, Drones, Security, Captures, Settings };
struct State {
    Page page = Page::Overview;
    bool setup_open = false;
    bool legacy = false;
    bool compact = false;
    float scale = 1.0f;
    bool security_first = true;
    bool drones_offline = false;
};
// Display-only context: the shell has no receiver, worker or storage access.
struct Context {
    std::string radio, band, step, error, recording, run;
    bool started = false, connected = false, stalled = false, overflow = false;
    bool recording_pending = false, laboratory = false;
    size_t incidents = 0;
    int cycles = 0;
};
std::string band_label(const std::string& band);
const char* page_name(Page page);
const char* page_purpose(Page page);
void configure_font();
void apply_theme(float scale = 1, bool compact = false);
void draw_workspace(State& state, const Context& context, bool drones_available,
                    const std::function<void(Page)>& draw_page);
void draw_capabilities(bool drones_available, bool cyclo_available);
bool section(const char* label, bool primary = false);
void note(const char* text, bool warning = false);
}

namespace rfmon::gui {
struct Overview {
    size_t captures=0, groups=0, queue_depth=0, queue_peak=0, lane_peak=0;
    bool queue_loss=false, lane_enabled=false, dji_enabled=false, rid_enabled=false;
    double lane_wait=0;
};
void draw_overview(State& state,const Context& context,const Overview& overview,bool drones_available,
                   const std::function<void()>& draw_map);
}
