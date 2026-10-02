#pragma once
#include "lora_observation.hpp"
#include "security/lora_security.hpp"
namespace rfmon {
void draw_lora_packet_table(const std::vector<LoraPacketRow>& packets, float height);
void draw_lora_replay_panel();
void draw_lora_security_panel(const lora_security::Snapshot& snapshot);
}
