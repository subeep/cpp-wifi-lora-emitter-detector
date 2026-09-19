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
#include "lora_gui.hpp"
#include "registry.hpp"
#include "scanner.hpp"
#include "wifi_master.hpp"
#include "wifi_gui.hpp"

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

            // Diagnostic-only escape hatch: the SX-reference decoder
            // currently rejects every real (non-loopback) capture at a
            // sync-word value check whose assumption (SYNC_WORD_DEFAULT
            // = 0x12) was only ever validated against this app's own
            // TX, never independently against third-party hardware -
            // see lora_phy_std.hpp's demodulate() comment. This lets a
            // user bypass that one gate to see what the header decode
            // (and its own, independent checksum) says regardless, off
            // by default so it never silently weakens the normal
            // integrity story. Rows produced this way are labeled in
            // the packet table (see draw_lora_packet_table()) so a
            // "Valid" header is never mistaken for a fully sync-word-
            // verified one.
            static bool lora_skip_sync_check = false;
            if (ImGui::Checkbox("Skip sync-word check (diagnostic)", &lora_skip_sync_check)) {
                scanner.set_lora_skip_sync_check(lora_skip_sync_check);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Off by default. Bypasses the sync-word value gate so the header decode "
                    "(and its own checksum) is visible even when the sync word doesn't match - "
                    "useful for diagnosing real hardware, but a header shown this way is NOT "
                    "cross-checked against the sync word. Rows produced with this on are marked "
                    "in the table.");
            }
        }

        if (active_band == BAND_SUB_GHZ) {
            if (ImGui::CollapsingHeader("Save a LoRa IQ capture")) {
                static char save_directory[1024] = PROJECT_ROOT_DIR "/data/lora_captures";
                ImGui::InputText("Save under", save_directory, sizeof(save_directory));
                ImGui::BeginDisabled(!scanner.status().connected || scanner.lora_capture_pending() || !save_directory[0]);
                if (ImGui::Button("Save next completed capture")) scanner.request_lora_capture_save(save_directory);
                ImGui::EndDisabled();
                ImGui::TextWrapped("One capture per click, including recordings with no detections. IQ and settings are saved together. Requires a connected receiver.");
                auto message = scanner.lora_capture_message();
                if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
            }
            draw_lora_replay_panel();
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
                "FCS-valid DSSS and legacy OFDM beacons/probe responses show decoded identities. "
                "Sampled, not exhaustive: only the channel currently being swept is heard.");
            float wifi_packets_height = ImGui::GetContentRegionAvail().y * 0.5f;
            draw_wifi_packet_table(scanner.wifi_packets(), wifi_packets_height);

            ImGui::Spacing();
            ImGui::TextUnformatted("Wi-Fi Identities & RF Clusters (persistent)");
            ImGui::TextDisabled("Click an identity for details. Orange = decoded BSSID; gray = provisional RF cluster. Ch = monitored; AP ch = advertised.");
            ImGui::TextDisabled("Identity details survive restart. RF history retains the latest ~1000 accepted readings per entry.");
            const auto wifi_storage_error = scanner.wifi_storage_error();
            if (!wifi_storage_error.empty()) ImGui::TextColored(ImVec4(1,.5f,.3f,1), "%s", wifi_storage_error.c_str());
            draw_wifi_master_table(scanner.wifi_master_snapshot(), ImGui::GetContentRegionAvail().y);
        }

        if (show_lora_packets) {
            ImGui::Spacing();
            ImGui::TextUnformatted("LoRa PHY packets (Sub-GHz IN865 channels)");
            ImGui::TextDisabled(
                "Each row is an SF/BW hypothesis, not a unique packet or identity. Hover the outcome for details. "
                "CRC validates bytes under the named decoder; real-radio interoperability remains unverified.");
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
