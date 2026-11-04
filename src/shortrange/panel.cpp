#include "shortrange/panel.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "imgui.h"

namespace rfmon::shortrange {
namespace {

const ImVec4 kWarn(.98f, .73f, .38f, 1), kMuted(.67f, .76f, .84f, 1), kGood(.45f, .85f, .55f, 1);

double age_s(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
std::string age_text(Clock::time_point t) {
    if (t == Clock::time_point{}) return "never";
    const double a = age_s(t);
    char b[32];
    if (a < 120) std::snprintf(b, sizeof b, "%.0f s ago", a);
    else if (a < 7200) std::snprintf(b, sizeof b, "%.0f min ago", a / 60);
    else std::snprintf(b, sizeof b, "%.1f h ago", a / 3600);
    return b;
}
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool matches(const char* filter, std::initializer_list<const std::string*> fields) {
    if (!filter[0]) return true;
    const auto f = lower(filter);
    for (const auto* s : fields)
        if (lower(*s).find(f) != std::string::npos) return true;
    return false;
}
std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}
template <class Set>
std::string join_set(const Set& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}
template <class Map>
std::string channels(const Map& m) {
    std::string s;
    for (const auto& [c, k] : m) s += (s.empty() ? "" : " ") + std::to_string(c);
    return s;
}
template <class Fields>
void field_table(const char* id, const Fields& fields) {
    if (fields.empty()) { ImGui::TextDisabled("None received."); return; }
    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 210);
        ImGui::TableSetupColumn("Decoded value");
        for (const auto& f : fields) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", f.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", f.value.c_str());
        }
        ImGui::EndTable();
    }
}

// Table flags: resizable, reorderable and hideable columns (right-click a
// header to show/hide one). No click-to-sort: rows keep their fixed order.
constexpr ImGuiTableFlags kListFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollX |
                                       ImGuiTableFlags_ScrollY;
// Columns whose values change with every packet start hidden, so a row's
// visible change while updating is its packet count.
constexpr ImGuiTableColumnFlags kLive = ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide;

// Drag handle under a table: changes `height` (persisted in PanelState).
void height_handle(const char* id, float& height) {
    ImGui::InvisibleButton(id, ImVec2(-1, 8));
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) height = std::clamp(height + ImGui::GetIO().MouseDelta.y, 120.f, 4000.f);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float y = (a.y + b.y) / 2;
    ImGui::GetWindowDrawList()->AddLine({a.x, y}, {b.x, y}, ImGui::GetColorU32(hot ? ImGuiCol_SeparatorActive : ImGuiCol_Separator), 2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to make the table taller or shorter");
}

void draw_coverage(const Snapshot& s) {
    const auto flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX;
    if (ImGui::BeginTable("Channel coverage", 8, flags)) {
        for (const char* h : {"Channel", "MHz", "Decoded", "Captures", "Packets", "Syncs", "Failed checks", "Last decoded"})
            ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (const auto* cov : {&s.ble_coverage, &s.zigbee_coverage})
            for (const auto& c : *cov) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (c.captures) ImGui::TextUnformatted(c.label.c_str());
                else ImGui::TextDisabled("%s", c.label.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.0f", c.frequency_hz / 1e6);
                ImGui::TableNextColumn(); ImGui::Text("%.1f s", c.seconds);
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.captures);
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.packets);
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.syncs);
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.failures);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(age_text(c.last).c_str());
            }
        ImGui::EndTable();
    }
}

