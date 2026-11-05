// Combined device list: what each source contributes, what is excluded, and
// the filters. No receiver, no GUI.
#include "device_inventory.hpp"
#include "lorawan_inspect.hpp"
#include <iostream>

using namespace rfmon;
using devices::Technology;

int main() {
    int failures = 0, checks = 0;
    auto check = [&](bool ok, const std::string& name) { ++checks; if (!ok) ++failures; std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; };
    const int64_t now = 1793800000;
    const auto steady = std::chrono::steady_clock::now();

    // Wi-Fi: MAC entries only; band from the advertised channel, else the capture.
    std::vector<wifi_master::WifiMasterRow> wifi(4);
    wifi[0].device_key = "98:ba:5f:37:57:4a"; wifi[0].key_is_mac = true; wifi[0].last_channel_hz = 2452e6;
    wifi[0].identity = wifi::BeaconInfo{}; wifi[0].identity->ssid = "Upstairs"; wifi[0].identity->channel = 9;
    wifi[0].first_seen_ts = now - 100; wifi[0].last_seen_ts = now - 5; wifi[0].vendor = "Example";
    wifi[1].device_key = "ce:11:22:33:44:55"; wifi[1].key_is_mac = true; wifi[1].last_channel_hz = 5180e6; wifi[1].last_seen_ts = now - 7200;
    wifi[2].device_key = "WIFI-FP-0001"; wifi[2].last_channel_hz = 2412e6;  // anonymous cluster: excluded
    wifi[3].device_key = "aa:bb:cc:00:00:01"; wifi[3].key_is_mac = true; wifi[3].last_channel_hz = 2412e6;
    wifi[3].identity = wifi::BeaconInfo{}; wifi[3].identity->channel = 44;  // advertised 5 GHz channel wins
    const auto w = devices::from_wifi(wifi);
    check(w.size() == 3, "Wi-Fi: anonymous RF clusters are not listed");
    check(w[0].technology == Technology::Wifi24 && w[0].name == "Upstairs" && w[0].channel == "ch 9" &&
          w[0].identifier_kind == "MAC (global)" && w[0].role == "AP / network interface", "Wi-Fi: access point fields");
    check(w[1].technology == Technology::Wifi5 && w[1].identifier_kind.find("randomised") != std::string::npos &&
          w[1].channel == "ch 36", "Wi-Fi: 5 GHz client, locally administered MAC");
    check(w[2].technology == Technology::Wifi5 && w[2].channel == "ch 44", "Wi-Fi: band from the advertised channel");

    // Bluetooth: one row per probable device (linked rotating addresses); Zigbee as reported.
    shortrange::Snapshot s;
    shortrange::BleDevice b;
    b.key = b.address = "C0:FF:EE:12:34:56"; b.address_kind = "random static"; b.name = "RFMON-RID-TEST";
    b.roles = {"Advertiser"}; b.channel_counts = {{37, 4}, {38, 2}}; b.packets = 6; b.last_power_db = -40;
    b.first_seen = steady - std::chrono::seconds(30); b.last_seen = steady - std::chrono::seconds(2);
    b.group = 7; b.group_addresses = {"4A:00:00:00:00:01", "C0:FF:EE:12:34:56"};
    s.ble_grouped = {b};
    s.ble = {b, b};  // every address: not used, the grouped view is
    shortrange::ZigbeeDevice z;
    z.key = z.address = "PAN 0x1A62 / 0x0000"; z.pan = 0x1A62; z.short_addr = 0; z.roles = {"Coordinator"};
    z.channel_counts = {{15, 3}}; z.frames = 3; z.secured_seen = true; z.network = "Zigbee";
    s.zigbee = {z};
    const auto bt = devices::from_bluetooth(s, steady, now);
    const auto zb = devices::from_zigbee(s, steady, now);
    check(bt.size() == 1 && bt[0].name == "RFMON-RID-TEST" && bt[0].channel == "BLE 37, 38" && bt[0].observations == 6 &&
          bt[0].power_db && *bt[0].power_db == -40, "Bluetooth: grouped device fields");
    check(bt[0].last_seen == now - 2 && bt[0].first_seen == now - 30, "Bluetooth: steady clock converted to Unix time");
    bool linked = false;
    for (const auto& [k, v] : bt[0].details) linked |= v.find("4A:00:00:00:00:01") != std::string::npos;
    check(linked, "Bluetooth: linked addresses listed in the details");
    check(zb.size() == 1 && zb[0].identifier_kind == "PAN + short address" && zb[0].channel == "ch 15" && zb[0].name.empty(),
          "Zigbee: device fields; the protocol label is not a name");
    check(devices::to_unix({}, steady, now) == 0, "Never-seen time stays 0");

    // LoRaWAN: identifiers come from the structural parse of CRC-valid frames.
    const std::vector<uint8_t> uplink = {0x40, 0xDA, 0x1B, 0x01, 0x26, 0x00, 0x05, 0x00, 0x01, 0xAA, 0x11, 0x22, 0x33, 0x44};
    const auto c = inspect_lorawan(uplink);
    check(c && c->device_kind == "DevAddr" && c->device_id == "26011BDA" && c->uplink, "LoRaWAN: DevAddr from an uplink");
    std::vector<uint8_t> join(23, 0);
    for (int i = 0; i < 8; ++i) join[size_t(9 + i)] = uint8_t(0x10 + i);
    const auto j = inspect_lorawan(join);
    check(j && j->device_kind == "DevEUI" && j->device_id == "1716151413121110", "LoRaWAN: DevEUI from a join request");
    LoraDeviceRow lr;
    lr.kind = "DevAddr"; lr.id = "26011BDA"; lr.frames = 2; lr.uplinks = 2; lr.last_freq_hz = 868.1e6; lr.last_sf = 7;
    lr.first_seen_ts = now - 50; lr.last_seen_ts = now - 40;
    const auto lo = devices::from_lora({lr});
    check(lo.size() == 1 && lo[0].identifier == "26011BDA" && lo[0].channel == "868.100 MHz SF7", "LoRaWAN: device fields");

    // Filters.
    std::vector<devices::Device> all = w;
    for (const auto* part : {&bt, &zb, &lo}) all.insert(all.end(), part->begin(), part->end());
    auto count = [&](const devices::Filter& f) { size_t n = 0; for (const auto& d : all) n += devices::matches(d, f, now); return n; };
    devices::Filter f;
    check(count(f) == 6, "Filter: everything by default");
    f.show[size_t(Technology::Wifi24)] = f.show[size_t(Technology::Wifi5)] = false;
    check(count(f) == 3, "Filter: technology toggles");
    f = {};
    f.search = "upstairs";
    check(count(f) == 1, "Filter: search is case-insensitive over names");
    f.search = "coordinator";
    check(count(f) == 1, "Filter: search covers roles");
    f.search = "4a:00:00";
    check(count(f) == 1, "Filter: search covers details (linked addresses)");
    f = {};
    f.named_only = true;
    check(count(f) == 2, "Filter: named only (an SSID and a BLE name; 802.15.4 has no names)");
    f = {};
    f.seen_within_s = 60;
    check(count(f) == 3, "Filter: seen within a minute");

    std::cout << checks - failures << "/" << checks << " checks passed\n";
    return failures ? 1 : 0;
}
