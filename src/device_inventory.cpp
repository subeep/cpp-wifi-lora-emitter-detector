#include "device_inventory.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "wifi_enrolment_types.hpp"
#include "wifi_frame.hpp"

namespace rfmon::devices {
namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string join(const std::set<std::string>& items) {
    std::string out;
    for (const auto& i : items) out += (out.empty() ? "" : ", ") + i;
    return out;
}
std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& i : items) out += (out.empty() ? "" : ", ") + i;
    return out;
}
template <class Map> std::string channels(const char* prefix, const Map& counts) {
    std::string out;
    for (const auto& [ch, n] : counts) out += (out.empty() ? std::string(prefix) + " " : ", ") + std::to_string(ch);
    return out;
}
std::string number(uint64_t v) { return std::to_string(v); }
std::string mhz(double hz) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f MHz", hz / 1e6);
    return b;
}
// IEEE MAC addresses: the locally administered bit marks randomised and
// virtual interfaces; a global address carries a registered OUI.
std::string mac_kind(const std::string& mac) {
    unsigned first = 0;
    if (std::sscanf(mac.c_str(), "%2x", &first) != 1) return "MAC";
    return (first & 0x02) ? "MAC (locally administered / randomised)" : "MAC (global)";
}
int wifi_channel(double hz) {
    if (hz >= 2.4e9 && hz < 2.5e9) return hz >= 2.484e9 ? 14 : int((hz - 2.407e9) / 5e6 + 0.5);
    if (hz >= 5e9 && hz < 6e9) return int((hz - 5e9) / 5e6 + 0.5);
    return 0;
}

}  // namespace

const char* technology_name(Technology t) {
    switch (t) {
        case Technology::Wifi24: return "Wi-Fi 2.4 GHz";
        case Technology::Wifi5: return "Wi-Fi 5 GHz";
        case Technology::Bluetooth: return "Bluetooth LE";
        case Technology::Zigbee: return "Zigbee / 802.15.4";
        case Technology::Lora: return "LoRaWAN";
        default: return "?";
    }
}

int64_t to_unix(std::chrono::steady_clock::time_point t, std::chrono::steady_clock::time_point now_steady, int64_t now_unix) {
    if (t.time_since_epoch().count() == 0) return 0;
    return now_unix - std::chrono::duration_cast<std::chrono::seconds>(now_steady - t).count();
}

