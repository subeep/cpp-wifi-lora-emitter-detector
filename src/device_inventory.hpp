// Combined device inventory for the Overview page: every device that a
// decoder named by a unique identifier in integrity-checked traffic, across
// receivers. Wi-Fi MAC addresses (2.4 and 5 GHz, FCS-valid frames),
// Bluetooth LE addresses (CRC-valid advertising), Zigbee / 802.15.4
// addresses (FCS-valid frames) and LoRaWAN DevAddr / DevEUI (CRC-valid
// frames). Anonymous RF clusters (WIFI-FP-*, LORA-*) are never included.
//
// Pure data: no ImGui, no receiver access. A new source is one builder
// below returning Device records; the panel and filters need no change
// beyond a Technology entry.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lora_observation.hpp"
#include "shortrange/monitor.hpp"
#include "wifi_master.hpp"

namespace rfmon::devices {

enum class Technology { Wifi24, Wifi5, Bluetooth, Zigbee, Lora, Count };
constexpr size_t kTechnologies = size_t(Technology::Count);
const char* technology_name(Technology t);

struct Device {
    Technology technology = Technology::Wifi24;
    std::string key;              // unique across the inventory (technology + identifier)
    std::string identifier;       // MAC / BLE address / IEEE or PAN + short address / DevAddr / DevEUI
    std::string identifier_kind;  // e.g. "MAC (global)", "random static", "IEEE 64-bit", "DevAddr"
    std::string name;             // SSID / alias, advertised name, Zigbee network; empty if none
    std::string role, vendor;
    std::string channel;          // channel(s) or frequency, as text
    uint64_t observations = 0;
    std::string observations_unit;  // "frames", "packets", "observations"
    std::optional<double> power_db; // latest relative power, where the source reports it
    int64_t first_seen = 0, last_seen = 0;  // Unix seconds; 0 = not reported
    std::vector<std::pair<std::string, std::string>> details;  // inspector rows; also searched
};

// Converts a steady-clock time point to Unix seconds using one reference
// pair taken at the same moment; 0 for the epoch (never seen).
int64_t to_unix(std::chrono::steady_clock::time_point t, std::chrono::steady_clock::time_point now_steady, int64_t now_unix);

std::vector<Device> from_wifi(const std::vector<wifi_master::WifiMasterRow>& rows);
// Rotating private addresses linked by the monitor appear as one device.
std::vector<Device> from_bluetooth(const shortrange::Snapshot& s, std::chrono::steady_clock::time_point now_steady, int64_t now_unix);
std::vector<Device> from_zigbee(const shortrange::Snapshot& s, std::chrono::steady_clock::time_point now_steady, int64_t now_unix);
std::vector<Device> from_lora(const std::vector<LoraDeviceRow>& rows);

struct Filter {
    std::array<bool, kTechnologies> show{};  // all false is treated as "none selected"
    std::string search;                       // case-insensitive, over every text field and detail
    bool named_only = false;
    int64_t seen_within_s = 0;                // 0 = any time
    Filter() { show.fill(true); }
};
bool matches(const Device& d, const Filter& f, int64_t now_unix);

}  // namespace rfmon::devices
