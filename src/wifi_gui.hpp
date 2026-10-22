#pragma once
#include "scanner.hpp"
namespace rfmon {
void draw_wifi_packet_table(const std::vector<WifiPacketRow>& packets, float height);
void draw_wifi_master_table(const std::vector<wifi_master::WifiMasterRow>& rows, float height, bool inline_inspector = false,
    const std::function<bool(wifi_master::EnrolmentCommand)>& command = {}, int band = 0);
}
