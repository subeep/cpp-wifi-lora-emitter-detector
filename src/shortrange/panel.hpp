// "Bluetooth & Zigbee" page: decoder controls, per-channel coverage, the
// Bluetooth LE and 802.15.4 / Zigbee address lists side by side, inspectors
// and recent packets. Display only, apart from the explicit controls.
#pragma once

#include <string>

#include "shortrange/monitor.hpp"

namespace rfmon::shortrange {

struct PanelState {
    std::string selected_ble, selected_zigbee;  // device keys
    char ble_filter[128] = "", zigbee_filter[128] = "";
    float ble_table_height = 0, zigbee_table_height = 0;  // 0: default; dragged by the handle under each table
    bool merge_rotating = true;  // one row per probable device instead of per address
    int hide_after = 0;          // 0 never, 1: 2 min, 2: 10 min, 3: 30 min without packets
    std::string export_dir, export_message;
};

void draw_page(PanelState& state, Monitor& monitor, const std::string& active_band, bool connected);
// Bluetooth Remote ID broadcasts for the Drones page.
void draw_remote_id_section(const Snapshot& snapshot);

}  // namespace rfmon::shortrange
