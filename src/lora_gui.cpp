#include "lora_gui.hpp"
#include "fingerprint.hpp"
#include "lora_capture.hpp"
#include "lora_export.hpp"
#include "imgui.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <ctime>
#include <future>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace rfmon {
namespace {

const ImVec4 kDim(0.55f, 0.55f, 0.58f, 1.0f);
const ImVec4 kGood(0.25f, 0.73f, 0.31f, 1.0f);
const ImVec4 kBad(0.85f, 0.30f, 0.28f, 1.0f);
const ImVec4 kCaution(0.86f, 0.65f, 0.13f, 1.0f);

std::string cluster_label(int id) {
    char b[16];
    std::snprintf(b, sizeof b, "LORA-%04d", id);
    return b;
}

std::string ago(int64_t ts) {
    if (ts <= 0) return "--";
    const double s = double(std::time(nullptr) - ts);
    char b[32];
    if (s < 120) std::snprintf(b, sizeof b, "%.0fs ago", s);
    else if (s < 7200) std::snprintf(b, sizeof b, "%.1fm ago", s / 60);
    else if (s < 172800) std::snprintf(b, sizeof b, "%.1fh ago", s / 3600);
    else std::snprintf(b, sizeof b, "%.1fd ago", s / 86400);
    return b;
}

std::string clock_time(double ts) {
    if (ts <= 0) return "--";
    std::time_t t = std::time_t(ts);
    std::tm tm{};
    localtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

// Export buttons share one status line per call site.
void export_button(const char* label, const char* stem, const char* ext, const std::function<std::string()>& content,
                   std::string& message) {
    if (ImGui::SmallButton(label)) {
        try {
            message = "Saved " + lora_export::write(lora_export::default_directory(), stem, ext, content());
        } catch (const std::exception& e) {
            message = std::string("Export failed: ") + e.what();
        }
    }
}

// Best hypothesis of a reception: CRC valid > complete payload > header > evidence.
int rank(const LoraPacketRow& r) {
    if (r.crc == LoraPacketRow::Crc::Valid) return 4;
    if (r.header_valid && r.payload_complete && r.crc == LoraPacketRow::Crc::Absent) return 3;
    if (r.header_valid && r.payload_complete) return 2;
    if (r.header_valid) return 1;
    return 0;
}

void stat_tile(const char* label, const std::string& value, const char* tip = nullptr, const ImVec4* color = nullptr) {
    ImGui::BeginGroup();
    ImGui::BeginChild(label, ImVec2(170, 52), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextDisabled("%s", label);
    if (color) ImGui::TextColored(*color, "%s", value.c_str());
    else ImGui::TextUnformatted(value.c_str());
    ImGui::EndChild();
    ImGui::EndGroup();
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
}

// Lays tiles left to right, wrapping at the available width.
struct TileRow {
    int per_row = 1, placed = 1;
    TileRow() { per_row = std::max(1, int((ImGui::GetContentRegionAvail().x + 8) / 178)); }
    void next() {
        if (placed++ % per_row) ImGui::SameLine(0, 8);
    }
};

void draw_packet_row(const LoraPacketRow& d, bool child, size_t group_size, bool* expanded,
                     std::optional<LoraPacketRow>& selected, int id) {
    const bool verified = d.crc == LoraPacketRow::Crc::Valid;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(id);
    if (!child && group_size > 1 && expanded) {
        if (ImGui::SmallButton(*expanded ? "-" : "+")) *expanded = !*expanded;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s all %zu hypotheses of this reception", *expanded ? "Hide" : "Show", group_size);
        ImGui::SameLine(0, 4);
    } else if (child) {
        ImGui::Indent(14);
    }
    if (ImGui::Selectable(d.time.c_str(), false)) selected = d;
    if (child) ImGui::Unindent(14);
    ImGui::PopID();
    ImGui::TableNextColumn();
    ImGui::TextColored(verified ? kGood : d.crc == LoraPacketRow::Crc::Failed ? kBad : kDim, "%s", d.status.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.detail.c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(d.decoder.c_str());
    ImGui::TableNextColumn();
    if (d.header_valid && d.sync_check_skipped.value_or(false)) {
        // Checksum-valid but NOT sync-word-verified (diagnostic toggle on).
        ImGui::TextColored(kCaution, "Valid*");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Header checksum passed, but the sync-word check was bypassed "
                              "(diagnostic toggle) - NOT cross-checked against the sync word. "
                              "Weaker evidence than a normal \"Valid\".");
    } else {
        ImGui::TextUnformatted(d.header_valid ? "Valid" : "Unverified");
    }
    ImGui::TableNextColumn();
    ImGui::Text("%.4f", d.freq_mhz);
    ImGui::TableNextColumn();
    ImGui::Text("%d", d.sf);
    ImGui::TableNextColumn();
    ImGui::Text("%.0f", d.bandwidth_khz);
    ImGui::TableNextColumn();
    if (d.cr.has_value()) ImGui::Text("4/%d", 4 + *d.cr); else ImGui::TextDisabled("--");
    ImGui::TableNextColumn();
    if (d.ldro.has_value()) {
        ImGui::Text("%s%s", *d.ldro ? "On" : "Off", d.ldro_ambiguous ? "?" : "");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Payload LDRO is inferred. '?' means the selected hypothesis is not resolved by a valid payload CRC.");
    } else ImGui::TextDisabled("--");
    ImGui::TableNextColumn();
    if (d.sync_word) ImGui::Text("0x%02X", *d.sync_word); else ImGui::TextDisabled("--");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Observed sync symbols; not a protocol or device identity. Other values are accepted.");
    ImGui::TableNextColumn();
    if (d.payload_len.has_value()) ImGui::Text("%d", *d.payload_len); else ImGui::TextDisabled("--");
    ImGui::TableNextColumn();
    ImGui::TextColored(verified ? kGood : d.crc == LoraPacketRow::Crc::Failed ? kBad : kDim, "%s", lora_crc_label(d.crc));
    ImGui::TableNextColumn();
    ImGui::Text("%d", d.cfo_bins);
    ImGui::TableNextColumn();
    if (d.fp_snr_db.has_value()) ImGui::Text("%.1f", *d.fp_snr_db);
    else if (d.fp_gate_reason.has_value()) ImGui::TextColored(kBad, "%s", d.fp_gate_reason->c_str());
    else ImGui::TextDisabled("--");
    ImGui::TableNextColumn();
    if (d.fp_evm_pct.has_value()) ImGui::TextColored(*d.fp_evm_pct > fingerprint::LORA_EVM_CEILING_PCT ? kBad : kGood, "%.2f", *d.fp_evm_pct);
    else ImGui::TextDisabled("--");
    ImGui::TableNextColumn();
    if (d.fp_sync_corr.has_value()) ImGui::TextColored(*d.fp_sync_corr < fingerprint::LORA_SYNC_CORR_FLOOR ? kBad : kGood, "%.3f", *d.fp_sync_corr);
    else ImGui::TextDisabled("--");
    auto opt = [](const std::optional<double>& v, const char* f) {
        ImGui::TableNextColumn();
        if (v.has_value()) ImGui::Text(f, *v); else ImGui::TextDisabled("--");
    };
    opt(d.fp_cfo_ppm, "%.3f");
    opt(d.fp_irr_db, "%.2f");
    opt(d.fp_iq_eps, "%.5f");
    opt(d.fp_iq_phi_deg, "%.3f");
    opt(d.fp_dc_dbc, "%.1f");
    opt(d.fp_dc_ang_deg, "%.1f");
    ImGui::TableNextColumn();
    if (d.payload_repr.has_value()) {
        ImGui::TextUnformatted(d.payload_hex.empty() ? "(empty payload)" : d.payload_hex.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ASCII: %s\n%s", d.payload_repr->c_str(), d.detail.c_str());
    } else ImGui::TextDisabled("--");
    ImGui::TableNextColumn();
    if (d.inverted_iq) ImGui::TextUnformatted(*d.inverted_iq ? "Inverted" : "Normal"); else ImGui::TextDisabled("--");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Polarity hypothesis used to recover the header; does not establish uplink/downlink or LoRaWAN.");
    opt(d.preamble_peak_ratio, "%.3f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aligned dechirped FFT peak / sum of sample magnitudes. Correlation evidence, not probability or SNR.");
    opt(d.sfd_peak_ratio, "%.3f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Weaker peak ratio of the two aligned SFD downchirps.");
    opt(d.cfo_hz, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fractional preamble offset estimate in captured-IQ convention; includes receiver and residual timing error. Not calibrated transmitter frequency.");
    opt(d.drift_hz_per_symbol, "%.3f");
    opt(d.capture_offset_s, "%.6f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Coarse detected preamble position relative to this capture; not a hardware timestamp or precise packet onset.");
    ImGui::TableNextColumn();
    if (d.fec_disagreements) ImGui::Text("%d", *d.fec_disagreements); else ImGui::TextDisabled("--");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Codewords differing from their nearest valid codeword. Not corrected-bit count; zero does not guarantee integrity.");
    ImGui::TableNextColumn();
    if (d.lorawan_candidate) {
        ImGui::TextUnformatted(d.lorawan_candidate->c_str());
        if (ImGui::IsItemHovered() && d.lorawan_detail) ImGui::SetTooltip("%s", d.lorawan_detail->c_str());
    } else ImGui::TextDisabled("Unclassified");
    ImGui::TableNextColumn();
    if (d.reception_id.empty()) ImGui::TextDisabled("Ungrouped / unavailable");
    else ImGui::Text("%s (%zu hypotheses)", d.reception_id.c_str(), d.reception_hypotheses);
    ImGui::TableNextColumn();
    if (d.rf_cluster_id) ImGui::TextUnformatted(cluster_label(*d.rf_cluster_id).c_str()); else ImGui::TextDisabled("--");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Persistent RF cluster whose fingerprint came from this burst. Heuristic IRR/DC/IQ match.");
}

}  // namespace

void draw_lora_interpretation_limits() {
    if (!ImGui::CollapsingHeader("Interpretation limits")) return;
    ImGui::BulletText("Packet rows are SF/BW/polarity hypotheses; a reception groups hypotheses from one capture. Neither is a device.");
    ImGui::BulletText("LoRaWAN fields are structural: MIC not verified, nothing decrypted, addresses are what the air carried.");
    ImGui::BulletText("RF clusters match IRR/DC/IQ imbalance (not CFO). Heuristic; review labels do not change matching.");
    ImGui::BulletText("Listening is sampled in finite windows; packets crossing a window edge or between windows are missed.");
    ImGui::BulletText("Rules only use evidence that missed frames cannot fabricate; a quiet incident list is not proof of no attack.");
    ImGui::BulletText("Interoperability evidence covers tested setups only (SX1262 SF7/BW125 explicit header).");
}

void draw_lora_security_panel(const lora_security::Snapshot& s) {
    if (!ImGui::CollapsingHeader("Capture coverage and evidence", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextWrapped("Latest capture: %s", s.health.c_str());
    char b[64];
    TileRow row;
    std::snprintf(b, sizeof b, "%llu", (unsigned long long)s.captures);
    stat_tile("Captures", b, "Captures submitted to the evidence monitor (each derived channel counts once).");
    row.next();
    std::snprintf(b, sizeof b, "%.1f s", s.usable_s);
    stat_tile("Usable exposure", b, "Seconds of IQ usable by every supported decoder hypothesis.");
    row.next();
    std::snprintf(b, sizeof b, "%llu", (unsigned long long)s.eligible);
    stat_tile("Eligible frames", b, "CRC-valid explicit-PHY frames usable as protocol input.", &kGood);
    row.next();
    std::snprintf(b, sizeof b, "%llu", (unsigned long long)s.crc_failed);
    stat_tile("CRC failed", b, nullptr, s.crc_failed ? &kBad : nullptr);
    row.next();
    std::snprintf(b, sizeof b, "%llu", (unsigned long long)(s.input_loss + s.rejected_inputs));
    stat_tile("Lost / rejected", b, "Capture batches lost to a full queue plus rejected inputs.",
              s.input_loss + s.rejected_inputs ? &kCaution : nullptr);
    row.next();
    std::snprintf(b, sizeof b, "%.1f s", s.dead_s);
    stat_tile("Unsampled time", b, "Device time between captures that was never sampled.");
    if (ImGui::TreeNode("All counters")) {
        ImGui::Text("Captures: %llu | Excluded: %llu | Device-timed: %llu", (unsigned long long)s.captures,
                    (unsigned long long)s.excluded, (unsigned long long)s.device_timed);
        ImGui::Text("Sampled: %.3f s | Analysed: %.3f s | Usable supported-PHY exposure: %.3f s", s.sampled_s, s.analysed_s, s.usable_s);
        ImGui::Text("Events: %llu | Eligible protocol inputs: %llu | Failed: %llu | CRC absent: %llu | Partial: %llu",
                    (unsigned long long)s.events, (unsigned long long)s.eligible, (unsigned long long)s.crc_failed,
                    (unsigned long long)s.crc_absent, (unsigned long long)s.partial);
        ImGui::Text("Duplicate hypotheses: %llu | Lost capture batches: %llu | Rejected inputs: %llu",
                    (unsigned long long)s.hypotheses_deduplicated, (unsigned long long)s.input_loss,
                    (unsigned long long)s.rejected_inputs);
        ImGui::Text("Unsampled device time: %.3f s | Unknown gaps: %llu | Time regressions: %llu", s.dead_s,
                    (unsigned long long)s.unknown_gaps, (unsigned long long)s.time_regressions);
        ImGui::TextWrapped("Capture coverage applies to supported decoder hypotheses (explicit PHY SF7-12, BW125/250/500 kHz); "
                           "it does not establish packet recall. CRC-valid bytes do not authenticate LoRaWAN MICs or identify a physical device.");
        ImGui::TreePop();
    }
    if (!s.coverage.empty() && ImGui::BeginTable("lora_security_coverage", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (const char* label : {"Frequency MHz", "Rate ksps", "Gain dB", "Sampled s", "Usable s"}) ImGui::TableSetupColumn(label);
        ImGui::TableHeadersRow();
        for (const auto& [key, c] : s.coverage) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%.3f", c.center_hz / 1e6);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f", c.rate_hz / 1e3);
            ImGui::TableSetColumnIndex(2);
            if (c.gain_db) ImGui::Text("%.1f", *c.gain_db); else ImGui::TextUnformatted("AGC/unknown");
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.3f", c.sampled_s);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.3f", c.usable_s);
        }
        ImGui::EndTable();
    }
    if (!s.recording_error.empty()) ImGui::TextColored(kBad, "Recording error: %s", s.recording_error.c_str());
    if (s.captures)
        ImGui::TextDisabled("Last capture: #%llu | %.3f MHz | %.1f ksps | session %llu", (unsigned long long)s.latest.capture_seq,
                            s.latest.center_hz / 1e6, s.latest.rate_hz / 1e3, (unsigned long long)s.latest.radio_session);
}

void draw_lora_packet_table(const std::vector<LoraPacketRow>& packets, float height) {
    if (packets.empty()) {
        ImGui::TextDisabled("No LoRa packets observed yet.");
        return;
    }

    struct View {
        ImGuiTextFilter filter;
        bool crc_only = false, decoded_only = false;
        int mode = 0;  // 0 receptions, 1 all hypotheses
        std::optional<LoraPacketRow> selected;
        std::set<std::string> expanded;
        std::string export_message;
    };
    static std::map<ImGuiID, View> views;
    auto& view = views[ImGui::GetID("LoRa retained view")];
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
    view.filter.Draw("Search", 240);
    ImGui::SameLine();
    ImGui::RadioButton("Receptions", &view.mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("All hypotheses", &view.mode, 1);
    ImGui::SameLine();
    ImGui::Checkbox("Header decoded", &view.decoded_only);
    ImGui::SameLine();
    ImGui::Checkbox("CRC valid only", &view.crc_only);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear filters")) { view.filter.Clear(); view.crc_only = view.decoded_only = false; }

    std::vector<const LoraPacketRow*> shown;  // newest first, after filters
    for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
        const auto& d = *it;
        std::string search = d.time + " " + d.status + " " + d.decoder + " " + d.reception_id + " " + d.payload_hex + " " +
                             std::to_string(d.sf) + " " + std::to_string(d.freq_mhz) + " " + d.lorawan_device_id.value_or("") +
                             " " + d.payload_repr.value_or("");
        if (!view.filter.PassFilter(search.c_str())) continue;
        if (view.crc_only && d.crc != LoraPacketRow::Crc::Valid) continue;
        if (view.decoded_only && !d.header_valid) continue;
        shown.push_back(&d);
    }
    ImGui::SameLine();
    std::string& msg = view.export_message;
    auto filtered = [&] {
        std::vector<LoraPacketRow> rows;
        for (auto it = shown.rbegin(); it != shown.rend(); ++it) rows.push_back(**it);
        return rows;
    };
    export_button("Export CSV", "lora-packets", "csv", [&] { return lora_export::packets_csv(filtered()); }, msg);
    ImGui::SameLine();
    export_button("Export JSON", "lora-packets", "json", [&] { return lora_export::packets_json(filtered()); }, msg);
    if (!msg.empty()) ImGui::TextDisabled("%s", msg.c_str());

    size_t headers = 0, verified_count = 0, partial = 0, failed = 0, ungrouped = 0;
    std::set<std::string> reception_groups;
    bool grouping_limited = false;
    for (const auto& row : packets) {
        headers += row.header_valid;
        if (row.reception_id.empty()) ++ungrouped; else reception_groups.insert(row.reception_id);
        grouping_limited = grouping_limited || row.reception_grouping_limited;
        verified_count += row.crc == LoraPacketRow::Crc::Valid;
        partial += row.header_valid && !row.payload_complete;
        failed += row.crc == LoraPacketRow::Crc::Failed;
    }
    ImGui::Text("Retained observations: %zu | Valid headers: %zu | CRC valid: %zu | Partial: %zu | CRC failed: %zu",
                packets.size(), headers, verified_count, partial, failed);
    ImGui::Text("Retained reception groups: %zu | Ungrouped rows: %zu", reception_groups.size(), ungrouped);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Conservative capture-local grouping. Groups are not devices or a packet-delivery count.\n"
                          "CRC-valid rows are retained separately from failed/partial rows, so undecodable bursts never push decoded packets out.");
    if (grouping_limited) ImGui::TextWrapped("Grouping comparison limit reached: unmatched hypotheses were kept separate.");

    // Receptions view: groups in newest-first order, best hypothesis first.
    struct Group { std::string key; std::vector<const LoraPacketRow*> rows; };
    std::vector<Group> groups;
    if (view.mode == 0) {
        std::map<std::string, size_t> index;
        for (const auto* r : shown) {
            if (r->reception_id.empty()) { groups.push_back({"", {r}}); continue; }
            auto [it, fresh] = index.try_emplace(r->reception_id, groups.size());
            if (fresh) groups.push_back({r->reception_id, {}});
            groups[it->second].rows.push_back(r);
        }
        for (auto& g : groups)
            std::stable_sort(g.rows.begin(), g.rows.end(), [](const auto* a, const auto* b) { return rank(*a) > rank(*b); });
    }

    static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable |
                                   ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_ScrollX;
    ImVec2 outer_size(0.0f, view.selected ? std::min(height, 240.f) : height);
    if (!ImGui::BeginTable("lora_packets", 33, flags, outer_size)) return;

    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 105.0f);
    ImGui::TableSetupColumn("Decode outcome", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableSetupColumn("Decoder", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 235.0f);
    ImGui::TableSetupColumn("Header", ImGuiTableColumnFlags_WidthFixed, 85.0f);
    ImGui::TableSetupColumn("Freq (MHz)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("SF", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("BW guess (kHz)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("CR", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("LDRO", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 55.0f);
    ImGui::TableSetupColumn("Sync observed", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 100.0f);
    ImGui::TableSetupColumn("Header len", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 85.0f);
    ImGui::TableSetupColumn("Payload CRC", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("CFO (bins)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90.0f);
    // RF fingerprint (fingerprint.hpp) Tier-1 parameters and fit-quality
    // covariates; SNR doubles as the gate-reason column.
    ImGui::TableSetupColumn("SNR (dB)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 110.0f);
    ImGui::TableSetupColumn("EVM (%)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90.0f);
    ImGui::TableSetupColumn("Sync corr", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90.0f);
    ImGui::TableSetupColumn("CFO (ppm)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90.0f);
    ImGui::TableSetupColumn("IRR (dB)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
    ImGui::TableSetupColumn("IQ eps", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
    ImGui::TableSetupColumn("IQ phi (deg)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 100.0f);
    ImGui::TableSetupColumn("DC (dBc)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
    ImGui::TableSetupColumn("DC ang (deg)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 100.0f);
    ImGui::TableSetupColumn("Payload", ImGuiTableColumnFlags_WidthFixed, 360.0f);
    ImGui::TableSetupColumn("IQ polarity", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 95.0f);
    ImGui::TableSetupColumn("Preamble peak", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 105.0f);
    ImGui::TableSetupColumn("SFD peak", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90.0f);
    ImGui::TableSetupColumn("CFO estimate (Hz)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 130.0f);
    ImGui::TableSetupColumn("Drift (Hz/symbol)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 135.0f);
    ImGui::TableSetupColumn("Capture offset (s)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 135.0f);
    ImGui::TableSetupColumn("FEC mismatches", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 120.0f);
    ImGui::TableSetupColumn("LoRaWAN structure", ImGuiTableColumnFlags_WidthFixed, 220.0f);
    ImGui::TableSetupColumn("Reception group", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 260);
    ImGui::TableSetupColumn("RF cluster", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableHeadersRow();

    int id = 0;
    if (view.mode == 1) {
        for (const auto* r : shown) draw_packet_row(*r, false, 1, nullptr, view.selected, id++);
    } else {
        for (const auto& g : groups) {
            bool open = !g.key.empty() && view.expanded.count(g.key);
            bool toggled = open;
            draw_packet_row(*g.rows.front(), false, g.rows.size(), g.key.empty() ? nullptr : &toggled, view.selected, id++);
            if (toggled != open) {
                if (toggled) view.expanded.insert(g.key); else view.expanded.erase(g.key);
            }
            if (open)
                for (size_t k = 1; k < g.rows.size(); ++k) draw_packet_row(*g.rows[k], true, 1, nullptr, view.selected, id++);
        }
        if (view.expanded.size() > 512) view.expanded.clear();
    }
    ImGui::EndTable();
    if (view.selected) {
        const auto& d = *view.selected;
        ImGui::SeparatorText("Selected LoRa hypothesis snapshot");
        ImGui::TextWrapped("%s | %s | %s | CRC: %s", d.time.c_str(), d.status.c_str(), d.decoder.c_str(), lora_crc_label(d.crc));
        ImGui::TextWrapped("%s", d.detail.c_str());
        if (!d.reception_id.empty())
            ImGui::TextWrapped("Capture-local reception: %s | %zu source hypotheses (before GUI tail retention)",
                               d.reception_id.c_str(), d.reception_hypotheses);
        if (d.rf_cluster_id) ImGui::TextWrapped("RF cluster: %s (heuristic fingerprint match)", cluster_label(*d.rf_cluster_id).c_str());
        if (d.fp_gate_reason) ImGui::TextWrapped("RF fingerprint gate: %s", d.fp_gate_reason->c_str());
        if (ImGui::Button("Close hypothesis details")) view.selected.reset();
        else {
            if (ImGui::CollapsingHeader("PHY, payload and decoder evidence", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Frequency %.17g MHz | SF %d | BW %.17g kHz | CFO %d bins", d.freq_mhz, d.sf, d.bandwidth_khz, d.cfo_bins);
                ImGui::Text("Header valid: %s | Payload complete: %s | LDRO ambiguous: %s | Analysis limited: %s",
                            d.header_valid ? "yes" : "no", d.payload_complete ? "yes" : "no", d.ldro_ambiguous ? "yes" : "no",
                            d.analysis_limited ? "yes" : "no");
                auto field = [](const char* name, const auto& value) {
                    if (value) {
                        std::ostringstream out;
                        out << std::setprecision(17) << *value;
                        ImGui::TextWrapped("%s: %s", name, out.str().c_str());
                    } else ImGui::TextDisabled("%s: unavailable", name);
                };
                field("CR code", d.cr); field("Header length bytes", d.payload_len); field("CRC validity", d.crc_valid);
                field("LDRO", d.ldro); field("Sync observation", d.sync_word); field("Sync check skipped (lab)", d.sync_check_skipped);
                field("Inverted IQ", d.inverted_iq);
                field("Preamble peak ratio", d.preamble_peak_ratio); field("SFD peak ratio", d.sfd_peak_ratio); field("CFO Hz", d.cfo_hz);
                field("Drift Hz/symbol", d.drift_hz_per_symbol); field("Capture offset s", d.capture_offset_s);
                field("FEC disagreements", d.fec_disagreements);
                field("Decoder sample start", d.decoder_sample_start); field("Aligned data start (BW-rate samples)", d.decoder_data_start);
                field("Decoder sample end", d.decoder_sample_end);
                ImGui::Text("Start uncertainty: %zu samples", d.start_uncertainty_samples);
                ImGui::TextWrapped("Payload hex: %s", d.payload_hex.c_str());
                field("Payload text", d.payload_repr); field("LoRaWAN candidate", d.lorawan_candidate);
                field("LoRaWAN detail (MIC unverified)", d.lorawan_detail);
            }
            if (ImGui::CollapsingHeader("Complete RF fingerprint measurements")) {
                auto field = [](const char* name, const std::optional<double>& v) {
                    if (v) ImGui::Text("%s: %.17g", name, *v); else ImGui::TextDisabled("%s: unavailable", name);
                };
                field("CFO ppm", d.fp_cfo_ppm); field("IRR dB", d.fp_irr_db); field("IQ epsilon", d.fp_iq_eps);
                field("IQ phase degrees", d.fp_iq_phi_deg); field("DC dBc", d.fp_dc_dbc); field("DC angle degrees", d.fp_dc_ang_deg);
                field("SNR dB", d.fp_snr_db); field("EVM percent", d.fp_evm_pct); field("Sync correlation", d.fp_sync_corr);
            }
        }
    }
}

void draw_lora_device_table(const std::vector<LoraDeviceRow>& devices, const lora_rules::Snapshot& incidents,
                            const std::vector<lora_master::LoraMasterRow>& emitters, float height) {
    ImGui::TextDisabled("End devices named by CRC-valid LoRaWAN frames this session. Structural: MIC not verified.");
    if (devices.empty()) {
        ImGui::TextDisabled("No LoRaWAN addresses decoded yet.");
        return;
    }
    static ImGuiTextFilter filter;
    static std::string message;
    filter.Draw("Search##lora_devices", 240);
    ImGui::SameLine();
    std::vector<LoraDeviceRow> rows;
    for (const auto& d : devices)
        if (filter.PassFilter((d.kind + " " + d.id + " " + d.last_type).c_str())) rows.push_back(d);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.last_seen_ts > b.last_seen_ts; });
    export_button("Export CSV##devices", "lorawan-devices", "csv", [&] { return lora_export::devices_csv(rows); }, message);
    if (!message.empty()) ImGui::TextDisabled("%s", message.c_str());
    std::map<int, std::string> alias;
    for (const auto& e : emitters)
        if (!e.enrolment.alias.empty()) alias[e.device_id] = e.enrolment.alias;
    std::map<std::string, size_t> open;
    for (const auto& i : incidents.incidents) if (i.open) ++open[i.subject];

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Hideable;
    if (!ImGui::BeginTable("lorawan_devices", 12, flags, ImVec2(0, height))) return;
    ImGui::TableSetupScrollFreeze(2, 1);
    for (const char* c : {"Kind", "Address", "Frames", "Up / down", "Last FCnt", "First seen", "Last seen", "Freq (MHz)",
                          "SF / BW", "Last frame", "RF clusters", "Open incidents"})
        ImGui::TableSetupColumn(c, ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (const auto& d : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.kind.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.id.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.frames);
        ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", (unsigned long long)d.uplinks, (unsigned long long)d.downlinks);
        ImGui::TableNextColumn();
        if (d.last_fcnt16) ImGui::Text("%u", unsigned(*d.last_fcnt16)); else ImGui::TextDisabled("--");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lower 16 bits of the frame counter, last data frame.");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(d.first_seen_ts).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(d.last_seen_ts).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.4f", d.last_freq_hz / 1e6);
        ImGui::TableNextColumn(); ImGui::Text("SF%d / %.0f", d.last_sf, d.last_bw_hz / 1e3);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.last_type.c_str());
        ImGui::TableNextColumn();
        if (d.rf_clusters.empty()) ImGui::TextDisabled("--");
        else {
            std::string s;
            for (int c : d.rf_clusters) {
                s += (s.empty() ? "" : ", ") + cluster_label(c);
                if (alias.count(c)) s += " (" + alias[c] + ")";
            }
            ImGui::TextUnformatted(s.c_str());
            if (d.rf_clusters.size() > 1 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Heard from more than one RF cluster: a spoofed address or a split cluster.");
        }
        ImGui::TableNextColumn();
        const size_t n = open.count(d.kind + " " + d.id) ? open[d.kind + " " + d.id] : 0;
        if (n) ImGui::TextColored(kBad, "%zu", n); else ImGui::TextDisabled("0");
    }
    ImGui::EndTable();
}

void draw_lora_master_table(const std::vector<lora_master::LoraMasterRow>& rows, const std::vector<LoraDeviceRow>& devices,
                            const LoraTabActions& actions, float height, const std::string& storage_error) {
    if (!storage_error.empty()) ImGui::TextColored(kBad, "%s", storage_error.c_str());
    if (rows.empty()) {
        ImGui::TextDisabled("No master LoRa emitters recorded yet.");
        return;
    }
    static std::optional<int> selected;
    static std::string message, export_message;
    static ImGuiTextFilter filter;
    filter.Draw("Search##lora_emitters", 240);
    ImGui::SameLine();
    std::vector<const lora_master::LoraMasterRow*> shown;
    std::map<int, std::vector<std::string>> addresses;
    for (const auto& d : devices)
        for (int c : d.rf_clusters) addresses[c].push_back(d.kind + " " + d.id);
    for (const auto& r : rows) {
        std::string search = r.device_id_str + " " + r.enrolment.alias + " " + r.enrolment.policy + " " + r.enrolment.note;
        for (const auto& a : addresses[r.device_id]) search += " " + a;
        if (filter.PassFilter(search.c_str())) shown.push_back(&r);
    }
    export_button("Export CSV##emitters", "lora-emitters", "csv", [&] { return lora_export::emitters_csv(rows); }, export_message);
    if (!export_message.empty()) ImGui::TextDisabled("%s", export_message.c_str());
    size_t unreviewed = 0;
    for (const auto& r : rows) unreviewed += r.enrolment.policy == "unreviewed";
    ImGui::TextDisabled("%zu clusters, %zu unreviewed. Select an ID to review, merge or see its drift history.", rows.size(), unreviewed);

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_ScrollX;
    ImVec2 outer_size(0.0f, selected ? std::min(height, 220.f) : height);
    if (ImGui::BeginTable("lora_master", 14, flags, outer_size)) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Device ID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Alias", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Review", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("First seen", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 190.0f);
        ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Readings", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Freq (MHz)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("SF", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("BW (kHz)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("IRR (dB)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
        ImGui::TableSetupColumn("IQ eps", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
        ImGui::TableSetupColumn("IQ phi (deg)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 100.0f);
        ImGui::TableSetupColumn("DC (dBc)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
        ImGui::TableSetupColumn("LoRaWAN addresses", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();
        std::time_t now = std::time(nullptr);
        for (const auto* rp : shown) {
            const auto& row = *rp;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(row.device_id_str.c_str(), selected && *selected == row.device_id)) selected = row.device_id;
            ImGui::TableNextColumn();
            if (row.enrolment.alias.empty()) ImGui::TextDisabled("--"); else ImGui::TextUnformatted(row.enrolment.alias.c_str());
            if (!row.enrolment.note.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.enrolment.note.c_str());
            ImGui::TableNextColumn();
            const auto& p = row.enrolment.policy;
            if (p == "approved") ImGui::TextColored(kGood, "Approved");
            else if (p == "disallowed") ImGui::TextColored(kBad, "Disallowed");
            else ImGui::TextColored(kCaution, "Unreviewed");
            ImGui::TableNextColumn();
            {
                std::time_t fs = std::time_t(row.first_seen_ts);
                std::tm tm_buf{};
                localtime_r(&fs, &tm_buf);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
                ImGui::Text("%s (%.1fd ago)", buf, double(now - row.first_seen_ts) / 86400.0);
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(row.last_seen_ts).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", row.reading_count, lora_master::MAX_READINGS_PER_DEVICE);
            ImGui::TableNextColumn(); ImGui::Text("%.4f", row.last_freq_hz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%d", row.last_sf);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", row.last_bw_hz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", row.latest.irr_db);
            ImGui::TableNextColumn(); ImGui::Text("%.4f", row.latest.iq_eps);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", row.latest.iq_phi_deg);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", row.latest.dc_dbc);
            ImGui::TableNextColumn();
            const auto& a = addresses[row.device_id];
            if (a.empty()) ImGui::TextDisabled("--");
            else {
                std::string s;
                for (const auto& x : a) s += (s.empty() ? "" : ", ") + x;
                ImGui::TextUnformatted(s.c_str());
            }
        }
        ImGui::EndTable();
    }

    if (!selected) return;
    auto it = std::find_if(rows.begin(), rows.end(), [](const auto& r) { return r.device_id == *selected; });
    if (it == rows.end()) { selected.reset(); return; }
    const auto& row = *it;
    const auto& f = row.latest;
    ImGui::SeparatorText(("Cluster " + row.device_id_str).c_str());
    ImGui::TextWrapped("First seen %s | last seen %s | %d retained readings. Provisional cluster matched on IRR/DC/IQ imbalance, "
                       "not physical-device authentication.",
                       clock_time(double(row.first_seen_ts)).c_str(), clock_time(double(row.last_seen_ts)).c_str(), row.reading_count);
    if (ImGui::Button("Close cluster details")) { selected.reset(); return; }

    // Review label editor; buffers reload when the selection changes.
    static int editing = -1;
    static char alias[65], note[513];
    static int policy = 0;
    const char* policies[] = {"unreviewed", "approved", "disallowed"};
    if (editing != row.device_id) {
        editing = row.device_id;
        std::snprintf(alias, sizeof alias, "%s", row.enrolment.alias.c_str());
        std::snprintf(note, sizeof note, "%s", row.enrolment.note.c_str());
        policy = row.enrolment.policy == "approved" ? 1 : row.enrolment.policy == "disallowed" ? 2 : 0;
        message.clear();
    }
    if (ImGui::CollapsingHeader("Review", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(220);
        ImGui::InputText("Alias", alias, sizeof alias);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::Combo("Policy", &policy, "Unreviewed\0Approved\0Disallowed\0");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Disallowed: every later fingerprint match raises a LoRa incident. Labels never change matching.");
        ImGui::TextDisabled("Note");
        ImGui::InputTextMultiline("##note", note, sizeof note, ImVec2(-1, 44));
        ImGui::BeginDisabled(!actions.set_enrolment);
        if (ImGui::Button("Save review"))
            message = actions.set_enrolment(row.device_id, policies[policy], alias, note) ? "Review saved."
                                                                                          : "Review not saved (see storage error).";
        ImGui::EndDisabled();
        if (!message.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", message.c_str()); }

        static int merge_from = -1;
        static bool confirm = false;
        std::vector<const lora_master::LoraMasterRow*> others;
        for (const auto& r : rows) if (r.device_id != row.device_id) others.push_back(&r);
        if (!others.empty()) {
            std::string preview = "Choose a cluster";
            for (const auto* o : others) if (o->device_id == merge_from) preview = o->device_id_str;
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("Merge into this cluster", preview.c_str())) {
                for (const auto* o : others) {
                    std::string label = o->device_id_str + (o->enrolment.alias.empty() ? "" : " (" + o->enrolment.alias + ")") +
                                        "  " + std::to_string(o->reading_count) + " readings";
                    if (ImGui::Selectable(label.c_str(), merge_from == o->device_id)) { merge_from = o->device_id; confirm = false; }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Same emitter (cannot be undone)", &confirm);
            ImGui::SameLine();
            ImGui::BeginDisabled(!actions.merge || merge_from < 0 || !confirm);
            if (ImGui::Button("Merge")) {
                message = actions.merge(row.device_id, merge_from) ? "Merged " + cluster_label(merge_from) + "." : "Merge failed.";
                merge_from = -1;
                confirm = false;
            }
            ImGui::EndDisabled();
        }
    }

    if (ImGui::CollapsingHeader("Fingerprint drift history", ImGuiTreeNodeFlags_DefaultOpen)) {
        static int history_id = -1, history_count = -1;
        static double history_at = 0;
        static std::vector<lora_master::LoraMasterReading> history;
        const double t = ImGui::GetTime();
        if (actions.history && (history_id != row.device_id || history_count != row.reading_count || t - history_at > 5)) {
            history = actions.history(row.device_id);
            history_id = row.device_id;
            history_count = row.reading_count;
            history_at = t;
        }
        if (history.empty()) ImGui::TextDisabled("No reading history available.");
        else {
            static std::string history_message;
            export_button("Export history CSV", ("lora-emitter-" + std::to_string(row.device_id)).c_str(), "csv",
                          [&] { return lora_export::history_csv(history); }, history_message);
            if (!history_message.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", history_message.c_str()); }
            ImGui::TextDisabled("%zu readings, oldest left. Line: values; text: median [min, max].", history.size());
            const float plot_w = std::max(200.f, (ImGui::GetContentRegionAvail().x - 8) * 0.5f);
            auto plot = [&](const char* name, auto get) {
                std::vector<float> v;
                v.reserve(history.size());
                for (const auto& r : history) v.push_back(float(get(r)));
                std::vector<float> sorted = v;
                std::sort(sorted.begin(), sorted.end());
                char overlay[96];
                std::snprintf(overlay, sizeof overlay, "%s  %.4g [%.4g, %.4g]", name, sorted[sorted.size() / 2], sorted.front(),
                              sorted.back());
                ImGui::PlotLines((std::string("##") + name).c_str(), v.data(), int(v.size()), 0, overlay, FLT_MAX, FLT_MAX,
                                 ImVec2(plot_w, 64));
            };
            plot("IRR dB", [](const auto& r) { return r.irr_db; });
            ImGui::SameLine();
            plot("DC dBc", [](const auto& r) { return r.dc_dbc; });
            plot("IQ epsilon", [](const auto& r) { return r.iq_eps; });
            ImGui::SameLine();
            plot("IQ phase deg", [](const auto& r) { return r.iq_phi_deg; });
            plot("CFO ppm (not matched)", [](const auto& r) { return r.cfo_ppm; });
            ImGui::SameLine();
            plot("SNR dB", [](const auto& r) { return r.snr_db; });
        }
    }
    if (ImGui::CollapsingHeader("Complete latest reading")) {
        ImGui::Text("Reading Unix seconds: %lld | Frequency %.17g Hz | SF %d | BW %.17g Hz", (long long)f.ts, f.freq_hz, f.sf, f.bw_hz);
        ImGui::Text("CFO %.17g ppm | IRR %.17g dB | IQ epsilon %.17g | IQ phase %.17g deg", f.cfo_ppm, f.irr_db, f.iq_eps, f.iq_phi_deg);
        ImGui::Text("DC %.17g dBc | DC angle %.17g deg | SNR %.17g dB", f.dc_dbc, f.dc_ang_deg, f.snr_db);
        ImGui::Text("EVM %.17g percent | Sync correlation %.17g", f.evm_pct, f.sync_corr);
    }
}

void draw_lora_incidents(const lora_rules::Snapshot& s, const LoraTabActions& actions, float height) {
    size_t open = 0;
    for (const auto& i : s.incidents) open += i.open;
    ImGui::Text("Open incidents: %zu | Total retained: %zu", open, s.incidents.size());
    ImGui::SameLine();
    ImGui::TextDisabled("| Frames checked: %llu (data %llu, join %llu, retransmissions %llu, not LoRaWAN %llu)",
                        (unsigned long long)s.stats.frames, (unsigned long long)s.stats.data_frames,
                        (unsigned long long)s.stats.join_requests, (unsigned long long)s.stats.retransmissions,
                        (unsigned long long)s.stats.ignored);
    if (ImGui::TreeNode("Rules")) {
        ImGui::BulletText("%s: identical bytes again more than %.0f s later.", lora_rules::RULE_REPLAY, lora_rules::RETRANSMIT_WINDOW_S);
        ImGui::BulletText("%s: FCnt moved back by less than half the 16-bit space.", lora_rules::RULE_FCNT_REGRESSION);
        ImGui::BulletText("%s.", lora_rules::RULE_FCNT_REUSE);
        ImGui::BulletText("%s: a DevEUI repeated a join nonce.", lora_rules::RULE_DEVNONCE_REUSE);
        ImGui::BulletText("%s: %zu or more join requests within %.0f s.", lora_rules::RULE_JOIN_FLOOD, lora_rules::JOIN_FLOOD_COUNT,
                          lora_rules::JOIN_FLOOD_WINDOW_S);
        ImGui::BulletText("%s: after %zu cluster-tagged frames, %.0f%% from one cluster.", lora_rules::RULE_RF_MISMATCH,
                          lora_rules::RF_BASELINE_MIN, lora_rules::RF_BASELINE_SHARE * 100);
        ImGui::BulletText("%s: a cluster you marked disallowed matched again.", lora_rules::RULE_DISALLOWED);
        ImGui::TextWrapped("Frame-counter gaps are never reported: sampled listening misses frames, which only hides evidence. "
                           "Keys are not used; a reset device can look like a replay.");
        ImGui::TreePop();
    }
    static bool open_only = true;
    static std::string message;
    ImGui::Checkbox("Open only", &open_only);
    ImGui::SameLine();
    ImGui::BeginDisabled(!actions.clear_closed || open == s.incidents.size());
    if (ImGui::SmallButton("Clear acknowledged")) actions.clear_closed();
    ImGui::EndDisabled();
    ImGui::SameLine();
    export_button("Export CSV##incidents", "lora-incidents", "csv", [&] { return lora_export::incidents_csv(s.incidents); }, message);
    if (!message.empty()) ImGui::TextDisabled("%s", message.c_str());
    if (s.incidents.empty()) {
        ImGui::TextDisabled("No LoRa incidents.");
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    if (!ImGui::BeginTable("lora_incidents", 8, flags, ImVec2(0, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40);
    ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn("Rule", ImGuiTableColumnFlags_WidthFixed, 260);
    ImGui::TableSetupColumn("Subject", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch, 420);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableHeadersRow();
    for (const auto& i : s.incidents) {
        if (open_only && !i.open) continue;
        ImGui::TableNextRow();
        ImGui::PushID(int(i.id));
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)i.id);
        ImGui::TableNextColumn();
        ImGui::TextColored(i.severity == "High" ? kBad : i.severity == "Medium" ? kCaution : kDim, "%s", i.severity.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(i.rule.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(i.subject.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)i.count);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock_time(i.last_ts).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("First: %s", clock_time(i.first_ts).c_str());
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", i.summary.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i.evidence.c_str());
        ImGui::TableNextColumn();
        if (i.open) {
            ImGui::BeginDisabled(!actions.acknowledge);
            if (ImGui::SmallButton("Acknowledge")) actions.acknowledge(i.id);
            ImGui::EndDisabled();
        } else ImGui::TextDisabled("Acknowledged");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void draw_lora_receiver_panel(const LoraReceiverView& v, const LoraTabActions& actions) {
    if (v.locked_hz)
        ImGui::TextColored(kCaution, "Frequency lock active at %.4f MHz: the channel plan below is paused until the lock is released.",
                           *v.locked_hz / 1e6);
    ImGui::SeparatorText("Channel plan");
    static char buffer[1024] = "";
    static std::vector<double> shown_channels;
    static std::string error;
    if (shown_channels != v.channels) {
        shown_channels = v.channels;
        std::snprintf(buffer, sizeof buffer, "%s", lora_plan::format_channels_mhz(v.channels).c_str());
        error.clear();
    }
    const auto& presets = lora_plan::presets();
    std::string current = "Custom";
    for (const auto& p : presets)
        if (lora_plan::normalise(p.channels_hz) == v.channels) current = p.name;
    ImGui::SetNextItemWidth(260);
    ImGui::BeginDisabled(!actions.set_channels);
    if (ImGui::BeginCombo("Preset", current.c_str())) {
        for (const auto& p : presets) {
            if (ImGui::Selectable(p.name, current == p.name)) actions.set_channels(p.channels_hz);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s MHz", p.note, lora_plan::format_channels_mhz(p.channels_hz).c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-90);
    const bool enter = ImGui::InputText("##lora_channels", buffer, sizeof buffer, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Apply") || enter) {
        auto parsed = lora_plan::parse_channels_mhz(buffer, &error);
        if (!parsed.empty()) { actions.set_channels(parsed); error.clear(); }
    }
    ImGui::EndDisabled();
    if (!error.empty()) ImGui::TextColored(kBad, "%s", error.c_str());
    else ImGui::TextDisabled("Channel centres in MHz, comma separated (up to %zu).", lora_plan::MAX_CHANNELS);

    bool wideband = v.wideband;
    ImGui::BeginDisabled(!actions.set_wideband);
    if (ImGui::Checkbox("Wideband listening (receive nearby channels in one capture)", &wideband)) actions.set_wideband(wideband);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: one wider capture covers several channels, each derived at 500 kHz and decoded in parallel,\n"
                          "so every channel in it is heard for the whole window. Off: one channel per cycle.");
    {
        const std::pair<const char*, std::optional<double>> widths[] = {
            {"20 MHz (same as Wi-Fi / drones)", 20e6}, {"10 MHz", 10e6}, {"8 MHz", 8e6}, {"4 MHz", 4e6},
            {"Auto (narrowest that fits)", std::nullopt}};
        const char* current = "Custom";
        for (const auto& [label, rate] : widths) if (rate == v.wideband_rate_hz) current = label;
        ImGui::SetNextItemWidth(260);
        ImGui::BeginDisabled(!actions.set_wideband_rate || !v.wideband);
        if (ImGui::BeginCombo("Capture width", current)) {
            for (const auto& [label, rate] : widths)
                if (ImGui::Selectable(label, rate == v.wideband_rate_hz)) actions.set_wideband_rate(rate);
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("20 MHz covers about 15 MHz of usable band in one capture. If the capture also keeps 863-868 MHz,\n"
                              "it replaces the separate 5 Msps energy scan. Long capture windows fall back to a narrower width\n"
                              "(at most 125 M samples per capture).");
        if (v.replaces_energy_scan)
            ImGui::TextColored(kGood, "This capture also feeds the energy map; the separate sub-GHz energy scan is skipped.");
        else if (v.wideband && !v.locked_hz)
            ImGui::TextDisabled("The separate sub-GHz energy scan still runs each cycle (plan does not fit one capture keeping 863-868 MHz).");
    }

    if (!v.plan.empty() && ImGui::BeginTable("lora_plan", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (const char* c : {"Capture", "Mode", "Tuned (MHz)", "Rate (Msps)", "Usable span (MHz)", "Channels (MHz)", "DC clearance"})
            ImGui::TableSetupColumn(c);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < v.plan.size(); ++i) {
            const auto& c = v.plan[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%zu / %zu", i + 1, v.plan.size());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.wideband ? "Wideband" : "Single");
            ImGui::TableNextColumn(); ImGui::Text("%.4f", c.center_hz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", c.rate_hz / 1e6);
            ImGui::TableNextColumn();
            if (c.wideband) {
                const double h = lora_plan::usable_half_span(c.rate_hz);
                ImGui::Text("%.2f - %.2f", (c.center_hz - h) / 1e6, (c.center_hz + h) / 1e6);
            } else ImGui::TextDisabled("one channel");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(lora_plan::format_channels_mhz(c.channels_hz).c_str());
            ImGui::TableNextColumn();
            if (c.wideband) ImGui::Text("%.0f kHz", c.dc_clearance_hz / 1e3); else ImGui::TextDisabled("centred");
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Each cycle takes the next capture in this list; capture window %.1f s.", v.capture_seconds);

    ImGui::SeparatorText("Measured listening");
    const auto& st = v.stats;
    if (!st.captures) ImGui::TextDisabled("No LoRa listen since the plan last changed.");
    else {
        ImGui::Text("Last capture: %s %.4f MHz @ %.2f Msps, %zu channel(s) | %.2f s IQ | %.2f s decode after capture",
                    st.last_wideband ? "wideband" : "single", st.last_center_hz / 1e6, st.last_rate_hz / 1e6, st.last_channels,
                    st.last_capture_s, st.last_decode_s);
        const double wall = std::max(1e-9, v.now_steady_s - st.first_listen_steady_s);
        if (ImGui::BeginTable("lora_listen_share", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            for (const char* c : {"Channel (MHz)", "IQ captured", "Share of wall time"}) ImGui::TableSetupColumn(c);
            ImGui::TableHeadersRow();
            for (double ch : v.channels) {
                const auto it = st.channel_listened_s.find(ch);
                const double s = it == st.channel_listened_s.end() ? 0 : it->second;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%.4f", ch / 1e6);
                ImGui::TableNextColumn(); ImGui::Text("%.1f s", s);
                ImGui::TableNextColumn();
                ImGui::ProgressBar(float(std::min(1.0, s / wall)), ImVec2(160, 0));
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Share = seconds of IQ captured on the channel / wall time since the first listen (%.0f s). "
                            "Time spent on other bands counts against it.", wall);
    }

    ImGui::SeparatorText("Suggested channels");
    if (v.suggestions.empty()) ImGui::TextDisabled("No LoRa-like energy outside the plan in the RF observations.");
    else if (ImGui::BeginTable("lora_suggest", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (const char* c : {"Frequency (MHz)", "Bandwidth (kHz)", "Peak (dB, relative)", "Hits", ""}) ImGui::TableSetupColumn(c);
        ImGui::TableHeadersRow();
        for (const auto& s : v.suggestions) {
            ImGui::TableNextRow();
            ImGui::PushID(int(s.freq_hz / 1e3));
            ImGui::TableNextColumn(); ImGui::Text("%.4f", s.freq_hz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", s.bandwidth_hz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", s.power_db);
            ImGui::TableNextColumn(); ImGui::Text("%d", s.hits);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!actions.set_channels || v.channels.size() >= lora_plan::MAX_CHANNELS);
            if (ImGui::SmallButton("Add to plan")) {
                auto next = v.channels;
                next.push_back(s.freq_hz);
                actions.set_channels(next);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Energy shape only (orange rows in RF observations); a suggestion is not a decoded LoRa packet.");
}

void draw_lora_replay_panel() {
    if (!ImGui::CollapsingHeader("Offline LoRa capture replay")) return;
    static char directory[1024] = "";
    struct Result { std::vector<LoraPacketRow> rows; std::string description; };
    static std::future<Result> pending;
    static Result result;
    static std::string error;
    if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try { result = pending.get(); error.clear(); }
        catch (const std::exception& e) { error = e.what(); }
    }
    ImGui::TextWrapped("Replay is offline. Results stay separate from live packets and persistent emitter records. Enter a saved capture directory.");
    ImGui::InputText("Capture directory", directory, sizeof(directory));
    static bool laboratory_mode = false;
    ImGui::Checkbox("Legacy laboratory codecs (nonstandard)##replay", &laboratory_mode);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Experimental legacy codecs only. Disable for standard over-the-air decoding.");
    ImGui::BeginDisabled(pending.valid() || directory[0] == '\0');
    if (ImGui::Button("Replay capture")) {
        std::string path(directory);
        bool skip = laboratory_mode;
        result = {};
        error.clear();
        pending = std::async(std::launch::async, [path, skip] {
            auto c = load_lora_capture(path);
            Result r;
            r.rows = analyze_lora_capture(c.iq, c.sample_rate_hz, c.requested_center_hz, skip);
            if (!skip) annotate_lora_receptions(r.rows, c.sample_rate_hz, "offline:" + path);
            for (auto& row : r.rows) {
                row.time = "Replay";
                if (c.overflow) row.detail += " Capture overflow: sample continuity lost.";
            }
            r.description = path + " | " + std::to_string(c.iq.size()) + " samples | " + std::to_string(c.sample_rate_hz) +
                            " samples/s | source: " + c.source + " | " + std::to_string(r.rows.size()) + " hypothesis results";
            r.description += "\nRequested center: " + std::to_string(c.requested_center_hz / 1e6) +
                             " MHz | host capture-call Unix seconds: " + std::to_string(c.host_start_unix_s) +
                             " | device: " + c.device_args + " | antenna: " + c.antenna;
            r.description += "\nRequested gain: " + (c.requested_gain_db ? std::to_string(*c.requested_gain_db) + " dB" : "AGC") +
                             " | requested duration: " + std::to_string(c.requested_duration_s) + " s";
            if (c.iq.size() + 1 < c.requested_duration_s * c.sample_rate_hz) r.description += " | WARNING: capture shorter than requested";
            if (c.overflow) r.description += " | WARNING: overflow/discontinuous IQ";
            return r;
        });
    }
    ImGui::EndDisabled();
    if (pending.valid()) ImGui::TextUnformatted("Analyzing capture...");
    if (!error.empty()) ImGui::TextWrapped("Replay failed: %s", error.c_str());
    if (!result.description.empty()) {
        ImGui::TextWrapped("%s", result.description.c_str());
        ImGui::PushID("replay");
        draw_lora_packet_table(result.rows, 220);
        ImGui::PopID();
    }
}

}  // namespace rfmon
