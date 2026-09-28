#include "security_gui.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <vector>

#include "imgui.h"
#include "wifi_frame.hpp"

namespace rfmon {

using namespace wifi_security;

namespace {

const ImVec4 kDim(0.55f, 0.55f, 0.58f, 1.0f);
const ImVec4 kWarn(0.95f, 0.65f, 0.20f, 1.0f);
const ImVec4 kBad(0.90f, 0.40f, 0.35f, 1.0f);
const ImVec4 kOk(0.25f, 0.73f, 0.31f, 1.0f);
const ImVec4 kMgmt(0.80f, 0.70f, 0.95f, 1.0f);   // informational highlight, not an alert colour
const ImVec4 kDsss(0.36f, 0.68f, 0.93f, 1.0f);

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof(b), f, v);
    return b;
}

std::string mac_or_dash(const std::optional<MacAddress>& a) { return a ? format_mac(*a) : "--"; }

// Host wall-clock with its uncertainty. Device time is exact within a radio
// session but is not a wall clock, so the table shows host time and says so.
std::string time_label(const FrameEvent& e) {
    if (!e.host_time_ns) return e.after_overflow ? "after overflow gap" : "time unknown";
    std::time_t sec = std::time_t(*e.host_time_ns / 1000000000);
    std::tm tm{};
    localtime_r(&sec, &tm);
    char buf[48];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    char out[80];
    std::snprintf(out, sizeof(out), "%s.%03lld", buf, static_cast<long long>((*e.host_time_ns / 1000000) % 1000));
    return out;
}

bool security_relevant(const MacFrame& f) {
    if (f.fc.protocol_version != 0 || f.fc.type != FrameType::Management) return false;
    switch (f.fc.subtype) {
        case mgmt::Deauthentication: case mgmt::Disassociation: case mgmt::Authentication:
        case mgmt::AssocRequest: case mgmt::AssocResponse: case mgmt::ReassocRequest: case mgmt::ReassocResponse:
        case mgmt::Action: case mgmt::ActionNoAck:
            return true;
        default:
            return false;
    }
}

void coverage_tab(const SecuritySnapshot& s, float height) {
    if (s.coverage.empty()) { ImGui::TextDisabled("No Wi-Fi captures ingested yet."); return; }
    const TimelineSummary& t = s.timeline;
    ImGui::Text("Receiver timeline: sampled %.1f s, dead %.1f s", t.sampled_s, t.dead_s);
    ImGui::SameLine();
    ImGui::TextColored(t.duty() < 0.5 ? kWarn : kOk, "duty %.0f%%", 100.0 * t.duty());
    ImGui::SameLine();
    ImGui::TextDisabled("| %llu gaps (max %.2f s), %llu unknown, %llu radio sessions, %llu retunes skipped",
                        static_cast<unsigned long long>(t.gaps), t.max_gap_s,
                        static_cast<unsigned long long>(t.unknown_gaps), static_cast<unsigned long long>(t.radio_sessions),
                        static_cast<unsigned long long>(t.retune_skipped));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("From USRP device time. Dead time is unsampled time between consecutive captures of one radio "
                          "session (retune/settle, processing, other channels). Gaps after an overflow of unknown "
                          "length or across restarts are counted as unknown, never as observed.");
    ImGui::TextDisabled("Sampled = received. Analysed = actually examined for bursts: once a capture hits the "
                        "burst cap, the rest of it is never looked at and does not count as observed.");
    const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    if (!ImGui::BeginTable("wifi_security_coverage_v2", 17, flags, ImVec2(0, height - ImGui::GetTextLineHeightWithSpacing()))) return;
    ImGui::TableSetupScrollFreeze(2, 1);
    const char* cols[] = {"Band", "Ch", "Captures", "Failed", "Overflow", "Sampled (s)", "Dead before (s)", "Analysed (s)",
                          "Analysed %", "Burst cap hit", "Bursts", "Security-only tail", "DSSS dec / FCS",
                          "OFDM dec / FCS", "Frames", "Queue drops", "Processing (s)"};
    const float widths[] = {75, 40, 75, 60, 70, 90, 115, 95, 85, 100, 70, 135, 115, 115, 70, 95, 105};
    for (int i = 0; i < 17; ++i) ImGui::TableSetupColumn(cols[i], ImGuiTableColumnFlags_WidthFixed, widths[i]);
    ImGui::TableHeadersRow();
    for (const auto& [key, c] : s.coverage) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.band == "wifi_5g" ? "5 GHz" : "2.4 GHz");
        ImGui::TableNextColumn(); ImGui::Text("%d", c.channel);
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(c.captures));
        ImGui::TableNextColumn();
        if (c.failed_captures) ImGui::TextColored(kBad, "%llu", static_cast<unsigned long long>(c.failed_captures));
        else ImGui::TextColored(kDim, "0");
        ImGui::TableNextColumn();
        if (c.overflow_captures) ImGui::TextColored(kBad, "%llu", static_cast<unsigned long long>(c.overflow_captures));
        else ImGui::TextColored(kDim, "0");
        ImGui::TableNextColumn(); ImGui::Text("%.2f", c.sampled_s);
        ImGui::TableNextColumn(); ImGui::Text("%.2f", c.dead_s);
        ImGui::TableNextColumn(); ImGui::Text("%.2f", c.analysed_s);
        ImGui::TableNextColumn();
        const double pct = c.sampled_s > 0 ? 100.0 * c.analysed_s / c.sampled_s : 0.0;
        ImGui::TextColored(pct < 90.0 ? kWarn : kOk, "%.0f%%", pct);
        ImGui::TableNextColumn();
        if (c.burst_cap_captures)
            ImGui::TextColored(kWarn, "%llu / %llu", static_cast<unsigned long long>(c.burst_cap_captures),
                               static_cast<unsigned long long>(c.captures));
        else ImGui::TextColored(kDim, "0");
        if (ImGui::IsItemHovered() && c.burst_cap_captures)
            ImGui::SetTooltip("Captures that reached the per-capture burst limit; later transmissions in them "
                              "were not examined.");
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(c.bursts));
        ImGui::TableNextColumn();
        if (c.bursts_beyond_identity_limit) ImGui::Text("%llu", static_cast<unsigned long long>(c.bursts_beyond_identity_limit));
        else ImGui::TextColored(kDim, "0");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Bursts past the per-capture packet-row limit: analysed for security, but not used for "
                              "identities, fingerprints or the packet table.");
        ImGui::TableNextColumn();
        ImGui::Text("%llu / %llu", static_cast<unsigned long long>(c.dsss_attempts),
                    static_cast<unsigned long long>(c.dsss_fcs_valid));
        ImGui::TableNextColumn();
        ImGui::Text("%llu / %llu", static_cast<unsigned long long>(c.ofdm_attempts),
                    static_cast<unsigned long long>(c.ofdm_fcs_valid));
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(c.frames_accepted));
        ImGui::TableNextColumn();
        if (c.events_rejected_by_queue)
            ImGui::TextColored(kBad, "%llu", static_cast<unsigned long long>(c.events_rejected_by_queue));
        else ImGui::TextColored(kDim, "0");
        ImGui::TableNextColumn(); ImGui::Text("%.2f", c.processing_s);
    }
    ImGui::EndTable();
}

