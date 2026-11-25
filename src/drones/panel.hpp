#pragma once
#include "addon.hpp"
namespace drone {
void draw_remote_id(const RidJson& snapshot);
void draw_results(const Snapshot& snapshot, bool offline);
// Native DJI packet / O4 evidence counters and inspectors.
void draw_native_results(const Snapshot& snapshot, bool offline);
// The live decoder sections and the offline raw-IQ review, drawn by draw_panel and by the Drones page.
void draw_decoder_sections(Addon& addon);
void draw_offline_review(Addon& addon);
void draw_panel(Addon& addon, const std::string& band, bool connected, const char* select_tab = nullptr);
}
