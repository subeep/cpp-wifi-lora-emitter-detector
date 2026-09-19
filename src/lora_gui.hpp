#pragma once
#include "lora_observation.hpp"
namespace rfmon {
void draw_lora_packet_table(const std::vector<LoraPacketRow>& packets, float height);
void draw_lora_replay_panel();
}
