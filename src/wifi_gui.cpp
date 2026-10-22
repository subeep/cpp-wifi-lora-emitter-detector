#include "wifi_gui.hpp"
#include "imgui.h"
#include <algorithm>
#include <ctime>
#include <map>
#include <sstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <cstring>

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
void draw_wifi_master_table(const std::vector<wifi_master::WifiMasterRow>& rows, float height, bool inline_inspector, const std::function<bool(wifi_master::EnrolmentCommand)>& command, int band) {
    if (rows.empty()) { ImGui::TextDisabled("No Wi-Fi identities or fingerprints recorded yet."); return; }
    static std::string selected;
    struct MasterView { ImGuiTextFilter search; int filter=0; bool all_bands=false, clusters=false, candidates=false; std::string message, edit_key, pending_key, pending_action; char alias[129]={}, note[1025]={}; };
    static std::map<ImGuiID,MasterView> views;auto& v=views[ImGui::GetID("enrolment view")];
    v.search.Draw("Search master list",240);ImGui::SameLine();
    ImGui::Combo("Review filter",&v.filter,"All\0Needs attention\0Approved\0Unapproved\0Disallowed\0");
    if(band){ImGui::Checkbox("All Wi-Fi bands",&v.all_bands);ImGui::SameLine();}ImGui::Checkbox("Include anonymous RF clusters",&v.clusters);
    ImGui::Checkbox("Include unconfirmed candidates",&v.candidates);
    ImGui::TextWrapped("Default: repeated observations in 3 usable captures spanning 10 seconds within one receiver context. Historical inventory, not devices currently online.");
    ImGui::TextDisabled("Claimed interfaces, not unique physical devices. Red means unreviewed/unapproved, not proven malicious.");
    size_t fresh=0,unapproved=0,approved=0,changed=0,confirmed=0,candidates=0;
    for(const auto& r:rows)if(r.enrolment){const auto& e=*r.enrolment;
        if(!wifi_master::has_repeated_identity_evidence(e)){++candidates;continue;}++confirmed;
        fresh+=e.policy=="unreviewed"&&!e.imported;unapproved+=e.policy!="approved";approved+=e.policy=="approved";
        changed+=e.digital_review||wifi_master::has_rf_review(e);}
    ImGui::Text("All Wi-Fi: %zu repeatedly observed interfaces | %zu unconfirmed candidates",confirmed,candidates);
    ImGui::Text("Repeated observations: %zu new | %zu unapproved | %zu approved | %zu changes to review",fresh,unapproved,approved,changed);
    if(!v.message.empty())ImGui::TextWrapped("%s",v.message.c_str());
    auto submit=[&](const std::string& action,const std::string& key){
        if(command){v.message=command({key,action,v.alias,v.note})?"Command queued; saved status appears above when committed.":"Command queue full; action was NOT accepted.";}
    };
    std::vector<const wifi_master::WifiMasterRow*> visible;
    auto attention=[](const auto& r){return r.enrolment && (r.enrolment->policy!="approved" || r.enrolment->digital_review || wifi_master::has_rf_review(*r.enrolment));};
    for(const auto& r:rows){
        if(!r.key_is_mac&&!v.clusters)continue;
        if(r.key_is_mac&&!v.candidates&&(!r.enrolment||!wifi_master::has_repeated_identity_evidence(*r.enrolment)))continue;
        if(band&&!v.all_bands){bool match=r.enrolment?(band==24?r.enrolment->seen_24>0:r.enrolment->seen_5>0):(band==24?r.last_channel_hz>=2400e6&&r.last_channel_hz<2500e6:r.last_channel_hz>=5000e6&&r.last_channel_hz<6000e6);if(!match)continue;}
        const std::string policy=r.enrolment?r.enrolment->policy:"unreviewed";
        if((v.filter==1&&!attention(r))||(v.filter==2&&policy!="approved")||(v.filter==3&&policy=="approved")||(v.filter==4&&policy!="disallowed"))continue;
        std::string haystack=r.device_key+" "+r.vendor+" "+(r.identity?wifi::display_text(r.identity->ssid):"")+" "+(r.enrolment?r.enrolment->alias:"");
        if(v.search.PassFilter(haystack.c_str()))visible.push_back(&r);
    }
    std::stable_sort(visible.begin(),visible.end(),[&](auto a,auto b){return attention(*a)>attention(*b);});
    height=std::max(160.f,height-115.f);
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
    if (ImGui::BeginTable("wifi_master_identity_v2", 14, flags, ImVec2(0, wide_inspector?std::max(120.f,height-20):inline_inspector && !selected.empty()?std::min(height,260.f):height))) {
        ImGui::TableSetupScrollFreeze(2, 1);
        const char* columns[] = {"Identity (click)", "Review status", "Network / SSID", "Vendor assignment", "Advertised security",
            "AP channel", "WPS model", "PHY capabilities", "First seen", "Last seen", "Identity observations",
            "RF readings retained", "Monitored MHz", "Identity source"};
        const float widths[] = {155,160,180,190,245,85,150,180,155,155,140,135,110,140};
        for (int i=0; i<14; ++i) ImGui::TableSetupColumn(columns[i], ImGuiTableColumnFlags_WidthFixed | ((i>=5)?ImGuiTableColumnFlags_DefaultHide:0), widths[i]);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;clipper.Begin(int(visible.size()));
        while(clipper.Step())for(int ri=clipper.DisplayStart;ri<clipper.DisplayEnd;++ri){const auto& row=*visible[ri];
            ImGui::TableNextRow(0,ImGui::GetTextLineHeightWithSpacing()*3); ImGui::TableNextColumn();
            // Red is an unreviewed/unapproved identity, never a claim of malice.
            const bool candidate=row.key_is_mac && (!row.enrolment || !wifi_master::has_repeated_identity_evidence(*row.enrolment));
            ImGui::PushStyleColor(ImGuiCol_Text, candidate ? ImVec4(1.f,.72f,.3f,1) : row.key_is_mac && (!row.enrolment || row.enrolment->policy!="approved") ? ImVec4(1.f,.35f,.35f,1) : ImVec4(.85f,.90f,.94f,1));
            if (ImGui::Selectable(row.device_key.c_str(), selected == row.device_key)) {
                selected = row.device_key; open_details = true;
            }
            ImGui::PopStyleColor();
            if(row.enrolment){
                if(row.enrolment->seen_24)ImGui::TextColored(ImVec4(.3f,.8f,1,1),"2.4 GHz");
                if(row.enrolment->seen_5){if(row.enrolment->seen_24)ImGui::SameLine();ImGui::TextColored(ImVec4(.8f,.6f,1,1),"5 GHz");}
            }
            ImGui::TextDisabled("Latest %.0f MHz",row.last_channel_hz/1e6);
            ImGui::TableNextColumn();
            if(row.enrolment){const auto& e=*row.enrolment;
                ImGui::TextUnformatted(wifi_master::enrolment_label(e));
                if(e.digital_review)ImGui::TextColored(ImVec4(1,.65f,.2f,1),"DIGITAL CHANGE");
                if(wifi_master::has_rf_review(e))ImGui::TextColored(ImVec4(1,.65f,.2f,1),"RF REVIEW (experimental)");
            }else ImGui::TextDisabled("%s",row.key_is_mac?"REVIEW REQUIRED":"RF CANDIDATE");
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
            if(row.enrolment){const auto& e=*row.enrolment;
                ImGui::TextWrapped("%s | %s",wifi_master::enrolment_label(e),e.role.c_str());
                if(!e.alias.empty())ImGui::TextWrapped("Alias: %s",e.alias.c_str());
                ImGui::TextWrapped("Latest source: %s",e.last_source.c_str());
                ImGui::Text("Since enrolment: %llu observations | Accepted RF: %llu | Rejected RF: %llu",(unsigned long long)e.observations,(unsigned long long)e.accepted,(unsigned long long)e.rejected);
                ImGui::TextDisabled("Presence is last observation only; scan gaps are not evidence of absence.");
                if(e.seen_24)ImGui::Text("2.4 GHz last observed: %s",date(e.seen_24).c_str());
                if(e.seen_5)ImGui::Text("5 GHz last observed: %s",date(e.seen_5).c_str());
                if(command){
                    if(v.edit_key!=row.device_key){v.edit_key=row.device_key;std::snprintf(v.alias,sizeof(v.alias),"%s",e.alias.c_str());std::snprintf(v.note,sizeof(v.note),"%s",e.note.c_str());}
                    if(ImGui::Button("Acknowledge"))submit("acknowledge",row.device_key);
                    ImGui::SameLine();if(ImGui::Button("Approve / enrol...")){v.pending_key=row.device_key;v.pending_action="approve";ImGui::OpenPopup("Confirm enrolment action");}
                    if(ImGui::Button("Mark disallowed...")){v.pending_key=row.device_key;v.pending_action="disallow";ImGui::OpenPopup("Confirm enrolment action");}
                    ImGui::SameLine();if(ImGui::Button("Revoke approval"))submit("unapprove",row.device_key);
                    ImGui::InputText("Alias",v.alias,sizeof(v.alias));ImGui::InputTextMultiline("Note",v.note,sizeof(v.note),ImVec2(0,55));
                    if(ImGui::Button("Save label / note"))submit("note",row.device_key);
                    if(e.digital_review&&ImGui::Button("Acknowledge digital changes"))submit("review_digital",row.device_key);
                    if(wifi_master::has_rf_review(e)&&ImGui::Button("Acknowledge RF warnings"))submit("review_rf",row.device_key);
                    if(ImGui::Button("Accept / replace RF reference...")){v.pending_key=row.device_key;v.pending_action="reference";ImGui::OpenPopup("Confirm enrolment action");}
                    if(ImGui::BeginPopupModal("Confirm enrolment action",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
                        ImGui::TextWrapped("Action: %s for %s",v.pending_action.c_str(),v.pending_key.c_str());
                        ImGui::TextWrapped("Approval is your local policy, not authenticated hardware identity. RF references preserve prior versions. Automatic adaptation remains disabled.");
                        if(ImGui::Button("Confirm")){submit(v.pending_action,v.pending_key);ImGui::CloseCurrentPopup();}ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
                    }
                }
                if(ImGui::CollapsingHeader("RF profile and reference history",ImGuiTreeNodeFlags_DefaultOpen)){
                    ImGui::TextWrapped("Automatic trusted adaptation: OFF. Window summaries update; accepted references never change without an explicit action. Comparisons are experimental.");
                    if(!e.last_rejection.empty())ImGui::TextWrapped("Last exclusion: %s",e.last_rejection.c_str());
                    if(e.profiles.empty())ImGui::TextDisabled("No comparable RF profile. Digital identity still retained.");
                    for(size_t pi=0;pi<e.profiles.size();++pi){const auto& p=e.profiles[pi];ImGui::PushID(int(pi));
                        if(ImGui::TreeNode("Context","%s: %zu captures | %s",p.phy.c_str(),p.captures,p.review?"REVIEW":p.has_reference?"Reference available":"Collecting / no reference")){
                            ImGui::TextWrapped("%s",p.context.c_str());ImGui::Text("%s to %s",date(p.first_ts).c_str(),date(p.last_ts).c_str());
                            const char* names[]={"CFO (ppm)","Image/signal (dB)","IQ amplitude error","IQ phase (degrees)","DC (dBc)","DC phase (degrees)"};
                            for(int m=0;m<6;++m){if(!(p.valid&(1u<<m))){ImGui::TextDisabled("%s: unavailable",names[m]);continue;}
                                ImGui::Text("%s: %.4g | MAD %.4g",names[m],p.centre[m],p.spread[m]);if(p.has_reference)ImGui::Text("  Reference %.4g | deviation %+.4g",p.reference[m],p.deviation[m]);}
                            if(!p.cfo_history.empty())ImGui::PlotLines("CFO capture summaries",p.cfo_history.data(),int(p.cfo_history.size()),0,"Capture order; gaps not shown as elapsed time",FLT_MAX,FLT_MAX,ImVec2(0,70));
                            ImGui::TextWrapped("Source: %s",p.source.c_str());ImGui::TreePop();}
                        ImGui::PopID();
                    }
                    if(!e.archived_profiles.empty()&&ImGui::TreeNode("Archived receiver contexts")){
                        ImGui::TextDisabled("Latest 64 retired context summaries; accepted references remain preserved separately.");
                        for(const auto& p:e.archived_profiles){ImGui::Separator();ImGui::TextWrapped("%s",p.context.c_str());
                            ImGui::Text("%zu captures | CFO %.4g ppm | MAD %.4g | %s",p.captures,p.centre[0],p.spread[0],p.review?"REVIEW":"Historical");}
                        ImGui::TreePop();
                    }
                    ImGui::Text("Accepted reference versions: %llu (maximum 8)",(unsigned long long)e.reference_version);
                }
                if(ImGui::CollapsingHeader("Discovery, change and operator history")){
                    ImGui::Text("Latest %zu of %llu events retained",e.history.size(),(unsigned long long)e.history_total);
                    for(auto it=e.history.rbegin();it!=e.history.rend();++it){ImGui::Separator();ImGui::TextWrapped("%s | %s",date(it->ts).c_str(),it->kind.c_str());ImGui::TextWrapped("%s",it->detail.c_str());ImGui::TextDisabled("%s",it->source.c_str());}
                }
                if(ImGui::CollapsingHeader("Related security incidents")){
                    ImGui::TextWrapped("Association by claimed address in the published incident tail only; not authenticated attribution or complete history. Use Security workspace for evidence.");
                    if(row.related_incidents.empty())ImGui::TextDisabled("No address association in current incident snapshot.");
                    for(const auto& incident:row.related_incidents)ImGui::BulletText("%s",incident.c_str());
                }
                if(ImGui::Button("Copy enrolment evidence JSON")){
                    nlohmann::json j={{"source","Wi-Fi enrolment; claimed identity"},{"key",row.device_key},{"policy",e.policy},{"alias",e.alias},{"note",e.note},{"last_source",e.last_source},{"first_seen",row.first_seen_ts},{"last_seen",row.last_seen_ts},{"observations",e.observations},{"reference_version",e.reference_version},{"history_total",e.history_total}};
                    j["history"]=nlohmann::json::array();for(const auto& h:e.history)j["history"].push_back({{"id",h.id},{"ts",h.ts},{"kind",h.kind},{"detail",h.detail},{"source",h.source}});
                    j["profiles"]=nlohmann::json::array();for(const auto& p:e.profiles)j["profiles"].push_back({{"context",p.context},{"valid_mask",p.valid},{"centre",p.centre},{"mad",p.spread},{"has_reference",p.has_reference},{"reference",p.reference},{"review",p.review},{"source",p.source}});
                    ImGui::SetClipboardText(j.dump(2).c_str());
                }
            }
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
