#pragma once
#include "addon.hpp"
namespace drone {
void draw_remote_id(const RidJson& snapshot);
void draw_results(const Snapshot& snapshot, bool offline);
void draw_panel(Addon& addon, const std::string& band, bool connected, const char* select_tab = nullptr);
}
