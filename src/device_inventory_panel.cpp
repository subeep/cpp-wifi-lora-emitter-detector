#include "device_inventory_panel.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <numeric>

#include "gui/app_shell.hpp"
#include "imgui.h"

namespace rfmon::devices {
namespace {

std::string ago(int64_t t, int64_t now) {
    if (t <= 0) return "-";
    const int64_t d = std::max<int64_t>(0, now - t);
    char b[32];
    if (d < 60) std::snprintf(b, sizeof b, "%llds ago", static_cast<long long>(d));
    else if (d < 3600) std::snprintf(b, sizeof b, "%lldm ago", static_cast<long long>(d / 60));
    else if (d < 86400) std::snprintf(b, sizeof b, "%lldh ago", static_cast<long long>(d / 3600));
    else {
        const std::time_t tt = std::time_t(t);
        std::tm tm{};
        localtime_r(&tt, &tm);
        std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
    }
    return b;
}
std::string clock_time(int64_t t) {
    if (t <= 0) return "not reported";
    const std::time_t tt = std::time_t(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

struct Recency { const char* label; int64_t seconds; };
constexpr Recency kRecency[] = {{"Any time", 0}, {"Last minute", 60}, {"Last 10 minutes", 600},
                                {"Last hour", 3600}, {"Last 24 hours", 86400}};

enum Column { ColTechnology, ColIdentifier, ColName, ColKind, ColRole, ColVendor, ColChannel, ColSeen, ColPower, ColLast, ColFirst, ColCount };

// Sort key per column; ties fall back to the identifier so rows never jump.
bool less(const Device& a, const Device& b, int column) {
    switch (column) {
        case ColTechnology: if (a.technology != b.technology) return a.technology < b.technology; break;
        case ColName: if (a.name != b.name) return a.name < b.name; break;
        case ColKind: if (a.identifier_kind != b.identifier_kind) return a.identifier_kind < b.identifier_kind; break;
        case ColRole: if (a.role != b.role) return a.role < b.role; break;
        case ColVendor: if (a.vendor != b.vendor) return a.vendor < b.vendor; break;
        case ColChannel: if (a.channel != b.channel) return a.channel < b.channel; break;
        case ColSeen: if (a.observations != b.observations) return a.observations < b.observations; break;
        case ColPower: if (a.power_db.value_or(-1e9) != b.power_db.value_or(-1e9)) return a.power_db.value_or(-1e9) < b.power_db.value_or(-1e9); break;
        case ColLast: if (a.last_seen != b.last_seen) return a.last_seen < b.last_seen; break;
        case ColFirst: if (a.first_seen != b.first_seen) return a.first_seen < b.first_seen; break;
        default: break;
    }
    return a.key < b.key;
}

void draw_filters(PanelState& st, const std::vector<Device>& devices) {
    std::array<size_t, kTechnologies> per{};
    for (const auto& d : devices) ++per[size_t(d.technology)];
    for (size_t t = 0; t < kTechnologies; ++t) {
        if (t) ImGui::SameLine();
        char label[64];
        std::snprintf(label, sizeof label, "%s (%zu)", technology_name(Technology(t)), per[t]);
        ImGui::Checkbox(label, &st.filter.show[t]);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("All")) st.filter.show.fill(true);
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) st.filter.show.fill(false);
    ImGui::SetNextItemWidth(std::max(200.f, ImGui::GetContentRegionAvail().x - 420 * ImGui::GetStyle().FontScaleMain));
    ImGui::InputTextWithHint("##device search", "Search identifier, name, vendor, role, channel, details", st.search, sizeof st.search);
    st.filter.search = st.search;
    ImGui::SameLine();
    ImGui::Checkbox("Named only", &st.filter.named_only);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170 * ImGui::GetStyle().FontScaleMain);
    if (ImGui::BeginCombo("Seen", kRecency[st.recency].label)) {
        for (int i = 0; i < int(std::size(kRecency)); ++i)
            if (ImGui::Selectable(kRecency[i].label, st.recency == i)) st.recency = i;
        ImGui::EndCombo();
    }
    st.filter.seen_within_s = kRecency[st.recency].seconds;
}

void draw_inspector(const PanelState& st, const std::vector<Device>& devices, int64_t now) {
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const Device& d) { return d.key == st.selected; });
    if (it == devices.end()) return;
    const Device& d = *it;
    ImGui::SeparatorText("Selected device");
    ImGui::Text("%s  %s%s%s", technology_name(d.technology), d.identifier.c_str(), d.name.empty() ? "" : "  ", d.name.c_str());
    if (ImGui::BeginTable("Device details", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable)) {
        auto row = [](const std::string& k, const std::string& v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k.c_str());
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", v.empty() ? "-" : v.c_str());
        };
        row("Identifier kind", d.identifier_kind);
        row("Role", d.role);
        row("Vendor", d.vendor);
        row("Channel", d.channel);
        row("Seen", std::to_string(d.observations) + " " + d.observations_unit);
        if (d.power_db) { char b[32]; std::snprintf(b, sizeof b, "%.1f dB (relative)", *d.power_db); row("Last power", b); }
        row("First seen", clock_time(d.first_seen));
        row("Last seen", clock_time(d.last_seen) + " (" + ago(d.last_seen, now) + ")");
        for (const auto& [k, v] : d.details) row(k, v);
        ImGui::EndTable();
    }
}

}  // namespace