void draw_ble(PanelState& st, const Snapshot& s, float height) {
    const auto& rows = st.merge_rotating ? s.ble_grouped : s.ble;
    static const float kHideAfter[] = {0, 120, 600, 1800};
    const float hide_after = kHideAfter[std::clamp(st.hide_after, 0, 3)];
    auto hidden = [&](const BleDevice& d) { return hide_after > 0 && age_s(d.last_seen) > hide_after; };
    size_t named = 0, shown = 0;
    for (const auto& d : rows)
        if (!hidden(d)) { ++shown; named += !d.name.empty(); }
    if (st.merge_rotating)
        ImGui::Text("Bluetooth LE: %zu probable devices from %zu addresses (%zu named)", shown, s.ble.size(), named);
    else
        ImGui::Text("Bluetooth LE advertising: %zu addresses (%zu named)", shown, named);
    if (shown < rows.size()) ImGui::TextDisabled("%zu not heard recently are hidden.", rows.size() - shown);
    ImGui::Checkbox("Merge rotating addresses", &st.merge_rotating);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A new private address is joined to one that just went quiet when the advertising layout,\n"
                          "carrier offset and power match and no other candidate is close. Provisional: without the\n"
                          "device's key no link is certain. A link breaks if both addresses are heard together.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    ImGui::Combo("Hide if not heard for", &st.hide_after, "never\0002 min\00010 min\00030 min\0");
    ImGui::TextDisabled("Named first, then in first-seen order; rows stay put as packets arrive. Names and IDs are unauthenticated.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ble filter", "Search address, name, company, tag", st.ble_filter, sizeof st.ble_filter);
    if (st.ble_table_height <= 0) st.ble_table_height = height;
    const BleDevice* selected = nullptr;
    if (ImGui::BeginTable("BLE devices", 8, kListFlags, ImVec2(0, st.ble_table_height))) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 150);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Company / vendor / tags", ImGuiTableColumnFlags_WidthFixed, 220);
        ImGui::TableSetupColumn("Packets", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Max power dBFS", kLive, 90);
        ImGui::TableSetupColumn("Last seen", kLive, 80);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();
        for (const auto& d : rows) {
            const std::string tags = join(d.tags), who = !d.company.empty() ? d.company : d.vendor;
            const std::string all = join(d.group_addresses, " ");
            if (hidden(d) || !matches(st.ble_filter, {&d.address, &all, &d.name, &d.company, &d.vendor, &tags, &d.address_kind})) continue;
            if (d.key == st.selected_ble) selected = &d;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(d.key.c_str());
            const std::string shown_address =
                d.group_addresses.size() > 1 ? d.address + " (+" + std::to_string(d.group_addresses.size() - 1) + ")" : d.address;
            if (ImGui::Selectable(shown_address.c_str(), d.key == st.selected_ble, ImGuiSelectableFlags_SpanAllColumns)) {
                st.selected_ble = d.key;
                selected = &d;
            }
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.address_kind.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.name.empty() ? "-" : d.name.c_str());
            ImGui::TableNextColumn();
            const std::string label = who + (tags.empty() ? "" : (who.empty() ? "" : " | ") + tags);
            if (!d.remote_id.empty()) ImGui::TextColored(kWarn, "Remote ID | %s", label.c_str());
            else ImGui::TextUnformatted(label.empty() ? "-" : label.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.packets);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(channels(d.channel_counts).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", d.max_power_db);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(age_text(d.last_seen).c_str());
        }
        ImGui::EndTable();
    }
    height_handle("##ble table height", st.ble_table_height);
    if (!selected) {
        for (const auto& d : rows)
            if (d.key == st.selected_ble) selected = &d;
    }
    ImGui::SeparatorText(st.merge_rotating ? "Selected Bluetooth LE device" : "Selected Bluetooth LE address");
    if (!selected) { ImGui::TextDisabled(rows.empty() ? "No advertising decoded yet." : "Select a row to see every decoded field."); return; }
    const auto& d = *selected;
    ImGui::PushID("ble inspector");
    ImGui::TextWrapped("%s | %s%s%s", d.address.c_str(), d.address_kind.c_str(), d.vendor.empty() ? "" : " | OUI vendor: ",
                       d.vendor.c_str());
    if (d.group_addresses.size() > 1) {
        ImGui::TextColored(kWarn, "Probable same device across %zu rotating addresses (provisional link):", d.group_addresses.size());
        ImGui::TextWrapped("%s (current)", join(d.group_addresses, " -> ").c_str());
        if (!d.link_reason.empty()) ImGui::TextDisabled("Latest link: %s", d.link_reason.c_str());
    }
    ImGui::TextWrapped("Roles: %s | Packets: %llu | Channels: %s", join_set(d.roles).c_str(), (unsigned long long)d.packets,
                       channels(d.channel_counts).c_str());
    std::string pdus;
    for (const auto& [k, v] : d.pdu_counts) pdus += (pdus.empty() ? "" : ", ") + k + " x" + std::to_string(v);
    ImGui::TextWrapped("PDU types: %s", pdus.c_str());
    ImGui::TextWrapped("Power last / max: %.1f / %.1f dBFS (relative, uncalibrated) | Carrier offset: %.1f kHz | First seen %s, last %s",
                       d.last_power_db, d.max_power_db, d.last_cfo_hz / 1e3, age_text(d.first_seen).c_str(), age_text(d.last_seen).c_str());
    if (!d.name.empty() || !d.company.empty() || !d.appearance.empty() || d.tx_power_dbm)
        ImGui::TextWrapped("Name: %s | Company: %s | Appearance: %s | TX power: %s", d.name.empty() ? "-" : d.name.c_str(),
                           d.company.empty() ? "-" : d.company.c_str(), d.appearance.empty() ? "-" : d.appearance.c_str(),
                           d.tx_power_dbm ? (std::to_string(*d.tx_power_dbm) + " dBm").c_str() : "-");
    if (!d.services.empty()) ImGui::TextWrapped("Services: %s", join(d.services).c_str());
    if (!d.tags.empty()) ImGui::TextWrapped("Recognised formats: %s", join(d.tags).c_str());
    if (!d.related.empty()) ImGui::TextWrapped("Seen with: %s", join(d.related, "; ").c_str());
    ImGui::TextDisabled("Source: %s", d.last_source.c_str());
    if (!d.remote_id.empty() && ImGui::CollapsingHeader("Bluetooth Remote ID messages (unauthenticated)", ImGuiTreeNodeFlags_DefaultOpen))
        for (const auto& [k, v] : d.remote_id) ImGui::TextWrapped("%s: %s", k.c_str(), v.c_str());
    if (ImGui::CollapsingHeader("Advertising data (latest)", ImGuiTreeNodeFlags_DefaultOpen)) field_table("adv fields", d.adv_fields);
    if (!d.scan_response_fields.empty() && ImGui::CollapsingHeader("Scan response (latest)", ImGuiTreeNodeFlags_DefaultOpen))
        field_table("scan fields", d.scan_response_fields);
    if (ImGui::CollapsingHeader("Raw PDU")) {
        ImGui::TextWrapped("Advertising: %s", d.adv_hex.empty() ? "-" : d.adv_hex.c_str());
        ImGui::TextWrapped("Scan response: %s", d.scan_response_hex.empty() ? "-" : d.scan_response_hex.c_str());
        if (ImGui::SmallButton("Copy advertising PDU hex")) ImGui::SetClipboardText(d.adv_hex.c_str());
    }
    ImGui::PopID();
}

