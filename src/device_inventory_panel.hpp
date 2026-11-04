// Overview page: the combined device list (device_inventory.hpp) with
// technology filters, a search over every field, a recency filter and an
// inspector for the selected row.
#pragma once

#include <string>
#include <vector>

#include "device_inventory.hpp"

namespace rfmon::devices {

struct PanelState {
    Filter filter;
    char search[128] = {};
    int recency = 0;        // index into the "Seen" choices
    std::string selected;   // Device::key
    float height = 0;       // table height; 0 = default
};

void draw_panel(PanelState& state, const std::vector<Device>& devices, int64_t now_unix);

}  // namespace rfmon::devices