void draw_panel(PanelState& st, const std::vector<Device>& devices, int64_t now) {
    draw_filters(st, devices);
    std::vector<size_t> rows;
    for (size_t i = 0; i < devices.size(); ++i)
        if (matches(devices[i], st.filter, now)) rows.push_back(i);
    ImGui::TextDisabled("%zu of %zu identifiers shown. Rows are identifiers from integrity-checked frames, not a count of physical devices: "
                        "one phone can appear as a Wi-Fi and a Bluetooth identifier.", rows.size(), devices.size());
    if (devices.empty()) {
        gui::note("No identifiers yet. Wi-Fi fills from decoded frames while scanning; Bluetooth and Zigbee decode the 2.4 GHz "
                  "captures; LoRaWAN needs CRC-valid frames on the Sub-GHz band.");
        return;
    }
    if (st.height <= 0) st.height = 320 * ImGui::GetStyle().FontScaleMain;
    // Resizable, reorderable, hideable (right-click a header) and sortable
    // columns; the handle below sets the height.
    constexpr auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                           ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("Combined devices", ColCount, flags, ImVec2(0, st.height))) {
        ImGui::TableSetupColumn("Technology", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Identifier", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 190);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableSetupColumn("Identifier kind", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 170);
        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Seen", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Power", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 70);
        ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                ImGuiTableColumnFlags_PreferSortDescending, 90);
        ImGui::TableSetupColumn("First seen", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90);
        ImGui::TableSetupScrollFreeze(2, 1);
        ImGui::TableHeadersRow();
        if (const ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsCount > 0) {
            const auto& spec = sort->Specs[0];
            std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
                return spec.SortDirection == ImGuiSortDirection_Descending ? less(devices[b], devices[a], spec.ColumnIndex)
                                                                           : less(devices[a], devices[b], spec.ColumnIndex);
            });
        }
        ImGuiListClipper clipper;
        clipper.Begin(int(rows.size()));
        while (clipper.Step())
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const Device& d = devices[rows[size_t(r)]];
                ImGui::PushID(d.key.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(technology_name(d.technology), st.selected == d.key, ImGuiSelectableFlags_SpanAllColumns))
                    st.selected = st.selected == d.key ? std::string() : d.key;
                ImGui::TableNextColumn(); ImGui::TextUnformatted(d.identifier.c_str());
                ImGui::TableNextColumn();
                if (d.name.empty()) ImGui::TextDisabled("-"); else ImGui::TextUnformatted(d.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(d.identifier_kind.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(d.role.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(d.vendor.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(d.channel.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(d.observations));
                ImGui::TableNextColumn();
                if (d.power_db) ImGui::Text("%.1f", *d.power_db); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(d.last_seen, now).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(d.first_seen, now).c_str());
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    gui::height_handle("##devices height", st.height);
    ImGui::TextDisabled("Click a row to inspect it. Click a header to sort; right-click a header to show or hide columns.");
    draw_inspector(st, devices, now);
}

}  // namespace rfmon::devices
