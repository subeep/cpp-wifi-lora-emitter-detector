#include "wifi_gui.hpp"
#include "imgui.h"
#include <algorithm>
#include <ctime>
#include <map>
#include <sstream>
#include <iomanip>

namespace rfmon {
void draw_wifi_packet_table(const std::vector<WifiPacketRow>& packets, float height) {
    if (packets.empty()) {
        ImGui::TextDisabled("No current Wi-Fi packet rows; any paused or selected snapshot is retained below.");
    }

    struct View { ImGuiTextFilter filter; bool valid_only=false, follow=true; std::vector<WifiPacketRow> paused; std::optional<WifiPacketRow> selected; };
    static std::map<ImGuiID, View> views;
    auto& view=views[ImGui::GetID("packet view")];
    if(ImGui::IsKeyChordPressed(ImGuiMod_Ctrl|ImGuiKey_F))ImGui::SetKeyboardFocusHere();
    view.filter.Draw("Search retained rows",std::min(300.f,ImGui::GetContentRegionAvail().x));
    ImGui::Checkbox("FCS valid only",&view.valid_only);ImGui::SameLine();
    if(ImGui::Checkbox("Follow live",&view.follow)) {if(!view.follow)view.paused=packets;else view.paused.clear();}
    ImGui::SameLine();if(ImGui::SmallButton("Clear filters")){view.filter.Clear();view.valid_only=false;}
    if(!view.follow){ImGui::TextDisabled("Paused display snapshot; acquisition continues.");}
    ImGui::TextDisabled("Right-click table headers for all columns. Selected details are a frozen row snapshot, not an exact security-frame join.");
    if(view.selected && ImGui::IsKeyPressed(ImGuiKey_Escape))view.selected.reset();
    const auto& displayed=view.follow?packets:view.paused;
    static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_ScrollX;
    ImVec2 outer_size(0.0f, view.selected?std::min(height,280.f):height);
    if (!ImGui::BeginTable("wifi_packets_identity_v4", 23, flags, outer_size)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Freq (MHz)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 100.0f);
    ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("Identity key", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 155.0f);
    ImGui::TableSetupColumn("Network / SSID", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableSetupColumn("AP ch", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 55.0f);
    ImGui::TableSetupColumn("Advertised security", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 230.0f);
    ImGui::TableSetupColumn("Decode status", ImGuiTableColumnFlags_WidthFixed, 270.0f);
    // MAC type/subtype of any FCS-valid MPDU (both receive chains);
    // "short" marks a sub-beacon-length DSSS burst decoded for the
    // security monitor only (never used for identity/fingerprints).
    ImGui::TableSetupColumn("Frame type", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn("OFDM Mbps", ImGuiTableColumnFlags_WidthFixed, 95.0f);
    ImGui::TableSetupColumn("PSDU bytes", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 95.0f);
    ImGui::TableSetupColumn("Modulation", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Power (dB)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90.0f);
    ImGui::TableSetupColumn("BW (MHz)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90.0f);
    ImGui::TableSetupColumn("Duration (us)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 110.0f);
    ImGui::TableSetupColumn("Classifier score", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90.0f);
    // RF fingerprint (see wifi_fingerprint.hpp) - only populated once a
    // burst clears the bandwidth gate and (for DSSS) is beacon-shaped;
    // "--" means not attempted, the gate reason means attempted but
    // rejected. IRR/IQ eps/IQ phi are additionally always blank on a
    // DSSS row - see WifiPacketRow's own comment on why that's
    // structural, not a gate outcome.
    ImGui::TableSetupColumn("CFO (ppm)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90.0f);
    ImGui::TableSetupColumn("IRR (dB)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
    ImGui::TableSetupColumn("IQ eps", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
    ImGui::TableSetupColumn("IQ phi (deg)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 100.0f);
    ImGui::TableSetupColumn("DC (dBc)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
    ImGui::TableSetupColumn("EVM (%)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
    ImGui::TableSetupColumn("Sync corr", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90.0f);
    ImGui::TableHeadersRow();

    const ImVec4 dsss_color(0.36f, 0.68f, 0.93f, 1.0f);
    const ImVec4 ofdm_color(0.25f, 0.73f, 0.31f, 1.0f);
    const ImVec4 dim(0.55f, 0.55f, 0.58f, 1.0f);
    const ImVec4 bad(0.85f, 0.45f, 0.40f, 1.0f);

    auto opt_cell = [&](const std::optional<double>& v, const char* fmt) {
        ImGui::TableNextColumn();
        if (v.has_value()) ImGui::Text(fmt, *v);
        else ImGui::TextColored(dim, "--");
    };

    // Filter the bounded tail once, then virtualise fixed-height visible rows.
    std::vector<size_t> visible;
    for(size_t i=displayed.size();i-- >0;) {
        const auto& p=displayed[i];
        const std::string searchable=p.time+" "+p.modulation+" "+p.frame_type+" "+p.master_key+" "+p.decode_status+" "+
            (p.identity?p.identity->ssid+" "+p.identity->security:std::string())+" "+std::to_string(p.channel);
        if(view.filter.PassFilter(searchable.c_str()) && (!view.valid_only||p.fcs_valid))visible.push_back(i);
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()),ImGui::GetTextLineHeight()+2*ImGui::GetStyle().CellPadding.y);
    while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row) {
        const size_t i=visible[row];const auto& p=displayed[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(i));
        if(ImGui::Selectable(p.time.c_str(),false))view.selected=p;
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", p.freq_mhz);
        ImGui::TableNextColumn();
        ImGui::Text("%d", p.channel);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.master_key.empty() ? "--" : p.master_key.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.identity ? (p.identity->ssid.empty() ? "Hidden / absent" : wifi::display_text(p.identity->ssid).c_str()) : "Not decoded");
        ImGui::TableNextColumn();
        if (p.identity && p.identity->channel) ImGui::Text("%d", p.identity->channel);
        else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.identity ? p.identity->security.c_str() : "Unknown");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.decode_status.empty() ? "Not attempted" : p.decode_status.c_str());
        ImGui::TableNextColumn();
        if (p.frame_type.empty()) ImGui::TextColored(dim, "--");
        else if (p.security_decode_only) {
            ImGui::Text("%s (short)", p.frame_type.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Short DSSS burst decoded for the security monitor only: not used for "
                                  "identity, fingerprints or beacon-cadence source counts.");
        } else ImGui::TextUnformatted(p.frame_type.c_str());
        ImGui::TableNextColumn();
        if (p.ofdm_rate_mbps) ImGui::Text("%d", p.ofdm_rate_mbps); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (p.psdu_length) ImGui::Text("%zu", p.psdu_length); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        ImGui::TextColored(p.modulation == "DSSS" ? dsss_color : ofdm_color, "%s",
                            p.modulation.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", p.power_db);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", p.bandwidth_khz / 1e3);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", p.duration_us);
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", p.confidence);

        opt_cell(p.fp_cfo_ppm, "%.2f");
        // IRR/IQ eps/IQ phi: structurally unset on DSSS, not a gate
        // outcome - shown as "n/a" rather than "--" to distinguish
        // "can never be measured for this modulation" from "not
        // attempted/gated out".
        if (p.modulation == "DSSS") {
            ImGui::TableNextColumn();
            ImGui::TextColored(dim, "n/a");
            ImGui::TableNextColumn();
            ImGui::TextColored(dim, "n/a");
            ImGui::TableNextColumn();
            ImGui::TextColored(dim, "n/a");
        } else {
            opt_cell(p.fp_irr_db, "%.2f");
            opt_cell(p.fp_iq_eps, "%.4f");
            opt_cell(p.fp_iq_phi_deg, "%.2f");
        }
        opt_cell(p.fp_dc_dbc, "%.2f");
        opt_cell(p.fp_evm_pct, "%.1f");
        ImGui::TableNextColumn();
        if (p.fp_gate_reason.has_value()) {
            ImGui::TextColored(bad, "%s", p.fp_gate_reason->c_str());
        } else if (p.fp_sync_corr.has_value()) {
            ImGui::Text("%.3f", *p.fp_sync_corr);
        } else {
            ImGui::TextColored(dim, "--");
        }
    }
    ImGui::EndTable();
    if(view.selected) {
        const auto& p=*view.selected;
        ImGui::SeparatorText("Selected packet snapshot");
        ImGui::TextWrapped("%s | Channel %d | %.9g MHz | %s | FCS: %s",p.time.c_str(),p.channel,p.freq_mhz,p.modulation.c_str(),p.fcs_valid?"passed":"not passed / unavailable");
        ImGui::TextWrapped("Decode: %s | Frame: %s | Identity key: %s",p.decode_status.c_str(),p.frame_type.c_str(),p.master_key.c_str());
        if(p.fp_gate_reason)ImGui::TextWrapped("Fingerprint rejected: %s",p.fp_gate_reason->c_str());
        if(ImGui::Button("Close packet details"))view.selected.reset();
        // Do not access p after resetting its owning optional.
        else if(ImGui::CollapsingHeader("Complete packet measurements",ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Relative power %.17g dB | Bandwidth %.17g kHz | Duration %.17g us",p.power_db,p.bandwidth_khz,p.duration_us);
            ImGui::Text("Classifier score (not calibrated probability): %.17g",p.confidence);
            if(p.ofdm_rate_mbps)ImGui::Text("OFDM rate: %d Mbps",p.ofdm_rate_mbps);else ImGui::TextDisabled("OFDM rate: unavailable / not applicable");
            if(p.psdu_length)ImGui::Text("PSDU: %zu bytes",p.psdu_length);else ImGui::TextDisabled("PSDU length: unavailable");
            auto value=[](const char* label,const std::optional<double>& v){if(v)ImGui::Text("%s: %.17g",label,*v);else ImGui::TextDisabled("%s: unavailable",label);};
            value("CFO ppm",p.fp_cfo_ppm);value("DC dBc",p.fp_dc_dbc);value("DC angle degrees",p.fp_dc_ang_deg);value("SNR dB",p.fp_snr_db);value("EVM percent",p.fp_evm_pct);value("Sync correlation",p.fp_sync_corr);
            if(p.modulation=="DSSS")ImGui::TextDisabled("IRR / IQ imbalance: n/a for DSSS");
            else {value("IRR dB",p.fp_irr_db);value("IQ epsilon",p.fp_iq_eps);value("IQ phase degrees",p.fp_iq_phi_deg);}
            ImGui::Text("Security-only decode: %s",p.security_decode_only?"yes":"no");
            if(p.identity) {
                ImGui::TextWrapped("SSID: %s | Claimed BSSID: %s | Advertised security: %s",wifi::display_text(p.identity->ssid).c_str(),p.identity->bssid.c_str(),p.identity->security.c_str());
                if(p.identity->channel)ImGui::Text("Advertised AP channel: %d",p.identity->channel);else ImGui::TextDisabled("Advertised AP channel: unavailable");
            }
            ImGui::TextDisabled("Full raw MPDU and exact capture key are not retained in this row. Inspect Security for its own retained frame evidence.");
        }
    }

}

// Persistent identities and provisional clusters. Select a row for provenance,
// advertised capabilities and the latest accepted RF reading.
void draw_wifi_master_table(const std::vector<wifi_master::WifiMasterRow>& rows, float height, bool inline_inspector) {
    if (rows.empty()) { ImGui::TextDisabled("No Wi-Fi identities or fingerprints recorded yet."); return; }
    static std::string selected;
    const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Resizable |
                       ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    bool open_details = false;
    auto text = [](const std::string& value, const char* empty = "Not advertised") {
        if (value.empty()) ImGui::TextDisabled("%s", empty);
        else ImGui::TextUnformatted(wifi::display_text(value).c_str());
    };
    auto date = [](int64_t ts) {
        std::time_t time = std::time_t(ts); std::tm tm{}; localtime_r(&time, &tm);
        char buf[32]; std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
        return std::string(buf);
    };
    const bool wide_inspector=inline_inspector && !selected.empty() && ImGui::GetContentRegionAvail().x>=1100;
    if(wide_inspector)ImGui::BeginChild("Identity list",ImVec2(ImGui::GetContentRegionAvail().x-450,height),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::BeginTable("wifi_master_identity_v2", 13, flags, ImVec2(0, wide_inspector?std::max(120.f,height-20):inline_inspector && !selected.empty()?std::min(height,260.f):height))) {
        ImGui::TableSetupScrollFreeze(2, 1);
        const char* columns[] = {"Identity (click)", "Network / SSID", "Vendor assignment", "Advertised security",
            "AP channel", "WPS model", "PHY capabilities", "First seen", "Last seen", "Identity observations",
            "RF readings retained", "Monitored MHz", "Identity source"};
        const float widths[] = {155,180,190,245,85,150,180,155,155,140,135,110,140};
        for (int i=0; i<13; ++i) ImGui::TableSetupColumn(columns[i], ImGuiTableColumnFlags_WidthFixed | ((i>=5)?ImGuiTableColumnFlags_DefaultHide:0), widths[i]);
        ImGui::TableHeadersRow();
        for (const auto& row : rows) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            // Green for a decoded network identity (MAC-keyed - see
            // key_is_mac), white for a provisional RF-fingerprint
            // cluster with no decoded SSID/name yet.
            ImGui::PushStyleColor(ImGuiCol_Text, row.key_is_mac ? ImVec4(0.25f,0.73f,0.31f,1) : ImVec4(0.92f,0.92f,0.94f,1));
            if (ImGui::Selectable(row.device_key.c_str(), selected == row.device_key)) {
                selected = row.device_key; open_details = true;
            }
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (row.identity) text(row.identity->ssid, row.identity->ssid_present ? "Hidden SSID" : "SSID not advertised");
            else ImGui::TextDisabled("Not decoded");
            ImGui::TableNextColumn(); text(row.vendor, "Unknown (cluster)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.vendor_source.c_str());
            ImGui::TableNextColumn(); text(row.identity ? row.identity->security : "", "Unknown");
            ImGui::TableNextColumn();
            if (row.identity && row.identity->channel) ImGui::Text("%d", row.identity->channel);
            else ImGui::TextDisabled("Unknown");
            ImGui::TableNextColumn(); text(row.identity ? row.identity->wps_model_name : "");
            ImGui::TableNextColumn(); text(row.identity ? row.identity->standards : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(date(row.first_seen_ts).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(date(row.last_seen_ts).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(row.identity_count));
            ImGui::TableNextColumn(); ImGui::Text("%d", row.reading_count);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", row.last_channel_hz/1e6);
            ImGui::TableNextColumn(); text(row.identity ? row.identity->frame_source : "", row.key_is_mac ? "Legacy MAC record" : "RF cluster (unverified)");
        }
        ImGui::EndTable();
    }
    if(wide_inspector){ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("Identity inspector",ImVec2(0,height),ImGuiChildFlags_Borders);}
    if (open_details && !inline_inspector) ImGui::OpenPopup("Wi-Fi identity details");
    if(!inline_inspector)ImGui::SetNextWindowSize(ImVec2(720, 620), ImGuiCond_Appearing);
    if (inline_inspector ? !selected.empty() : ImGui::BeginPopup("Wi-Fi identity details")) {
        if(inline_inspector)ImGui::SeparatorText("Selected identity inspector");
        auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.device_key == selected; });
        if (it != rows.end()) {
            const auto& row = *it;
            ImGui::PushTextWrapPos(0);
            ImGui::Text("%s", row.device_key.c_str());
            ImGui::TextDisabled("%s", row.key_is_mac ? "Decoded network identity; multiple BSSIDs can share one radio." : "Provisional RF cluster; not a verified physical device.");
            ImGui::Separator();
            ImGui::Text("Vendor assignment: %s", row.vendor.empty() ? "Unknown" : wifi::display_text(row.vendor).c_str());
            ImGui::TextWrapped("Lookup source: %s", row.vendor_source.empty() ? "No decoded MAC" : row.vendor_source.c_str());
            if (row.identity) {
                const auto& b = *row.identity;
                ImGui::Text("SSID: %s", b.ssid.empty() ? (b.ssid_present ? "Hidden" : "Not advertised") : wifi::display_text(b.ssid).c_str());
                if (!b.ssid.empty()) ImGui::TextDisabled("Name last observed: %s (%s)", date(row.ssid_seen_ts).c_str(), row.ssid_source.c_str());
                ImGui::Text("Source: FCS-valid %s (%s), %s", b.frame_source.c_str(), row.identity_phy.c_str(), date(row.identity_ts).c_str());
                ImGui::Text("Monitored channel center: %.3f MHz", row.monitored_channel_hz / 1e6);
                if (b.channel) ImGui::Text("Advertised channel: %d (%s)", b.channel, b.channel_source.c_str());
                else ImGui::TextDisabled("Advertised channel: unknown");
                if (!b.ies_complete) ImGui::TextColored(ImVec4(1,.65f,.2f,1), "Some information elements are malformed or incomplete.");
                if(ImGui::CollapsingHeader("Advertised security and PHY capabilities",ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Beacon interval: %u TU (%.3f ms)", unsigned(b.beacon_interval_tu), b.beacon_interval_tu * 1.024);
                ImGui::Text("Capability: 0x%04x | ESS: %s | IBSS: %s | Privacy: %s", unsigned(b.capability),
                            b.capability & 1 ? "yes" : "no", b.capability & 2 ? "yes" : "no", b.capability & 0x10 ? "set" : "clear");
                ImGui::TextWrapped("Advertised security: %s", b.security.c_str());
                ImGui::TextWrapped("Cipher suites: %s", b.ciphers.empty() ? "Not advertised" : b.ciphers.c_str());
                ImGui::Text("Protected management frames: %s", b.pmf.empty() ? "Not advertised" : b.pmf.c_str());
                ImGui::TextWrapped("PHY capability IEs: %s", b.standards.empty() ? "Not advertised" : b.standards.c_str());
                }
                ImGui::Separator();
                if(ImGui::CollapsingHeader("WPS device-advertised hints")) {
                ImGui::Text("WPS IE in latest frame: %s", b.wps_present ? "Present" : "Not advertised");
                if (row.wps_seen_ts) ImGui::TextDisabled("WPS hints last observed: %s (%s)", date(row.wps_seen_ts).c_str(), row.wps_source.c_str());
                auto hint = [&](const char* label, const std::string& s) {
                    ImGui::TextWrapped("%s: %s", label, s.empty() ? "Not advertised" : wifi::display_text(s).c_str());
                };
                hint("WPS device name", b.wps_device_name); hint("WPS manufacturer", b.wps_manufacturer);
                hint("WPS model", b.wps_model_name); hint("WPS model number", b.wps_model_number);
                ImGui::TextDisabled("WPS strings are device-advertised hints, not verified model identification.");
                }
            } else ImGui::TextWrapped("No beacon metadata saved. Legacy MAC records acquire details on the next successful decode.");
            ImGui::Separator();
            ImGui::Text("First seen: %s | Last seen: %s", date(row.first_seen_ts).c_str(), date(row.last_seen_ts).c_str());
            ImGui::Text("Identity observations: %llu | Retained RF readings: %d", static_cast<unsigned long long>(row.identity_count), row.reading_count);
            if (!row.reading_count) ImGui::TextDisabled("No accepted RF fingerprint. The decoded identity is still saved.");
            else if(ImGui::CollapsingHeader("Latest accepted RF fingerprint")) {
                const auto& f = row.latest;
                ImGui::Text("Latest accepted RF reading: %s (%s)", date(f.ts).c_str(), f.phy.c_str());
                ImGui::Text("CFO: %.2f ppm | DC: %.2f dBc | DC angle: %.2f deg", f.cfo_ppm, f.dc_dbc, f.dc_ang_deg);
                if (f.phy == "DSSS") ImGui::TextDisabled("IRR / IQ imbalance: n/a for DSSS");
                else ImGui::Text("IRR: %.2f dB | IQ eps: %.4f | IQ phi: %.2f deg", f.irr_db, f.iq_eps, f.iq_phi_deg);
                ImGui::Text("SNR: %.1f dB | EVM: %.1f%% | Sync correlation: %.3f", f.snr_db, f.evm_pct, f.sync_corr);
            }
            ImGui::PopTextWrapPos();
        }
        else ImGui::TextDisabled("Selected identity no longer retained.");
        if (ImGui::Button("Close")) {if(inline_inspector)selected.clear();else ImGui::CloseCurrentPopup();}
        if(!inline_inspector)ImGui::EndPopup();
    }
    if(wide_inspector)ImGui::EndChild();
}


} // namespace rfmon
