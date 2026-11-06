#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "lora_channel_plan.hpp"
#include "lora_master.hpp"
#include "lora_observation.hpp"
#include "security/lora_rules.hpp"
#include "security/lora_security.hpp"

namespace rfmon {

// Scanner callbacks the LoRa tab uses. Any may be empty (offline/test use):
// the matching controls are then disabled.
struct LoraTabActions {
    std::function<void(std::vector<double>)> set_channels;
    std::function<void(bool)> set_wideband;
    std::function<void(std::optional<double>)> set_wideband_rate;  // nullopt = narrowest that fits
    std::function<bool(int, const std::string&, const std::string&, const std::string&)> set_enrolment;
    std::function<bool(int, int)> merge;
    std::function<std::vector<lora_master::LoraMasterReading>(int)> history;
    std::function<bool(uint64_t)> acknowledge;
    std::function<void()> clear_closed;
};

struct LoraReceiverView {
    std::vector<double> channels;
    bool wideband = true;
    std::optional<double> wideband_rate_hz = 20e6;
    bool replaces_energy_scan = false;
    std::optional<double> locked_hz;
    double capture_seconds = 0;
    std::vector<lora_plan::Capture> plan;
    lora_plan::ListenStats stats;
    double now_steady_s = 0;
    std::vector<lora_plan::Suggestion> suggestions;
};

// Packet table. Default view: one row per capture-local reception (its best
// hypothesis), expandable to every SF/BW/polarity hypothesis.
void draw_lora_packet_table(const std::vector<LoraPacketRow>& packets, float height);
void draw_lora_replay_panel();
void draw_lora_security_panel(const lora_security::Snapshot& snapshot);

// LoRaWAN end devices named by CRC-valid frames.
void draw_lora_device_table(const std::vector<LoraDeviceRow>& devices, const lora_rules::Snapshot& incidents,
                            const std::vector<lora_master::LoraMasterRow>& emitters, float height);
// Persistent RF clusters with review labels, merge and reading history.
void draw_lora_master_table(const std::vector<lora_master::LoraMasterRow>& rows,
                            const std::vector<LoraDeviceRow>& devices, const LoraTabActions& actions, float height,
                            const std::string& storage_error = {});
// Keyless LoRaWAN rule incidents.
void draw_lora_incidents(const lora_rules::Snapshot& snapshot, const LoraTabActions& actions, float height);
// Channel plan, listen mode, measured coverage and channel suggestions.
void draw_lora_receiver_panel(const LoraReceiverView& view, const LoraTabActions& actions);
// What every LoRa view can and cannot establish (one collapsible block).
void draw_lora_interpretation_limits();

}  // namespace rfmon
