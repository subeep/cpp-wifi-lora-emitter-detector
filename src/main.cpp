// rf_monitor_gui - live B210 LoRa/Wi-Fi(2.4/5GHz) emitter monitor, ImGui UI.
//
// Energy detection only (see README.md) - frequency, bandwidth and
// power are measured directly; protocol is a bandwidth-based guess,
// not a decode. Switching the band mode below changes which scan plan
// the background Scanner thread is actively running and which of the
// three per-band device lists is shown; each band keeps its own list
// so switching away and back doesn't lose it.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "config.hpp"
#include "fingerprint.hpp"
#include "lora_master.hpp"
#include "registry.hpp"
#include "scanner.hpp"
#include "wifi_master.hpp"

using namespace rfmon;

namespace {

void GlfwErrorCallback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// LoRa-like -> orange, WiFi-like -> green, narrowband guess -> yellow, else grey.
ImVec4 category_color(const std::string& protocol) {
    if (protocol.rfind("LoRa-like", 0) == 0) return ImVec4(0.94f, 0.55f, 0.24f, 1.0f);
    if (protocol.rfind("WiFi-like", 0) == 0) return ImVec4(0.25f, 0.73f, 0.31f, 1.0f);
    if (protocol.find("narrowband") != std::string::npos) return ImVec4(0.82f, 0.60f, 0.13f, 1.0f);
    return ImVec4(0.55f, 0.55f, 0.58f, 1.0f);
}

struct BandRange {
    std::string label;
    double lo_mhz;
    double hi_mhz;
};

std::vector<BandRange> track_ranges_for_band(const std::string& band) {
    if (band == BAND_SUB_GHZ) return {{"Sub-GHz ISM (LoRa/IN865): 863-868 MHz", 863.0, 868.0}};
    if (band == BAND_WIFI_2G4) return {{"2.4GHz ISM (Wi-Fi): 2401-2484 MHz", 2401.0, 2484.0}};
    return {
        {"5GHz UNII-1 (ch 36-48): 5170-5250 MHz", 5170.0, 5250.0},
        {"5GHz UNII-3 (ch 149-165): 5735-5845 MHz", 5735.0, 5845.0},
    };
}

void draw_frequency_track(const BandRange& range, const std::vector<DeviceRow>& devices) {
    ImGui::TextUnformatted(range.label.c_str());
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    float height = 30.0f;
    draw_list->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), IM_COL32(26, 34, 51, 255),
                              4.0f);

    double span = range.hi_mhz - range.lo_mhz;
    for (const auto& d : devices) {
        if (d.freq_mhz < range.lo_mhz || d.freq_mhz > range.hi_mhz) continue;
        float frac = float((d.freq_mhz - range.lo_mhz) / span);
        float x = p0.x + frac * width;
        ImVec4 c = category_color(d.protocol_guess);
        ImU32 col = ImGui::ColorConvertFloat4ToU32(c);
        draw_list->AddRectFilled(ImVec2(x - 4, p0.y + 2), ImVec2(x + 4, p0.y + height - 2), col,
                                  2.0f);
    }
    ImGui::Dummy(ImVec2(width, height + 6));
}

// Nearest numbered Wi-Fi channel, or 0 for bands that have no channel
// numbering (sub-GHz), where the column renders as "--".
int device_row_channel(const DeviceRow& d) {
    if (d.band != BAND_WIFI_2G4 && d.band != BAND_WIFI_5G) return 0;
    const auto& channels = (d.band == BAND_WIFI_2G4) ? wifi_2g4_channels() : wifi_5g_channels();
    double freq_hz = d.freq_mhz * 1e6;
    int best_ch = channels.begin()->first;
    double best_dist = std::abs(channels.begin()->second - freq_hz);
    for (const auto& [ch, f] : channels) {
        double dist = std::abs(f - freq_hz);
        if (dist < best_dist) {
            best_dist = dist;
            best_ch = ch;
        }
    }
    return best_ch;
}