void draw_zigbee(PanelState& st, const Snapshot& s, float height) {
    ImGui::Text("Zigbee / IEEE 802.15.4: %zu addresses", s.zigbee.size());
    ImGui::TextDisabled("802.15.4 has no names: known IEEE vendors first, then first-seen order; rows stay put. "
                        "One node can appear by short and IEEE address. Encrypted payloads are not decrypted.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##zb filter", "Search PAN, address, role, vendor", st.zigbee_filter, sizeof st.zigbee_filter);
    if (st.zigbee_table_height <= 0) st.zigbee_table_height = height;
    const ZigbeeDevice* selected = nullptr;
    if (ImGui::BeginTable("Zigbee devices", 8, kListFlags, ImVec2(0, st.zigbee_table_height))) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 170);
        ImGui::TableSetupColumn("Network", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Role (hint)", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("IEEE / vendor", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableSetupColumn("Security", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Last seen", kLive, 80);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();
        for (const auto& d : s.zigbee) {
            const std::string roles = join_set(d.roles), ieee = d.ext ? ieee802154::eui64_text(*d.ext) : "";
            if (!matches(st.zigbee_filter, {&d.address, &d.network, &roles, &ieee, &d.vendor})) continue;
            if (d.key == st.selected_zigbee) selected = &d;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(d.key.c_str());
            if (ImGui::Selectable(d.address.c_str(), d.key == st.selected_zigbee, ImGuiSelectableFlags_SpanAllColumns)) {
                st.selected_zigbee = d.key;
                selected = &d;
            }
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.network.empty() ? "802.15.4" : d.network.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(roles.empty() ? "-" : roles.c_str());
            ImGui::TableNextColumn();
            const std::string id = ieee + (d.vendor.empty() ? "" : (ieee.empty() ? "" : " | ") + d.vendor);
            ImGui::TextUnformatted(id.empty() ? "-" : id.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.secured_seen ? (d.unsecured_seen ? "mixed" : "encrypted") : d.unsecured_seen ? "none seen" : "-");
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.frames);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(channels(d.channel_counts).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(age_text(d.last_seen).c_str());
        }
        ImGui::EndTable();
    }
    height_handle("##zigbee table height", st.zigbee_table_height);
    if (!selected) {
        for (const auto& d : s.zigbee)
            if (d.key == st.selected_zigbee) selected = &d;
    }
    ImGui::SeparatorText("Selected 802.15.4 address");
    if (!selected) { ImGui::TextDisabled(s.zigbee.empty() ? "No 802.15.4 frames decoded yet." : "Select a row to see every decoded field."); return; }
    const auto& d = *selected;
    ImGui::PushID("zigbee inspector");
    ImGui::TextWrapped("%s | %s", d.address.c_str(), d.network.empty() ? "802.15.4 (network layer not recognised)" : d.network.c_str());
    if (d.ext) ImGui::TextWrapped("IEEE address: %s%s%s", ieee802154::eui64_text(*d.ext).c_str(), d.vendor.empty() ? "" : " | OUI vendor: ",
                                  d.vendor.c_str());
    ImGui::TextWrapped("Roles (hints from frames): %s", d.roles.empty() ? "-" : join_set(d.roles).c_str());
    if (d.ext_pan || d.stack_profile)
        ImGui::TextWrapped("Zigbee beacon: extended PAN %s | stack profile %s | association %s",
                           d.ext_pan ? ieee802154::eui64_text(*d.ext_pan).c_str() : "-",
                           d.stack_profile ? std::to_string(*d.stack_profile).c_str() : "-", d.association_permit ? "permitted" : "not permitted");
    std::string kinds;
    for (const auto& [k, v] : d.frame_counts) kinds += (kinds.empty() ? "" : ", ") + k + " x" + std::to_string(v);
    ImGui::TextWrapped("Frames: %s", kinds.c_str());
    ImGui::TextWrapped("Power last / max: %.1f / %.1f dBFS (relative) | Carrier offset: %.1f kHz | Channels: %s | First seen %s, last %s",
                       d.last_power_db, d.max_power_db, d.last_cfo_hz / 1e3, channels(d.channel_counts).c_str(),
                       age_text(d.first_seen).c_str(), age_text(d.last_seen).c_str());
    ImGui::TextDisabled("Source: %s", d.last_source.c_str());
    if (ImGui::CollapsingHeader("Latest frame (all decoded fields)", ImGuiTreeNodeFlags_DefaultOpen)) field_table("zb fields", d.last_fields);
    if (ImGui::CollapsingHeader("Raw frame")) {
        ImGui::TextWrapped("%s", d.last_hex.empty() ? "-" : d.last_hex.c_str());
        if (ImGui::SmallButton("Copy frame hex")) ImGui::SetClipboardText(d.last_hex.c_str());
    }
    ImGui::PopID();
}