std::vector<Device> from_wifi(const std::vector<wifi_master::WifiMasterRow>& rows) {
    std::vector<Device> out;
    for (const auto& r : rows) {
        if (!r.key_is_mac) continue;  // anonymous RF clusters carry no identifier
        Device d;
        // Band: the advertised channel when a beacon named it, else the last capture.
        const bool five = r.identity && r.identity->channel ? r.identity->channel > 14 : r.last_channel_hz >= 4e9;
        d.technology = five ? Technology::Wifi5 : Technology::Wifi24;
        d.identifier = r.device_key;
        d.key = std::string(technology_name(d.technology)) + " " + r.device_key;
        d.identifier_kind = mac_kind(r.device_key);
        const std::string alias = r.enrolment ? r.enrolment->alias : "";
        const std::string ssid = r.identity ? wifi::display_text(r.identity->ssid) : "";
        d.name = !alias.empty() ? alias : ssid;
        d.role = r.enrolment ? r.enrolment->role : r.identity ? "AP / network interface" : "Observed interface";
        d.vendor = r.vendor;
        const int ch = r.identity && r.identity->channel ? r.identity->channel : wifi_channel(r.last_channel_hz);
        d.channel = ch ? "ch " + std::to_string(ch) : r.last_channel_hz > 0 ? mhz(r.last_channel_hz) : "";
        d.observations = r.enrolment && r.enrolment->observations ? r.enrolment->observations
                                                                   : uint64_t(r.reading_count) + r.identity_count;
        d.observations_unit = "observations";
        d.first_seen = r.first_seen_ts;
        d.last_seen = r.last_seen_ts;
        if (r.identity) {
            d.details.push_back({"SSID", ssid.empty() ? "(hidden)" : ssid});
            if (!alias.empty()) d.details.push_back({"Alias", alias});
            if (!r.identity->security.empty()) d.details.push_back({"Advertised security", r.identity->security});
            if (!r.identity->standards.empty()) d.details.push_back({"Advertised standards", r.identity->standards});
            if (r.identity->bss_color >= 0) d.details.push_back({"BSS colour", std::to_string(r.identity->bss_color)});
            if (!r.identity->wps_device_name.empty()) d.details.push_back({"WPS device", r.identity->wps_device_name});
        }
        if (r.enrolment) d.details.push_back({"Enrolment", wifi_master::enrolment_label(*r.enrolment)});
        if (!r.vendor_source.empty()) d.details.push_back({"Vendor source", r.vendor_source});
        if (!r.last_phy.empty()) d.details.push_back({"Last PHY", r.last_phy});
        d.details.push_back({"RF readings / identity frames", number(uint64_t(r.reading_count)) + " / " + number(r.identity_count)});
        for (const auto& i : r.related_incidents) d.details.push_back({"Security incident", i});
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<Device> from_bluetooth(const shortrange::Snapshot& s, std::chrono::steady_clock::time_point now_steady, int64_t now_unix) {
    std::vector<Device> out;
    for (const auto& b : s.ble_grouped) {
        Device d;
        d.technology = Technology::Bluetooth;
        d.identifier = b.address;
        d.key = std::string("Bluetooth ") + (b.group ? "group " + std::to_string(b.group) : b.key);
        d.identifier_kind = b.address_kind;
        d.name = b.name;
        d.role = join(b.roles);
        d.vendor = !b.vendor.empty() ? b.vendor : b.company;
        d.channel = channels("BLE", b.channel_counts);
        d.observations = b.packets;
        d.observations_unit = "packets";
        if (b.last_power_db > -199) d.power_db = b.last_power_db;
        d.first_seen = to_unix(b.first_seen, now_steady, now_unix);
        d.last_seen = to_unix(b.last_seen, now_steady, now_unix);
        if (!b.company.empty()) d.details.push_back({"Company", b.company});
        if (!b.appearance.empty()) d.details.push_back({"Appearance", b.appearance});
        if (!b.services.empty()) d.details.push_back({"Services", join(b.services)});
        if (!b.tags.empty()) d.details.push_back({"Tags", join(b.tags)});
        if (b.tx_power_dbm) d.details.push_back({"Advertised TX power", std::to_string(*b.tx_power_dbm) + " dBm"});
        if (b.group_addresses.size() > 1)
            d.details.push_back({"Addresses (provisionally linked, oldest first)", join(b.group_addresses)});
        if (!b.link_reason.empty()) d.details.push_back({"Link reason", b.link_reason});
        if (!b.remote_id.empty()) d.details.push_back({"Remote ID", std::to_string(b.remote_id.size()) + " message types"});
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<Device> from_zigbee(const shortrange::Snapshot& s, std::chrono::steady_clock::time_point now_steady, int64_t now_unix) {
    std::vector<Device> out;
    for (const auto& z : s.zigbee) {
        Device d;
        d.technology = Technology::Zigbee;
        d.identifier = z.address;
        d.key = "Zigbee " + z.key;
        d.identifier_kind = z.ext ? "IEEE 64-bit" : "PAN + short address";  // 802.15.4 frames carry no names
        d.role = join(z.roles);
        d.vendor = z.vendor;
        d.channel = channels("ch", z.channel_counts);
        d.observations = z.frames;
        d.observations_unit = "frames";
        if (z.last_power_db > -199) d.power_db = z.last_power_db;
        d.first_seen = to_unix(z.first_seen, now_steady, now_unix);
        d.last_seen = to_unix(z.last_seen, now_steady, now_unix);
        if (!z.network.empty()) d.details.push_back({"Network protocol", z.network});
        char b[64];
        if (z.pan) { std::snprintf(b, sizeof b, "0x%04X", *z.pan); d.details.push_back({"PAN ID", b}); }
        if (z.ext_pan) { std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(*z.ext_pan)); d.details.push_back({"Extended PAN ID", b}); }
        d.details.push_back({"Security", z.secured_seen && z.unsecured_seen ? "secured and unsecured frames"
                                         : z.secured_seen ? "secured frames" : "unsecured frames"});
        if (z.association_permit) d.details.push_back({"Association", "permit join advertised"});
        std::string frames;
        for (const auto& [type, n] : z.frame_counts) frames += (frames.empty() ? "" : ", ") + type + " " + number(n);
        if (!frames.empty()) d.details.push_back({"Frame types", frames});
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<Device> from_lora(const std::vector<LoraDeviceRow>& rows) {
    std::vector<Device> out;
    for (const auto& r : rows) {
        Device d;
        d.technology = Technology::Lora;
        d.identifier = r.id;
        d.key = "LoRaWAN " + r.kind + " " + r.id;
        d.identifier_kind = r.kind;
        d.role = r.uplinks ? "End device (uplink seen)" : "End device (downlinks only)";
        char b[64];
        std::snprintf(b, sizeof b, "%.3f MHz SF%d", r.last_freq_hz / 1e6, r.last_sf);
        d.channel = b;
        d.observations = r.frames;
        d.observations_unit = "frames";
        d.first_seen = r.first_seen_ts;
        d.last_seen = r.last_seen_ts;
        d.details.push_back({"Uplinks / downlinks", number(r.uplinks) + " / " + number(r.downlinks)});
        d.details.push_back({"Last frame", r.last_type});
        d.details.push_back({"Integrity", "LoRa CRC valid; LoRaWAN MIC not verified (structural identifier)"});
        out.push_back(std::move(d));
    }
    return out;
}

bool matches(const Device& d, const Filter& f, int64_t now_unix) {
    if (!f.show[size_t(d.technology)]) return false;
    if (f.named_only && d.name.empty()) return false;
    if (f.seen_within_s > 0 && (d.last_seen == 0 || now_unix - d.last_seen > f.seen_within_s)) return false;
    if (f.search.empty()) return true;
    const std::string needle = lower(f.search);
    std::string hay = lower(std::string(technology_name(d.technology)) + " " + d.identifier + " " + d.identifier_kind + " " +
                            d.name + " " + d.role + " " + d.vendor + " " + d.channel);
    for (const auto& [k, v] : d.details) hay += " " + lower(k) + " " + lower(v);
    return hay.find(needle) != std::string::npos;
}

}  // namespace rfmon::devices