void draw_device_table(const std::vector<DeviceRow>& devices, float height,
                        const std::map<int, int>& source_counts) {
    if (devices.empty()) {
        ImGui::TextDisabled("No active emitters right now.");
        return;
    }

    static ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                   ImGuiTableFlags_ScrollY;
    ImVec2 outer_size(0.0f, height);
    if (!ImGui::BeginTable("devices", 10, flags, outer_size)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Freq (MHz)",
                            ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed,
                            100.0f);
    ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("BW (kHz)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Protocol guess", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Sources", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Power (dB, rel.)", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("Cycles seen", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Age (s)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Last seen (s ago)", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableSetupColumn("Band", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableHeadersRow();

    std::vector<DeviceRow> rows = devices;
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsCount > 0) {
            int col = specs->Specs[0].ColumnIndex;
            bool asc = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            std::sort(rows.begin(), rows.end(), [&](const DeviceRow& a, const DeviceRow& b) {
                bool less;
                switch (col) {
                    case 0: less = a.freq_mhz < b.freq_mhz; break;
                    case 1: less = device_row_channel(a) < device_row_channel(b); break;
                    case 2: less = a.bandwidth_khz < b.bandwidth_khz; break;
                    case 3: less = a.protocol_guess < b.protocol_guess; break;
                    case 4: less = a.freq_mhz < b.freq_mhz; break;  // Sources: no stable key, fall back
                    case 5: less = a.power_db < b.power_db; break;
                    case 6: less = a.hit_count < b.hit_count; break;
                    case 7: less = a.age_s < b.age_s; break;
                    case 8: less = a.last_seen_s_ago < b.last_seen_s_ago; break;
                    default: less = a.band < b.band; break;
                }
                return asc ? less : !less;
            });
            specs->SpecsDirty = false;
        }
    }

    for (const auto& d : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", d.freq_mhz);
        ImGui::TableNextColumn();
        {
            int ch = device_row_channel(d);
            if (ch > 0) ImGui::Text("%d", ch);
            else ImGui::TextDisabled("--");
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", d.bandwidth_khz);
        ImGui::TableNextColumn();
        ImGui::TextColored(category_color(d.protocol_guess), "%s", d.protocol_guess.c_str());
        ImGui::TableNextColumn();
        {
            // Inferred lower bound from beacon-cadence clustering, not a
            // decoded device count - see Scanner::wifi_source_counts().
            int ch = device_row_channel(d);
            auto it = source_counts.find(ch);
            if (ch > 0 && it != source_counts.end() && it->second > 0) {
                ImGui::Text(">=%d", it->second);
            } else {
                ImGui::TextDisabled("--");
            }
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", d.power_db);
        ImGui::TableNextColumn();
        ImGui::Text("%d", d.hit_count);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", d.age_s);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", d.last_seen_s_ago);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(d.band == BAND_SUB_GHZ    ? "Sub-GHz ISM"
                                : d.band == BAND_WIFI_2G4 ? "2.4GHz Wi-Fi"
                                                          : "5GHz Wi-Fi");
    }
    ImGui::EndTable();
}

// Permanent, cross-run LoRa identity list (see lora_master.hpp) -
// deliberately a separate table from draw_device_table() above: that
// one is a generic multi-band energy-detection view (including
// narrowband/"Unknown" segments with no fingerprint at all), while
// this one only ever shows devices with real accepted fingerprint
// history, kept forever across restarts rather than for one session.
void draw_lora_master_table(const std::vector<lora_master::LoraMasterRow>& rows, float height) {
    if (rows.empty()) {
        ImGui::TextDisabled("No master LoRa emitters recorded yet.");
        return;
    }

    static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_ScrollX;
    ImVec2 outer_size(0.0f, height);
    if (!ImGui::BeginTable("lora_master", 11, flags, outer_size)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Device ID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("First seen", ImGuiTableColumnFlags_WidthFixed, 190.0f);
    ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Readings", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Freq (MHz)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("SF", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("BW (kHz)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IRR (dB)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IQ eps", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IQ phi (deg)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("DC (dBc)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableHeadersRow();

    // Already sorted by last_seen_ts (most recent first) by
    // LoraMasterList::snapshot() itself - not re-sortable by column
    // here, unlike draw_device_table(), since "last seen" ordering is
    // the whole point of this particular table.
    std::time_t now = std::time(nullptr);
    for (const auto& row : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.device_id_str.c_str());

        ImGui::TableNextColumn();
        {
            std::time_t fs = std::time_t(row.first_seen_ts);
            std::tm tm_buf{};
            localtime_r(&fs, &tm_buf);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
            double age_days = double(now - row.first_seen_ts) / 86400.0;
            ImGui::Text("%s (%.1fd ago)", buf, age_days);
        }

        ImGui::TableNextColumn();
        {
            double ago_s = double(now - row.last_seen_ts);
            if (ago_s < 120.0) ImGui::Text("%.0fs ago", ago_s);
            else if (ago_s < 7200.0) ImGui::Text("%.1fm ago", ago_s / 60.0);
            else if (ago_s < 172800.0) ImGui::Text("%.1fh ago", ago_s / 3600.0);
            else ImGui::Text("%.1fd ago", ago_s / 86400.0);
        }

        ImGui::TableNextColumn();
        ImGui::Text("%d / %d", row.reading_count, lora_master::MAX_READINGS_PER_DEVICE);
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", row.last_freq_hz / 1e6);
        ImGui::TableNextColumn();
        ImGui::Text("%d", row.last_sf);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", row.last_bw_hz / 1e3);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", row.latest.irr_db);
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", row.latest.iq_eps);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", row.latest.iq_phi_deg);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", row.latest.dc_dbc);
    }
    ImGui::EndTable();
}