void draw_recent(const char* id, const std::deque<PacketRow>& rows) {
    const auto flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                       ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable(id, 6, flags, ImVec2(0, 220))) {
        ImGui::TableSetupColumn("Seen", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 35);
        ImGui::TableSetupColumn("Protocol", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Sender", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableSetupColumn("dBFS", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Summary");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(age_text(r.at).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", r.channel);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.protocol.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.source.empty() ? "-" : r.source.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.power_db);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.summary.c_str());
        }
        ImGui::EndTable();
    }
}

}  // namespace

void draw_page(PanelState& st, Monitor& monitor, const std::string& band, bool connected) {
    const auto s = monitor.snapshot();
    const bool receiving = band == "wifi_2g4" || band == "wifi_both" || band == "coordinated";
    if (s.enabled) { if (ImGui::Button("Stop decoding")) monitor.enable(false); }
    else if (ImGui::Button("Start Bluetooth & Zigbee decoding")) monitor.enable(true);
    ImGui::SameLine();
    if (ImGui::Button("Clear lists")) { monitor.clear(); st.selected_ble.clear(); st.selected_zigbee.clear(); }
    ImGui::SameLine();
    if (ImGui::Button("Export JSON")) {
        try {
            st.export_message = "Exported " + monitor.export_json(st.export_dir);
        } catch (const std::exception& e) {
            st.export_message = std::string("Export failed: ") + e.what();
        }
    }
    ImGui::SameLine();
    ImGui::TextColored(s.enabled ? kGood : kMuted, "%s", s.status.c_str());
    if (!st.export_message.empty()) ImGui::TextWrapped("%s", st.export_message.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("Receive-only. Decodes BLE advertising (LE 1M, channels 37/38/39) and 802.15.4 / Zigbee (channels 11-26) from "
                       "the 2.4 GHz captures the receiver already takes; tuning is unchanged. Not decoded: Classic Bluetooth, BLE "
                       "connections and LE 2M / Coded secondary advertising, encrypted payloads. Each channel is heard only while a "
                       "capture covers it, so infrequent transmitters can be missed.");
    ImGui::PopStyleColor();
    if (!connected) ImGui::TextColored(kWarn, "Receiver not connected: nothing new is being decoded.");
    if (!receiving)
        ImGui::TextColored(kWarn, "The current mode does not capture 2.4 GHz. Lists below are retained history. "
                                  "Use Wi-Fi 2.4 GHz, Both, or Coordinated in Monitoring setup.");
    else if (band != "coordinated")
        ImGui::TextColored(kMuted, "BLE channel 37 (2402 MHz) and Zigbee 11 are outside this mode's captures; Coordinated mode covers them.");
    ImGui::Text("Captures offered %llu | decoded %llu | dropped while busy %llu | outside 2.4 GHz %llu | unsupported %llu",
                (unsigned long long)s.offered, (unsigned long long)s.decoded_captures, (unsigned long long)s.dropped_busy,
                (unsigned long long)s.outside_band, (unsigned long long)s.unsupported);
    ImGui::Text("Last decode %.2f s for a %.2f s capture (max %.2f s) | BLE packets %llu | 802.15.4 frames %llu", s.last_processing_s,
                s.last_capture_s, s.max_processing_s, (unsigned long long)s.ble_packets, (unsigned long long)s.zigbee_frames);
    if (s.ble_evicted || s.zigbee_evicted)
        ImGui::TextColored(kWarn, "Oldest entries evicted at the list limits: BLE %llu, 802.15.4 %llu",
                           (unsigned long long)s.ble_evicted, (unsigned long long)s.zigbee_evicted);
    if (ImGui::CollapsingHeader("Channel coverage")) draw_coverage(s);

    const float avail = ImGui::GetContentRegionAvail().x;
    const float width = std::max(320.f, (avail - 12) / 2);
    const float table = std::max(180.f, std::max(560.f, ImGui::GetContentRegionAvail().y - 40) * 0.42f);
    if (st.ble_table_height <= 0) st.ble_table_height = table;
    if (st.zigbee_table_height <= 0) st.zigbee_table_height = table;
    // Each side grows with its table so a taller table never squeezes the inspector.
    const float height = std::max({560.f, ImGui::GetContentRegionAvail().y - 40,
                                   std::max(st.ble_table_height, st.zigbee_table_height) + 420});
    ImGui::BeginChild("BLE list", ImVec2(width, height), ImGuiChildFlags_Borders);
    draw_ble(st, s, table);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Zigbee list", ImVec2(width, height), ImGuiChildFlags_Borders);
    draw_zigbee(st, s, table);
    ImGui::EndChild();
    if (ImGui::CollapsingHeader("Recent packets")) {
        ImGui::BeginChild("Recent BLE", ImVec2(width, 260), ImGuiChildFlags_Borders);
        ImGui::Text("Bluetooth LE (latest %zu)", s.recent_ble.size());
        draw_recent("recent ble", s.recent_ble);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("Recent Zigbee", ImVec2(width, 260), ImGuiChildFlags_Borders);
        ImGui::Text("802.15.4 / Zigbee (latest %zu)", s.recent_zigbee.size());
        draw_recent("recent zigbee", s.recent_zigbee);
        ImGui::EndChild();
    }
}

void draw_remote_id_section(const Snapshot& s) {
    std::vector<const BleDevice*> rid;
    for (const auto& d : s.ble)
        if (!d.remote_id.empty()) rid.push_back(&d);
    ImGui::TextWrapped("Bluetooth Remote ID from the Bluetooth & Zigbee decoder (%s). ASTM F3411 messages in BLE legacy advertising on "
                       "channels 37/38/39; LE Coded / extended advertising is not received. Unauthenticated broadcast claims.",
                       s.enabled ? "decoding" : "stopped");
    if (rid.empty()) { ImGui::TextDisabled("No Bluetooth Remote ID received."); return; }
    for (const auto* d : rid) {
        ImGui::PushID(d->key.c_str());
        const std::string label = d->address + (d->name.empty() ? "" : " | " + d->name) + " | " + std::to_string(d->remote_id.size()) +
                                  " message types | " + age_text(d->last_seen);
        if (ImGui::TreeNode(label.c_str())) {
            for (const auto& [k, v] : d->remote_id) ImGui::TextWrapped("%s: %s", k.c_str(), v.c_str());
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

}  // namespace rfmon::shortrange