void frame_types_tab(const SecuritySnapshot& s, float height) {
    if (s.frames_by_type.empty()) { ImGui::TextDisabled("No FCS-valid frames accepted yet."); return; }
    std::vector<std::pair<std::string, uint64_t>> v(s.frames_by_type.begin(), s.frames_by_type.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("wifi_security_types", 3, flags, ImVec2(520, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Frame type", ImGuiTableColumnFlags_WidthFixed, 280);
        ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (const auto& [type, n] : v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool mgmt_relevant = type.rfind("Mgmt/Deauth", 0) == 0 || type.rfind("Mgmt/Disassoc", 0) == 0 ||
                                       type.rfind("Mgmt/Auth", 0) == 0 || type.rfind("Mgmt/Action", 0) == 0 ||
                                       type.find("ssociation") != std::string::npos;
            if (mgmt_relevant) ImGui::TextColored(kMgmt, "%s", type.c_str());
            else ImGui::TextUnformatted(type.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(n));
            ImGui::TableNextColumn();
            ImGui::Text("%.1f%%", s.frames_accepted ? 100.0 * double(n) / double(s.frames_accepted) : 0.0);
        }
        ImGui::EndTable();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("By PHY:");
    for (const auto& [phy, n] : s.frames_by_phy) ImGui::BulletText("%s: %llu", phy.c_str(), static_cast<unsigned long long>(n));
    ImGui::Spacing();
    ImGui::Text("Body state:");
    for (const auto& [st, n] : s.body_states) ImGui::BulletText("%s: %llu", st.c_str(), static_cast<unsigned long long>(n));
    ImGui::Spacing();
    ImGui::TextColored(kMgmt, "Highlighted");
    ImGui::SameLine();
    ImGui::TextDisabled("= management types later detector rules will use.");
    ImGui::TextDisabled("Counts are observed decoded frames, not transmitted totals.");
    ImGui::EndGroup();
}

void frame_details_popup(const ProcessedFrame& p) {
    const FrameEvent& e = p.event;
    const MacFrame& f = p.frame;
    ImGui::Text("%s", frame_type_label(f).c_str());
    ImGui::TextDisabled("Received frame only: claimed addresses, FCS-validated bytes, no authenticity implied.");
    ImGui::Separator();
    ImGui::PushTextWrapPos(0);
    ImGui::TextWrapped("%s", describe(f).c_str());
    ImGui::Separator();
    ImGui::Text("Transmitter: %s | Receiver: %s | BSSID: %s", mac_or_dash(f.transmitter).c_str(),
                mac_or_dash(f.receiver).c_str(), mac_or_dash(f.bssid).c_str());
    if (f.transmitter && is_locally_administered(*f.transmitter))
        ImGui::TextDisabled("Transmitter address is locally administered (often MAC randomisation; not suspicious on its own).");
    ImGui::Text("Sequence: %s | Retry: %s | Protected: %s",
                f.sequence_number ? (std::to_string(*f.sequence_number) + "/" + std::to_string(*f.fragment_number)).c_str() : "--",
                f.fc.retry ? "yes" : "no", f.fc.protected_frame ? "yes" : "no");
    ImGui::Text("Header: %s | Body: %s", parse_state_name(f.header_state), body_state_name(f.body_state));
    if (!f.problem.empty()) ImGui::TextColored(kWarn, "Parser note: %s", f.problem.c_str());
    ImGui::Separator();
    ImGui::Text("Receive time (host): %s", time_label(e).c_str());
    if (e.host_time_ns) {
        ImGui::SameLine();
        ImGui::TextDisabled("+/- %.0f us", double(e.host_uncertainty_ns) / 1e3);
    }
    ImGui::Text("Clock domain: %s", clock_domain_name(e.clock));
    if (e.device_time_ns)
        ImGui::Text("Device time: %.9f s (radio session %llu; comparable only within one session)",
                    double(*e.device_time_ns) / 1e9, static_cast<unsigned long long>(e.radio_session));
    else ImGui::TextDisabled("Device time: not derivable%s", e.after_overflow ? " (after an overflow gap)" : "");
    ImGui::Text("Capture %llu, samples %zu..%zu at %.3f Msps | monitored ch %d (%.3f MHz)",
                static_cast<unsigned long long>(e.capture_seq), e.sample_start, e.sample_start + e.sample_length,
                e.sample_rate_hz / 1e6, e.channel, e.channel_hz / 1e6);
    ImGui::Text("PHY: %s %d Mbps%s | Power %.1f dB | BW %.2f MHz | %.1f us | confidence %.3f", e.phy.c_str(),
                e.rate_mbps, e.security_decode_only ? " (short-burst security decode)" : "", e.power_db,
                e.bandwidth_hz / 1e6, e.duration_us, e.confidence);
    if (e.fp_cfo_ppm) ImGui::Text("Fingerprint: CFO %.2f ppm%s", *e.fp_cfo_ppm, e.fp_gate_reason ? " (gated out)" : "");
    if (p.repeated_content && p.first_seen_as) {
        const auto& [run, seq, sample, phy] = *p.first_seen_as;
        ImGui::TextColored(kWarn, "Same bytes (ignoring Retry) first seen in capture %llu at sample %zu (%s).",
                           static_cast<unsigned long long>(seq), sample, phy.c_str());
        ImGui::TextDisabled("A separate reception, kept as its own frame. Repeats are normal for retries and control frames.");
    }
    ImGui::Separator();
    const std::string hex = to_hex(e.mpdu);
    ImGui::Text("MPDU (%zu octets incl. FCS)%s", e.mpdu.size(), e.mpdu_truncated ? " - truncated in storage" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy hex")) ImGui::SetClipboardText(hex.c_str());
    std::string spaced;
    for (size_t i = 0; i < hex.size() && i < 2 * 512; i += 2) {
        spaced += hex.substr(i, 2);
        spaced += ((i / 2 + 1) % 16 == 0) ? '\n' : ' ';
    }
    ImGui::BeginChild("mpdu_hex", ImVec2(0, 160), true);
    ImGui::TextUnformatted(spaced.c_str());
    ImGui::EndChild();
    ImGui::PopTextWrapPos();
}

void recent_tab(const SecuritySnapshot& s, float height) {
    static bool mgmt_only = false, hide_control = true;
    static int selected = -1;
    static ProcessedFrame selected_frame;
    ImGui::Checkbox("Management frames only", &mgmt_only);
    ImGui::SameLine();
    ImGui::Checkbox("Hide control frames (ACK/RTS/CTS/Block Ack)", &hide_control);
    ImGui::SameLine();
    ImGui::TextDisabled("| newest %zu shown, newest first; click a row for details", s.recent.size());
    if (s.recent.empty()) { ImGui::TextDisabled("No FCS-valid frames accepted yet."); return; }
    const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    bool open = false;
    if (ImGui::BeginTable("wifi_security_recent", 13, flags, ImVec2(0, height - ImGui::GetFrameHeightWithSpacing()))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        const char* cols[] = {"Time (host)", "Ch", "PHY", "Frame type", "Transmitter", "Receiver", "BSSID",
                              "Seq", "Retry", "Key fields", "Body", "Repeat", "Power (dB)"};
        const float widths[] = {110, 40, 85, 200, 140, 140, 140, 70, 50, 330, 130, 60, 85};
        for (int i = 0; i < 13; ++i) ImGui::TableSetupColumn(cols[i], ImGuiTableColumnFlags_WidthFixed, widths[i]);
        ImGui::TableHeadersRow();
        int shown = 0;
        for (size_t i = s.recent.size(); i-- > 0;) {
            const ProcessedFrame& p = s.recent[i];
            const MacFrame& f = p.frame;
            if (mgmt_only && f.fc.type != FrameType::Management) continue;
            if (hide_control && f.fc.type == FrameType::Control) continue;
            ++shown;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(int(i));
            if (ImGui::Selectable(time_label(p.event).c_str(), selected == int(i),
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                selected = int(i);
                selected_frame = p;
                open = true;
            }
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::Text("%d", p.event.channel);
            ImGui::TableNextColumn();
            ImGui::TextColored(p.event.phy == "DSSS" ? kDsss : kOk, "%s%s", p.event.phy.c_str(),
                               p.event.security_decode_only ? " short" : "");
            ImGui::TableNextColumn();
            if (security_relevant(f)) ImGui::TextColored(kMgmt, "%s", frame_type_label(f).c_str());
            else ImGui::TextUnformatted(frame_type_label(f).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mac_or_dash(f.transmitter).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mac_or_dash(f.receiver).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mac_or_dash(f.bssid).c_str());
            ImGui::TableNextColumn();
            if (f.sequence_number) ImGui::Text("%u", unsigned(*f.sequence_number)); else ImGui::TextColored(kDim, "--");
            ImGui::TableNextColumn();
            if (f.fc.retry) ImGui::TextUnformatted("yes"); else ImGui::TextColored(kDim, "no");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(wifi_security_key_info(p).c_str());
            ImGui::TableNextColumn();
            if (f.body_state == BodyState::Malformed || f.body_state == BodyState::Truncated)
                ImGui::TextColored(kWarn, "%s", body_state_name(f.body_state));
            else ImGui::TextUnformatted(body_state_name(f.body_state));
            ImGui::TableNextColumn();
            if (p.repeated_content) ImGui::TextUnformatted("yes"); else ImGui::TextColored(kDim, "no");
            ImGui::TableNextColumn(); ImGui::Text("%.1f", p.event.power_db);
        }
        ImGui::EndTable();
        if (!shown) ImGui::TextDisabled("No frames match the filters.");
    }
    if (open) ImGui::OpenPopup("Wi-Fi security frame details");
    ImGui::SetNextWindowSize(ImVec2(820, 560), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("Wi-Fi security frame details")) {
        // A copy is held so the popup stays stable while new frames arrive.
        frame_details_popup(selected_frame);
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void baselines_tab(const SecuritySnapshot& s, float height, const SecurityCommandFn& command) {
    ImGui::TextDisabled("Clean-traffic history per channel and receiver setting, in windows of analysed air. Only "
                        "contiguous captures are learned from. Freeze while reviewing suspected activity so it is not "
                        "learned as normal. Rates are observed decoded frames per analysed second.");
    const bool can = bool(command);
    if (!can) ImGui::BeginDisabled();
    if (s.baselines_frozen) {
        if (ImGui::Button("Unfreeze learning")) command("baseline_unfreeze", "");
    } else if (ImGui::Button("Freeze learning")) {
        command("baseline_freeze", "");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset all baselines...")) ImGui::OpenPopup("Reset all Wi-Fi baselines?");
    if (!can) ImGui::EndDisabled();
    ImGui::SameLine();
    if (s.baselines_frozen) ImGui::TextColored(kWarn, "Learning frozen: new windows are excluded from statistics.");
    else ImGui::TextColored(kDim, "Learning active.");
    if (s.baselines_restored)
        ImGui::TextDisabled("%zu baselines restored from disk; partial windows from the previous run were discarded.",
                            s.baselines_restored);
    if (!s.persist_error.empty()) ImGui::TextColored(kBad, "Saved state: %s", s.persist_error.c_str());
    if (ImGui::BeginPopupModal("Reset all Wi-Fi baselines?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Every baseline's learned windows are discarded and its version increases.");
        if (ImGui::Button("Reset")) { command("baseline_reset", ""); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (s.baselines.empty()) { ImGui::TextDisabled("No baselines yet: they start with the first contiguous capture."); return; }
    const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    const BaselineMetric shown[] = {BaselineMetric::Frames, BaselineMetric::Deauth, BaselineMetric::Disassoc,
                                    BaselineMetric::Auth, BaselineMetric::ProbeRequest, BaselineMetric::Beacon,
                                    BaselineMetric::Transmitters};
    const int n_fixed = 9, n_cols = n_fixed + int(std::size(shown));
    if (!ImGui::BeginTable("wifi_security_baselines", n_cols, flags, ImVec2(0, height - 3 * ImGui::GetFrameHeightWithSpacing())))
        return;
    ImGui::TableSetupScrollFreeze(2, 1);
    const char* fixed_cols[] = {"Band", "Ch", "Receiver", "Version", "State", "Windows used / closed",
                                "Excluded (frozen / degraded)", "Learned (s)", "Current window"};
    const float fixed_w[] = {70, 40, 170, 65, 110, 150, 185, 95, 115};
    for (int i = 0; i < n_fixed; ++i) ImGui::TableSetupColumn(fixed_cols[i], ImGuiTableColumnFlags_WidthFixed, fixed_w[i]);
    for (auto m : shown) {
        std::string label = std::string(baseline_metric_name(m)) + (m == BaselineMetric::Transmitters ? " /win p50|p95" : " /s p50|p95");
        ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed, 150);
    }
    ImGui::TableHeadersRow();
    for (const auto& b : s.baselines) {
        ImGui::TableNextRow();
        ImGui::PushID(b.key.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(b.band == "wifi_5g" ? "5 GHz" : "2.4 GHz");
        ImGui::TableNextColumn(); ImGui::Text("%d", b.channel);
        ImGui::TableNextColumn();
        ImGui::Text("%.0f Msps, %s", b.sample_rate_hz / 1e6, b.gain_db ? fmt("%.1f dB", *b.gain_db).c_str() : "AGC");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", b.key.c_str());
        ImGui::TableNextColumn(); ImGui::Text("v%u", b.version);
        ImGui::TableNextColumn();
        if (b.frozen) ImGui::TextColored(kWarn, "frozen");
        else if (!b.holders.empty()) ImGui::TextColored(kWarn, "rule hold");
        else ImGui::TextColored(kOk, "learning");
        if (ImGui::IsItemHovered() && !b.holders.empty()) {
            std::string holders; for (const auto& h : b.holders) holders += h + "\n";
            ImGui::SetTooltip("Learning held by:\n%s", holders.c_str());
        }
        if (b.restored) { ImGui::SameLine(); ImGui::TextDisabled("restored"); }
        ImGui::TableNextColumn();
        ImGui::Text("%zu / %llu", b.windows_included, static_cast<unsigned long long>(b.windows_closed));
        ImGui::TableNextColumn();
        ImGui::Text("%llu / %llu", static_cast<unsigned long long>(b.windows_excluded),
                    static_cast<unsigned long long>(b.captures_excluded));
        ImGui::TableNextColumn(); ImGui::Text("%.0f", b.learned_analysed_s);
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", b.current_analysed_s);
        for (auto m : shown) {
            ImGui::TableNextColumn();
            const auto& r = b.rates[size_t(m)];
            if (!b.windows_included) ImGui::TextColored(kDim, "--");
            else ImGui::Text("%.2f | %.2f", r.p50, r.p95);
            if (ImGui::IsItemHovered() && b.windows_included)
                ImGui::SetTooltip("mean %.3f, max %.3f over %zu windows", r.mean, r.max, b.windows_included);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void incidents_tab(const SecuritySnapshot& s, float height) {
    ImGui::TextDisabled("Incident store: coalesced, bounded, versioned records with evidence and coverage limits. "
                        "An incident closes only after quiet ANALYSED air on its channel, never because of a scan gap.");
    ImGui::Text("Open %zu | total %zu | evicted %llu", s.incidents_open, s.incidents_total,
                static_cast<unsigned long long>(s.incidents_evicted));
    if (s.incidents_restored) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %zu restored from disk (observation was interrupted by the restart)", s.incidents_restored);
    }
    if (s.incidents.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kDim, "%s", s.flood_enabled
            ? "No incidents recorded. Missing history, baseline warm-up and incomplete coverage can suppress detection."
            : "Flood detection disabled. An empty list does not establish absence of attacks.");
        return;
    }
    static uint64_t selected = 0;
    bool open_details = false;
    const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    if (!ImGui::BeginTable("wifi_security_incidents", 12, flags, ImVec2(0, height - 2 * ImGui::GetFrameHeightWithSpacing())))
        return;
    ImGui::TableSetupScrollFreeze(1, 1);
    const char* cols[] = {"#", "Rule", "State", "Severity", "Confidence", "Ch", "Claimed source", "Target",
                          "Observations", "Rate (per analysed s)", "Coverage", "Benign alternatives"};
    const float w[] = {45, 170, 90, 75, 90, 40, 140, 140, 100, 150, 220, 260};
    for (int i = 0; i < 12; ++i) ImGui::TableSetupColumn(cols[i], ImGuiTableColumnFlags_WidthFixed, w[i]);
    ImGui::TableHeadersRow();
    for (size_t k = s.incidents.size(); k-- > 0;) {
        const Incident& i = s.incidents[k];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(std::to_string(i.id).c_str(), selected == i.id)) { selected = i.id; open_details = true; }
        ImGui::TableNextColumn(); ImGui::Text("%s v%s", i.rule.c_str(), i.rule_version.c_str());
        ImGui::TableNextColumn();
        if (i.open) ImGui::TextColored(kWarn, "open%s", i.restored ? " (restored)" : "");
        else ImGui::TextColored(kDim, "closed");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(severity_name(i.severity));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(confidence_name(i.confidence));
        ImGui::TableNextColumn(); ImGui::Text("%d", i.channel);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(i.claimed_source.empty() ? "--" : i.claimed_source.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(i.target.empty() ? "--" : i.target.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(i.observations));
        ImGui::TableNextColumn();
        if (i.denominator_analysed_s > 0) ImGui::Text("%.2f (%.0f / %.1f s)", i.numerator / i.denominator_analysed_s, i.numerator, i.denominator_analysed_s);
        else ImGui::TextColored(kDim, "--");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(i.coverage_note.empty() ? "--" : i.coverage_note.c_str());
        ImGui::TableNextColumn();
        std::string alt;
        for (const auto& a : i.benign_alternatives) alt += (alt.empty() ? "" : "; ") + a;
        ImGui::TextUnformatted(alt.empty() ? "--" : alt.c_str());
    }
    ImGui::EndTable();
    if (open_details) ImGui::OpenPopup("Wi-Fi incident evidence");
    ImGui::SetNextWindowSize(ImVec2(920,650), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Wi-Fi incident evidence", nullptr, ImGuiWindowFlags_None)) {
        auto it = std::find_if(s.incidents.begin(),s.incidents.end(),[&](const auto& i){ return i.id == selected; });
        if (it == s.incidents.end()) ImGui::TextDisabled("Incident no longer retained.");
        else {
            const auto& i = *it;
            ImGui::TextWrapped("%s - suspected activity, service impact unverified", i.rule.c_str());
            ImGui::Text("Claimed BSSID: %s | target: %s", i.claimed_bssid.c_str(),i.target.c_str());
            ImGui::Text("Mode: %s | confidence: %s | severity: %s", i.mode.c_str(),confidence_name(i.confidence),severity_name(i.severity));
            if (i.denominator_analysed_s > 0) ImGui::Text("Latest triggering rate: %.2f /s (%.0f units / %.2f observed s)",
                i.denominator_analysed_s > 0 ? i.numerator/i.denominator_analysed_s : 0, i.numerator,i.denominator_analysed_s);
            if (auto t = i.measures.find("threshold"); t != i.measures.end()) ImGui::Text("Threshold: %.2f /s (provisional configuration)",t->second);
            if (i.rule == "historical_beacon_replay") {
                for (const auto& m : i.measures) ImGui::Text("%s: %.3f", m.first.c_str(), m.second);
                for (const auto& c : i.context) ImGui::TextWrapped("%s: %s", c.first.c_str(), c.second.c_str());
                ImGui::TextDisabled("Historical match rule; no learned flood baseline required.");
            } else ImGui::TextWrapped("Baseline: %s v%u",i.baseline_key.c_str(),i.baseline_version);
            for (const auto& b : s.baselines) if (b.key == i.baseline_key)
                ImGui::Text("Learning: %s | rule holders: %zu | held windows: %llu", b.frozen ? "operator frozen" : b.holders.empty() ? "enabled" : "rule held",b.holders.size(),(unsigned long long)b.windows_held);
            ImGui::TextWrapped("Coverage: %s",i.coverage_note.c_str());
            for (const auto& basis : i.confidence_basis) ImGui::BulletText("%s",basis.c_str());
            if (ImGui::CollapsingHeader("Benign alternatives"))
                for (const auto& a : i.benign_alternatives) ImGui::BulletText("%s",a.c_str());
            if (!i.timeline.empty() && ImGui::CollapsingHeader("Rate timeline", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginTable("incident_timeline",5,ImGuiTableFlags_Borders|ImGuiTableFlags_ScrollY,ImVec2(0,140))) {
                    for (auto label : {"Capture range","Observed s","Units / raw","Rate / threshold","Threshold basis"}) ImGui::TableSetupColumn(label);
                    ImGui::TableHeadersRow();
                    for (const auto& t : i.timeline) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%llu-%llu",(unsigned long long)t.capture_seq_first,(unsigned long long)t.capture_seq_last);
                        ImGui::TableNextColumn(); ImGui::Text("%.2f",t.analysed_s);
                        ImGui::TableNextColumn(); ImGui::Text("%.0f / %.0f",t.units,t.raw);
                        ImGui::TableNextColumn(); ImGui::Text("%.2f / %.2f",t.rate,t.threshold);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(t.governing_term.c_str());
                    }
                    ImGui::EndTable();
                }
            }
            if (ImGui::CollapsingHeader("First and latest evidence", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::BeginChild("incident_evidence",ImVec2(0,140),true);
                auto evidence = [&](const IncidentEvidence& e) {
                    ImGui::TextWrapped("Role: %s",e.role.empty() ? "frame" : e.role.c_str());
                    ImGui::TextWrapped("%s | capture %llu sample %zu | %s",std::get<0>(e.frame).c_str(),(unsigned long long)std::get<1>(e.frame),std::get<2>(e.frame),e.summary.c_str());
                    ImGui::TextWrapped("%s",e.note.c_str());
                    ImGui::TextWrapped("MPDU prefix (%zu bytes): %s",e.mpdu.size(),to_hex(e.mpdu).c_str());
                    ImGui::Separator();
                };
                for (const auto& e : i.first_evidence) evidence(e);
                for (const auto& e : i.evidence) evidence(e);
                ImGui::EndChild();
            }
        }
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace

std::string wifi_security_key_info(const ProcessedFrame& p) {
    const MacFrame& f = p.frame;
    std::string s;
    auto add = [&](const std::string& v) { if (!s.empty()) s += " | "; s += v; };
    if (f.cipher) {
        const char* kinds[] = {"WEP", "CCMP/GCMP", "TKIP", "cipher ambiguous"};
        add(std::string(kinds[int(f.cipher->kind)]) + " key " + std::to_string(f.cipher->key_id) +
            (f.cipher->packet_number ? " PN " + std::to_string(*f.cipher->packet_number) : ""));
    }
    if (f.management) {
        const auto& m = *f.management;
        if (m.reason_code) add("reason " + std::to_string(*m.reason_code) + " " + reason_code_name(*m.reason_code));
        if (m.auth_algorithm)
            add(auth_algorithm_name(*m.auth_algorithm) + " #" + std::to_string(m.auth_transaction.value_or(0)));
        if (m.sae_group) add("SAE group " + std::to_string(*m.sae_group));
        if (m.status_code) add("status " + std::to_string(*m.status_code) + " " + status_code_name(*m.status_code));
        if (m.association_id) add("AID " + std::to_string(*m.association_id));
        if (m.action_category)
            add("action " + std::to_string(*m.action_category) + "/" +
                (m.action_code ? std::to_string(*m.action_code) : std::string("?")) + (m.robust_action ? " robust" : ""));
        if (m.sa_query_transaction) add("SA Query " + std::to_string(*m.sa_query_transaction));
        if (m.elements) {
            const auto& e = *m.elements;
            if (e.ssid_present) add(e.ssid_wildcard ? "SSID <wildcard>" : e.ssid_all_zero || !e.ssid || e.ssid->empty()
                                                                       ? "SSID <hidden>"
                                                                       : "SSID " + wifi::display_text(*e.ssid));
            if (e.channel_switch)
                add("channel switch -> " + std::to_string(e.channel_switch->new_channel) + " in " +
                    std::to_string(e.channel_switch->count));
            if (e.quiet) add("quiet " + std::to_string(e.quiet->duration_tu) + " TU");
            if (e.mme) add("MME IPN " + std::to_string(e.mme->ipn));
        }
        if (m.timestamp_tsf) add("TSF " + fmt("%.6f s", double(*m.timestamp_tsf) / 1e6));
    }
    if (f.data) {
        if (f.data->eapol_key)
            add(std::string("EAPOL ") + eapol_message_name(f.data->eapol_key->message) + " replay " +
                std::to_string(f.data->eapol_key->replay_counter));
        else if (f.data->ethertype) {
            char b[24];
            std::snprintf(b, sizeof(b), "ethertype 0x%04x", *f.data->ethertype);
            add(b);
        } else if (f.data->null_function) add("null (power save)");
    }
    if (s.empty() && f.nav_us) add("NAV " + std::to_string(*f.nav_us) + " us");
    return s.empty() ? "--" : s;
}

std::string wifi_security_header_label(const SecuritySnapshot& s) {
    uint64_t capped = 0, captures = 0;
    for (const auto& [k, c] : s.coverage) { capped += c.burst_cap_captures; captures += c.captures; }
    char b[320];
    std::snprintf(b, sizeof(b),
                  "Wi-Fi security monitor - %llu frames, %llu captures, duty %.0f%%%s%s%s (passive security)"
                  "###wifi_security_monitor",
                  static_cast<unsigned long long>(s.frames_accepted), static_cast<unsigned long long>(captures),
                  100.0 * s.timeline.duty(),
                  capped ? (", " + std::to_string(capped) + " hit burst cap").c_str() : "",
                  (s.queue_events_dropped || s.queue_captures_dropped) ? ", QUEUE LOSS" : "",
                  s.incidents_open ? (", " + std::to_string(s.incidents_open) + " open incidents").c_str() : "");
    return b;
}

void draw_wifi_security_panel(const SecuritySnapshot& s, const QueueStats& q, const std::string& run_id,
                              const std::string& recording, float height, const char* select_tab,
                              const SecurityCommandFn& command) {
    ImGui::TextDisabled("Passive disconnect-flood and historical-beacon replay detection. Sender and impact unverified.");
    ImGui::Text("Flood rule: %s | evaluated windows %llu | excluded captures %llu", s.flood_enabled ? "enabled (baseline required)" : "disabled", (unsigned long long)s.flood_evaluations,(unsigned long long)s.flood_excluded_captures);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Provisional configuration: %s",s.flood_config.c_str());
    ImGui::Text("Beacon replay: %s | evaluated %llu | excluded captures %llu | history evictions %llu",
        s.beacon_replay_enabled ? "enabled (device time required)" : "disabled",
        (unsigned long long)s.beacon_replay_evaluated, (unsigned long long)s.beacon_replay_excluded,
        (unsigned long long)s.beacon_replay_forgotten);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Provisional configuration: %s", s.beacon_replay_config.c_str());
    // Summary strip.
    ImGui::Text("Frames accepted %llu of %llu offered", static_cast<unsigned long long>(s.frames_accepted),
                static_cast<unsigned long long>(s.frames_ingested));
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine(); ImGui::Text("short-burst DSSS %llu", static_cast<unsigned long long>(s.frames_security_only));
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine(); ImGui::Text("repeated content %llu", static_cast<unsigned long long>(s.frames_repeated_content));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Same bytes (ignoring Retry) received again elsewhere. Normal for retries and control "
                          "frames; kept as separate frames, never merged.");
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine();
    const uint64_t rejected = s.frames_rejected_fcs + s.frames_rejected_malformed;
    if (rejected) ImGui::TextColored(kWarn, "rejected %llu", static_cast<unsigned long long>(rejected));
    else ImGui::TextColored(kDim, "rejected 0");
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (s.frames_without_device_time)
        ImGui::TextColored(kWarn, "without device time %llu", static_cast<unsigned long long>(s.frames_without_device_time));
    else ImGui::TextColored(kDim, "all frames device-timed");

    ImGui::Text("Queue depth %zu (peak %zu)", q.depth, q.high_water);
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (q.events_dropped || q.captures_dropped)
        ImGui::TextColored(kBad, "QUEUE LOSS: %llu frames, %llu capture records dropped",
                           static_cast<unsigned long long>(q.events_dropped),
                           static_cast<unsigned long long>(q.captures_dropped));
    else ImGui::TextColored(kDim, "no queue loss");
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (!s.storage_error.empty()) ImGui::TextColored(kBad, "Recording failed: %s", s.storage_error.c_str());
    else if (!recording.empty())
        ImGui::Text("Recording %.1f MB -> %s", double(s.recorded_bytes) / 1e6, recording.c_str());
    else ImGui::TextColored(kDim, "not recording");
    ImGui::SameLine(); ImGui::TextDisabled("| run %s", run_id.c_str());

    if (ImGui::BeginTabBar("wifi_security_tabs")) {
        const float h = std::max(120.0f, height - 3.5f * ImGui::GetFrameHeightWithSpacing());
        auto tab_flags = [&](const char* name) {
            return select_tab && std::string(select_tab) == name ? ImGuiTabItemFlags_SetSelected : 0;
        };
        if (ImGui::BeginTabItem("Coverage", nullptr, tab_flags("Coverage"))) {
            coverage_tab(s, h - ImGui::GetTextLineHeightWithSpacing());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Frame types", nullptr, tab_flags("Frame types"))) { frame_types_tab(s, h); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Recent frames", nullptr, tab_flags("Recent frames"))) { recent_tab(s, h); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Baselines", nullptr, tab_flags("Baselines"))) { baselines_tab(s, h, command); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Incidents", nullptr, tab_flags("Incidents"))) { incidents_tab(s, h); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

}  // namespace rfmon