// Individually detected Wi-Fi transmissions, most recent first - one
// row per burst found by wifi::detect_bursts() and classified on its
// own window. Detection only: no PLCP/payload decode, so there is
// deliberately no rate, length or MAC address here.
void draw_wifi_packet_table(const std::vector<WifiPacketRow>& packets, float height) {
    if (packets.empty()) {
        ImGui::TextDisabled("No Wi-Fi packets detected yet.");
        return;
    }

    static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_ScrollX;
    ImVec2 outer_size(0.0f, height);
    if (!ImGui::BeginTable("wifi_packets", 15, flags, outer_size)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Freq (MHz)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("Modulation", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Power (dB)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("BW (MHz)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Duration (us)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Confidence", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    // RF fingerprint (see wifi_fingerprint.hpp) - only populated once a
    // burst clears the bandwidth gate and (for DSSS) is beacon-shaped;
    // "--" means not attempted, the gate reason means attempted but
    // rejected. IRR/IQ eps/IQ phi are additionally always blank on a
    // DSSS row - see WifiPacketRow's own comment on why that's
    // structural, not a gate outcome.
    ImGui::TableSetupColumn("CFO (ppm)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("IRR (dB)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IQ eps", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IQ phi (deg)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("DC (dBc)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("EVM (%)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Sync corr", ImGuiTableColumnFlags_WidthFixed, 90.0f);
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

    // Most recent first - the log itself is appended chronologically.
    for (size_t i = packets.size(); i-- > 0;) {
        const auto& p = packets[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.time.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", p.freq_mhz);
        ImGui::TableNextColumn();
        ImGui::Text("%d", p.channel);
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
}

// Permanent, cross-run Wi-Fi identity list (see wifi_master.hpp) - the
// Wi-Fi analog of draw_lora_master_table() below. "Device" is either a
// real decoded MAC/BSSID (currently only from a decoded DSSS beacon)
// shown in the accent color, or a WIFI-FP-#### fingerprint cluster
// otherwise - see that file's header for why MAC is the primary key
// here rather than porting LoRa's fingerprint-is-the-identity model.
void draw_wifi_master_table(const std::vector<wifi_master::WifiMasterRow>& rows, float height) {
    if (rows.empty()) {
        ImGui::TextDisabled("No master Wi-Fi emitters recorded yet.");
        return;
    }

    static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_ScrollX;
    ImVec2 outer_size(0.0f, height);
    if (!ImGui::BeginTable("wifi_master", 12, flags, outer_size)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthFixed, 140.0f);
    ImGui::TableSetupColumn("First seen", ImGuiTableColumnFlags_WidthFixed, 190.0f);
    ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Readings", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("PHY", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Freq (MHz)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("CFO (ppm)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("IRR (dB)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IQ eps", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IQ phi (deg)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("DC (dBc)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("EVM (%)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableHeadersRow();

    const ImVec4 mac_color(0.94f, 0.55f, 0.24f, 1.0f);  // matches the LoRa-like map legend accent
    const ImVec4 fp_color(0.75f, 0.75f, 0.78f, 1.0f);

    // Already sorted by last_seen_ts (most recent first) by
    // WifiMasterList::snapshot() itself - same reasoning as
    // draw_lora_master_table()'s identical choice.
    std::time_t now = std::time(nullptr);
    for (const auto& row : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(row.key_is_mac ? mac_color : fp_color, "%s", row.device_key.c_str());

        ImGui::TableNextColumn();
        {
            std::time_t fs = std::time_t(row.first_seen_ts);
            std::tm tm_buf{};
            localtime_r(&fs, &tm_buf);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
            double age_days = double(now - row.first_seen_ts) / 86400.0;
            ImGui::Text("%s (%.1fd ago)", buf, age_days);
        }

        ImGui::TableNextColumn();
        {
            double ago_s = double(now - row.last_seen_ts);
            if (ago_s < 120.0) ImGui::Text("%.0fs ago", ago_s);
            else if (ago_s < 7200.0) ImGui::Text("%.1fm ago", ago_s / 60.0);
            else if (ago_s < 172800.0) ImGui::Text("%.1fh ago", ago_s / 3600.0);
            else ImGui::Text("%.1fd ago", ago_s / 86400.0);
        }

        ImGui::TableNextColumn();
        ImGui::Text("%d / %d", row.reading_count, wifi_master::MAX_READINGS_PER_DEVICE);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.last_phy.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", row.last_channel_hz / 1e6);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", row.latest.cfo_ppm);
        ImGui::TableNextColumn();
        // 0.0 on a DSSS row means "not identifiable", not "measured
        // zero" - see wifi_fingerprint.cpp's own comment on why a
        // real-valued BPSK reference can't resolve gain/phase
        // imbalance separately from DC offset.
        if (row.last_phy == "DSSS") ImGui::TextDisabled("n/a");
        else ImGui::Text("%.2f", row.latest.irr_db);
        ImGui::TableNextColumn();
        if (row.last_phy == "DSSS") ImGui::TextDisabled("n/a");
        else ImGui::Text("%.4f", row.latest.iq_eps);
        ImGui::TableNextColumn();
        if (row.last_phy == "DSSS") ImGui::TextDisabled("n/a");
        else ImGui::Text("%.2f", row.latest.iq_phi_deg);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", row.latest.dc_dbc);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", row.latest.evm_pct);
    }
    ImGui::EndTable();
}

// "Decoded" = full payload recovered, header checksum matched.
// "Detected" = a real chirp preamble locked but the header/payload
// didn't fully decode (e.g. third-party hardware whose exact FEC/
// interleaver encoding isn't reverse-engineered yet - see
// lora_phy.hpp). Most-recent-first, matching the Python dashboard.
void draw_lora_packet_table(const std::vector<LoraPacketRow>& packets, float height) {
    if (packets.empty()) {
        ImGui::TextDisabled("No LoRa packets observed yet.");
        return;
    }

    static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_ScrollX;
    ImVec2 outer_size(0.0f, height);
    if (!ImGui::BeginTable("lora_packets", 19, flags, outer_size)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Freq (MHz)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("SF", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("BW (kHz)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("CR", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("Len", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupColumn("CRC", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("CFO (bins)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    // --- RF fingerprint (see fingerprint.hpp) - Tier-1 "stable core"
    // parameters, only populated on a "Detected" row (see
    // run_lora_listen_step()). SNR doubles as the gate-reason column
    // when extraction was rejected, since that's the most common gate.
    ImGui::TableSetupColumn("SNR (dB)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    // Fit-quality covariates (source spec §5/§1.6) - what the
    // LORA_EVM_CEILING_PCT/LORA_SYNC_CORR_FLOOR gate uses to reject a
    // wrong-hypothesis match that still had plenty of raw SNR behind
    // it. Shown even on a gated row when they were actually computed.
    ImGui::TableSetupColumn("EVM (%)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Sync corr", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("CFO (ppm)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("IRR (dB)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IQ eps", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IQ phi (deg)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("DC (dBc)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("DC ang (deg)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Payload", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    ImVec4 dim(0.55f, 0.55f, 0.58f, 1.0f);
    ImVec4 good(0.25f, 0.73f, 0.31f, 1.0f);
    ImVec4 bad(0.85f, 0.30f, 0.28f, 1.0f);

    for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
        const auto& d = *it;
        bool decoded = d.status == "decoded";
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(d.time.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(decoded ? good : dim, "%s", decoded ? "Decoded" : "Detected");
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", d.freq_mhz);
        ImGui::TableNextColumn();
        ImGui::Text("%d", d.sf);
        ImGui::TableNextColumn();
        ImGui::Text("%.0f", d.bandwidth_khz);
        ImGui::TableNextColumn();
        if (d.cr.has_value()) ImGui::Text("%d", *d.cr); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.payload_len.has_value()) ImGui::Text("%d", *d.payload_len); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.crc_valid.has_value()) {
            ImGui::TextColored(*d.crc_valid ? good : bad, "%s", *d.crc_valid ? "valid" : "invalid");
        } else {
            ImGui::TextDisabled("--");
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", d.cfo_bins);
        ImGui::TableNextColumn();
        if (d.fp_snr_db.has_value()) {
            ImGui::Text("%.1f", *d.fp_snr_db);
        } else if (d.fp_gate_reason.has_value()) {
            ImGui::TextColored(bad, "%s", d.fp_gate_reason->c_str());
        } else {
            ImGui::TextDisabled("--");
        }
        ImGui::TableNextColumn();
        if (d.fp_evm_pct.has_value()) {
            bool bad_evm = *d.fp_evm_pct > fingerprint::LORA_EVM_CEILING_PCT;
            ImGui::TextColored(bad_evm ? bad : good, "%.2f", *d.fp_evm_pct);
        } else {
            ImGui::TextDisabled("--");
        }
        ImGui::TableNextColumn();
        if (d.fp_sync_corr.has_value()) {
            bool bad_sync = *d.fp_sync_corr < fingerprint::LORA_SYNC_CORR_FLOOR;
            ImGui::TextColored(bad_sync ? bad : good, "%.3f", *d.fp_sync_corr);
        } else {
            ImGui::TextDisabled("--");
        }
        ImGui::TableNextColumn();
        if (d.fp_cfo_ppm.has_value()) ImGui::Text("%.3f", *d.fp_cfo_ppm); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.fp_irr_db.has_value()) ImGui::Text("%.2f", *d.fp_irr_db); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.fp_iq_eps.has_value()) ImGui::Text("%.5f", *d.fp_iq_eps); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.fp_iq_phi_deg.has_value()) ImGui::Text("%.3f", *d.fp_iq_phi_deg); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.fp_dc_dbc.has_value()) ImGui::Text("%.1f", *d.fp_dc_dbc); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.fp_dc_ang_deg.has_value()) ImGui::Text("%.1f", *d.fp_dc_ang_deg); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.payload_repr.has_value()) ImGui::TextUnformatted(d.payload_repr->c_str());
        else ImGui::TextDisabled("--");
    }
    ImGui::EndTable();
}

}  // namespace

int main() {
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "RF Monitor", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing worth persisting across runs yet
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Deliberately not auto-started here - the user picks a device and
    // clicks Connect in the UI (see the device selector below) rather
    // than the app silently connecting to whatever the default happens
    // to be the instant the window appears.
    Scanner scanner;
    bool scanner_started = false;

    int selftest_frames = -1;
    if (const char* env = std::getenv("RF_MONITOR_GUI_SELFTEST_FRAMES")) {
        int parsed = std::atoi(env);
        if (parsed > 0) selftest_frames = parsed;
    }

    float threshold_db = float(DEFAULT_DETECTION_THRESHOLD_DB);
    bool agc = false;
    float gain_db = float(DEFAULT_GAIN_DB);

    int frame_count = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& cur_io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(cur_io.DisplaySize);
        ImGui::Begin("RF Monitor", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        // --- SDR device selector ---
        // Picking a device only changes which radio button is
        // highlighted - it does NOT connect by itself. Switching means
        // physically different hardware (USB vs Ethernet, different
        // daughterboard/gain range - see config.hpp's DeviceProfile),
        // so this fully restarts the scan thread, and doing that
        // silently the instant a radio button is clicked was a bit
        // surprising - Connect is a separate, explicit action.
        static const std::vector<std::pair<const char*, SdrDeviceType>> devices = {
            {"USRP B210", SdrDeviceType::B210},
            {"USRP X310", SdrDeviceType::X310},
        };
        static SdrDeviceType selected_device = SdrDeviceType::B210;
        for (size_t i = 0; i < devices.size(); ++i) {
            bool selected = selected_device == devices[i].second;
            if (i > 0) ImGui::SameLine();
            if (ImGui::RadioButton(devices[i].first, selected)) {
                selected_device = devices[i].second;
            }
        }
        ImGui::SameLine();
        bool is_connected_to_selected = scanner_started && scanner.device_type() == selected_device;
        if (ImGui::Button(scanner_started ? "Reconnect" : "Connect")) {
            if (scanner_started) scanner.stop();
            scanner.set_device_type(selected_device);
            DeviceProfile new_profile = device_profile(selected_device);
            agc = false;
            gain_db = float(new_profile.default_gain_db);
            scanner.set_gain(gain_db);
            scanner.start();
            scanner_started = true;
        }
        if (scanner_started) {
            ImGui::SameLine();
            if (ImGui::Button("Disconnect")) {
                scanner.stop();
                scanner_started = false;
            }
        }
        if (is_connected_to_selected) {
            ImGui::SameLine();
            ImGui::TextDisabled("(currently running)");
        }
        DeviceProfile profile = device_profile(selected_device);

        ImGui::Separator();

        ScannerStatus status = scanner.status();
        std::string active_band = scanner.active_band();

        // --- Status line ---
        if (!scanner_started) {
            ImGui::TextDisabled("Select a device above and click Connect to start scanning.");
        } else if (!status.connected) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                                "Not connected%s%s", status.error.empty() ? "" : ": ",
                                status.error.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.25f, 0.73f, 0.31f, 1), "Connected");
            ImGui::SameLine();
            ImGui::Text("| step: %s | cycles: %d", status.active_step_label.c_str(),
                        status.cycle_count);
            if (status.last_overflow) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.1f, 1), "| USB overflow last cycle");
            }
            if (status.rx_stalled) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                                    "| RX STALLED - no samples received recently");
            }
        }

        ImGui::Separator();

        // --- Band mode selector ---
        static const std::vector<std::pair<const char*, std::string>> modes = {
            {"LoRa (Sub-GHz)", BAND_SUB_GHZ},
            {"Wi-Fi 2.4GHz", BAND_WIFI_2G4},
            {"Wi-Fi 5GHz", BAND_WIFI_5G},
        };
        for (size_t i = 0; i < modes.size(); ++i) {
            bool selected = active_band == modes[i].second;
            if (i > 0) ImGui::SameLine();
            if (ImGui::RadioButton(modes[i].first, selected)) {
                scanner.set_active_band(modes[i].second);
                active_band = modes[i].second;
            }
        }

        // --- LoRa frequency lock (only meaningful in Sub-GHz mode) ---
        // The 4 LORA_LISTEN_CHANNELS_HZ channels are cycled one per
        // cycle by default, so a real device on some other frequency
        // (see newrocktest/TARANGMINI_LORA_FINDINGS.md) is only listened
        // to ~1 cycle in 4. Locking pins every cycle to one exact
        // frequency instead, for watching one known device continuously.
        static bool lora_lock_enabled = false;
        static float lora_lock_mhz = 866.9f;
        if (active_band == BAND_SUB_GHZ) {
            ImGui::Spacing();
            if (ImGui::Checkbox("Lock to frequency", &lora_lock_enabled)) {
                scanner.set_lora_lock_freq(lora_lock_enabled
                                                ? std::optional<double>(double(lora_lock_mhz) * 1e6)
                                                : std::optional<double>());
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputFloat("MHz##lora_lock", &lora_lock_mhz, 0.0f, 0.0f, "%.4f") &&
                lora_lock_enabled) {
                scanner.set_lora_lock_freq(double(lora_lock_mhz) * 1e6);
            }
            if (lora_lock_enabled) {
                ImGui::SameLine();
                ImGui::TextDisabled("(listening only here instead of cycling all 4 channels)");
            }
        }

        ImGui::Spacing();

        // --- Controls ---
        if (ImGui::SliderFloat("Threshold (dB above noise floor)", &threshold_db, 3.0f, 30.0f,
                                "%.1f")) {
            scanner.set_threshold_db(threshold_db);
        }
        if (profile.supports_agc) {
            ImGui::SameLine();
            if (ImGui::Checkbox("AGC", &agc)) {
                scanner.set_gain(agc ? std::optional<double>() : std::optional<double>(gain_db));
            }
        } else {
            ImGui::SameLine();
            ImGui::BeginDisabled();
            bool agc_unavailable = false;
            ImGui::Checkbox("AGC (not supported on this radio)", &agc_unavailable);
            ImGui::EndDisabled();
        }
        if (!agc) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderFloat("Gain (dB)", &gain_db, 0.0f, float(profile.max_gain_db),
                                    "%.1f")) {
                scanner.set_gain(gain_db);
            }
        }

        ImGui::Separator();

        // --- Frequency map + table for the currently selected band ---
        ImGui::TextUnformatted("Frequency map");
        for (const auto& range : track_ranges_for_band(active_band)) {
            draw_frequency_track(range, scanner.snapshot(active_band));
        }
        auto legend_item = [](const char* id, ImVec4 color, const char* text) {
            ImGui::ColorButton(id, color,
                                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop |
                                    ImGuiColorEditFlags_NoBorder,
                                ImVec2(12, 12));
            ImGui::SameLine(0, 4);
            ImGui::TextUnformatted(text);
            ImGui::SameLine(0, 16);
        };
        legend_item("##legend_lora", ImVec4(0.94f, 0.55f, 0.24f, 1), "LoRa-like");
        legend_item("##legend_wifi", ImVec4(0.25f, 0.73f, 0.31f, 1), "WiFi-like");
        legend_item("##legend_narrow", ImVec4(0.82f, 0.60f, 0.13f, 1), "Narrowband (BLE/Zigbee?)");
        legend_item("##legend_unknown", ImVec4(0.55f, 0.55f, 0.58f, 1), "Unknown");
        ImGui::NewLine();

        ImGui::Spacing();
        ImGui::TextUnformatted("Active emitters");

        bool show_lora_packets = (active_band == BAND_SUB_GHZ);
        bool show_wifi_packets = (active_band == BAND_WIFI_2G4 || active_band == BAND_WIFI_5G);
        float remaining = ImGui::GetContentRegionAvail().y;
        // Split remaining space three ways when the two LoRa-only
        // tables are shown below it, in half when the Wi-Fi packet list
        // is, else give it all to the generic energy-detection list.
        float device_table_height = remaining;
        if (show_lora_packets) device_table_height = remaining * 0.3f;
        else if (show_wifi_packets) device_table_height = remaining * 0.45f;
        draw_device_table(scanner.snapshot(active_band), device_table_height,
                           scanner.wifi_source_counts(active_band));

        if (show_wifi_packets) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Detected Wi-Fi packets");
            ImGui::TextDisabled(
                "One row per individually detected burst, classified on its own window. "
                "Detection only - no header/payload decode, so no rate or MAC address. "
                "Sampled, not exhaustive: only the channel currently being swept is heard.");
            float wifi_packets_height = ImGui::GetContentRegionAvail().y * 0.5f;
            draw_wifi_packet_table(scanner.wifi_packets(), wifi_packets_height);

            ImGui::Spacing();
            ImGui::TextUnformatted("Wi-Fi Master Emitters (persistent across restarts)");
            ImGui::TextDisabled(
                "Every accepted fingerprint reading ever recorded for a device - kept forever "
                "until manually deleted. Orange = a real decoded MAC/BSSID (currently only from "
                "a decoded DSSS beacon). Gray = a fingerprint cluster (no MAC decoded yet) - "
                "matched on IRR/DC/IQ-imbalance only, not CFO, same placeholder-comparison "
                "caveat as the LoRa list (see docs).");
            draw_wifi_master_table(scanner.wifi_master_snapshot(), ImGui::GetContentRegionAvail().y);
        }

        if (show_lora_packets) {
            ImGui::Spacing();
            ImGui::TextUnformatted("LoRa PHY packets (Sub-GHz IN865 channels)");
            ImGui::TextDisabled(
                "Decoded = full payload recovered. Detected = a real chirp preamble locked but "
                "header/payload didn't fully decode (third-party hardware, see docs).");
            float lora_packets_height = ImGui::GetContentRegionAvail().y * 0.5f;
            draw_lora_packet_table(scanner.lora_packets(), lora_packets_height);

            ImGui::Spacing();
            ImGui::TextUnformatted("LoRa Master Emitters (persistent across restarts)");
            ImGui::TextDisabled(
                "Every accepted fingerprint reading ever recorded for a device - kept forever "
                "until manually deleted. Matched on IRR/DC/IQ-imbalance only, not CFO (see docs) "
                "- a placeholder comparison, not the final model.");
            draw_lora_master_table(scanner.lora_master_snapshot(), ImGui::GetContentRegionAvail().y);
        }

        ImGui::End();
        ImGui::Render();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.043f, 0.059f, 0.078f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        ++frame_count;
        if (selftest_frames > 0 && frame_count >= selftest_frames) break;
    }

    scanner.stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
