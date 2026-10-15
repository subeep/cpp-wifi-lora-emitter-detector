// rf_monitor_gui - live B210 LoRa/Wi-Fi(2.4/5GHz) emitter monitor, ImGui UI.
//
// Native display workspace. Navigation and display state are independent of
// explicit acquisition commands; existing Scanner and add-on ownership is retained.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#ifdef RFMON_ENABLE_DJI_DRONEID
#include "drones/panel.hpp"
#endif
#include "config.hpp"
#include "gui/app_shell.hpp"
#include "fingerprint.hpp"
#include "lora_master.hpp"
#include "lora_gui.hpp"
#include "registry.hpp"
#include "scanner.hpp"
#include "wifi_master.hpp"
#include "wifi_gui.hpp"
#include "security_gui.hpp"
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
#include "cyclostationary/panel.hpp"
#endif

using namespace rfmon;

namespace {

void GlfwErrorCallback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// LoRa-like -> orange, a decoded Wi-Fi AP with a known name (scanner.cpp's
// `"WiFi AP " + bssid + " \"" + ssid + "\""` label, built only when a
// beacon's SSID actually decoded - see run()'s decoded_beacons loop) ->
// green, everything else Wi-Fi (WiFi-like energy-only guess, or a
// decoded AP with a hidden/absent SSID - no name to show) -> white,
// narrowband guess -> yellow, else grey.
ImVec4 category_color(const std::string& protocol) {
    if (protocol.rfind("LoRa-like", 0) == 0) return ImVec4(0.94f, 0.55f, 0.24f, 1.0f);
    if (protocol.rfind("WiFi AP ", 0) == 0 && protocol.find('"') != std::string::npos)
        return ImVec4(0.25f, 0.73f, 0.31f, 1.0f);
    if (protocol.rfind("WiFi-like", 0) == 0 || protocol.rfind("WiFi AP ", 0) == 0)
        return ImVec4(0.92f, 0.92f, 0.94f, 1.0f);
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
        ImGui::TextDisabled("No retained RF observations for this band. This does not establish absence of signals.");
        return;
    }

    static ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
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
    ImGui::TableSetupColumn("Cycles seen", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 90.0f);
    ImGui::TableSetupColumn("Age (s)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
    ImGui::TableSetupColumn("Last seen (s ago)", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableSetupColumn("Band", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 110.0f);
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
                if (asc) return less;
                switch(col) {
                    case 0:case 4:return a.freq_mhz>b.freq_mhz;
                    case 1:return device_row_channel(a)>device_row_channel(b);
                    case 2:return a.bandwidth_khz>b.bandwidth_khz;
                    case 3:return a.protocol_guess>b.protocol_guess;
                    case 5:return a.power_db>b.power_db;
                    case 6:return a.hit_count>b.hit_count;
                    case 7:return a.age_s>b.age_s;
                    case 8:return a.last_seen_s_ago>b.last_seen_s_ago;
                    default:return a.band>b.band;
                }
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

    static std::optional<lora_master::LoraMasterRow> selected;
    ImGui::TextDisabled("Heuristic RF clusters. Select an ID for full latest-reading detail; header menu shows all columns.");
    static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_ScrollX;
    ImVec2 outer_size(0.0f, selected?std::min(height,250.f):height);
    if (!ImGui::BeginTable("lora_master", 11, flags, outer_size)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Device ID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("First seen", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 190.0f);
    ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Readings", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Freq (MHz)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("SF", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("BW (kHz)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IRR (dB)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
    ImGui::TableSetupColumn("IQ eps", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
    ImGui::TableSetupColumn("IQ phi (deg)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 100.0f);
    ImGui::TableSetupColumn("DC (dBc)", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0f);
    ImGui::TableHeadersRow();

    // Already sorted by last_seen_ts (most recent first) by
    // LoraMasterList::snapshot() itself - not re-sortable by column
    // here, unlike draw_device_table(), since "last seen" ordering is
    // the whole point of this particular table.
    std::time_t now = std::time(nullptr);
    for (const auto& row : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if(ImGui::Selectable(row.device_id_str.c_str(),selected && selected->device_id==row.device_id))selected=row;

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
    if(selected) {
        const auto& row=*selected;const auto& f=row.latest;
        ImGui::SeparatorText("Selected LoRa cluster snapshot");
        ImGui::TextWrapped("%s | First/last Unix seconds: %lld / %lld | Retained readings: %d",row.device_id_str.c_str(),(long long)row.first_seen_ts,(long long)row.last_seen_ts,row.reading_count);
        ImGui::TextWrapped("Provisional cluster; matching is based on IRR/DC/IQ imbalance, not physical-device authentication.");
        if(ImGui::Button("Close cluster details"))selected.reset();
        else if(ImGui::CollapsingHeader("Complete latest reading",ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Reading Unix seconds: %lld | Frequency %.17g Hz | SF %d | BW %.17g Hz",(long long)f.ts,f.freq_hz,f.sf,f.bw_hz);
            ImGui::Text("CFO %.17g ppm | IRR %.17g dB | IQ epsilon %.17g | IQ phase %.17g deg",f.cfo_ppm,f.irr_db,f.iq_eps,f.iq_phi_deg);
            ImGui::Text("DC %.17g dBc | DC angle %.17g deg | SNR %.17g dB",f.dc_dbc,f.dc_ang_deg,f.snr_db);
            ImGui::Text("EVM %.17g percent | Sync correlation %.17g",f.evm_pct,f.sync_corr);
        }
    }

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
    gui::configure_font();
    gui::apply_theme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Deliberately not auto-started here - the user picks a device and
    // clicks Connect in the UI (see the device selector below) rather
    // than the app silently connecting to whatever the default happens
    // to be the instant the window appears.
#ifdef RFMON_ENABLE_DJI_DRONEID
    drone::Addon drones; // Outlives scanner and its processing lane.
    Scanner scanner([&](const auto& iq, const auto& record) { drones.observe(iq, record); },
                    [&](const auto& frame) { drones.remote_id.submit(frame); });
#else
    Scanner scanner;
#endif
    bool scanner_started = false;
    // Wi-Fi security monitor recording (opt-in). Applied on the next
    // Connect/Reconnect, because the monitor is configured before start().
    bool record_wifi_security = false;
    std::string wifi_security_recording;  // path in use, "" when off

    int selftest_frames = -1;
    if (const char* env = std::getenv("RF_MONITOR_GUI_SELFTEST_FRAMES")) {
        int parsed = std::atoi(env);
        if (parsed > 0) selftest_frames = parsed;
    }

    float threshold_db = float(DEFAULT_DETECTION_THRESHOLD_DB);
    bool agc = false;
    float gain_db = float(DEFAULT_GAIN_DB);

    gui::State ui;
    bool theme_dirty = false;
    SdrDeviceType selected_device = SdrDeviceType::B210;
    bool lora_laboratory_mode = false;
    int frame_count = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if(theme_dirty){gui::apply_theme(ui.scale,ui.compact);theme_dirty=false;}
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ScannerStatus status = scanner.status();
        std::string active_band = scanner.active_band();
        DeviceProfile profile = device_profile(selected_device);
        const bool drones_available =
#ifdef RFMON_ENABLE_DJI_DRONEID
            true;
#else
            false;
#endif
        const bool cyclo_available =
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
            true;
#else
            false;
#endif
        // UI snapshot cache only; receiver/analysis cadence is unchanged.
        static double last_snapshot = -1;
        static std::map<std::string, std::vector<DeviceRow>> rf_rows;
        static std::map<std::string, std::map<int,int>> source_counts;
        static std::vector<WifiPacketRow> wifi_rows;
        static std::vector<wifi_master::WifiMasterRow> wifi_identities;
        static std::vector<LoraPacketRow> lora_rows;
        static std::vector<lora_master::LoraMasterRow> lora_identities;
        if(last_snapshot < 0 || ImGui::GetTime()-last_snapshot >= .2) {
            for(const auto& band : {BAND_SUB_GHZ,BAND_WIFI_2G4,BAND_WIFI_5G}) {
                rf_rows[band]=scanner.snapshot(band);source_counts[band]=scanner.wifi_source_counts(band);
            }
            wifi_rows=scanner.wifi_packets();wifi_identities=scanner.wifi_master_snapshot();
            lora_rows=scanner.lora_packets();lora_identities=scanner.lora_master_snapshot();
            last_snapshot=ImGui::GetTime();
        }
        const auto security=scanner.wifi_security_snapshot();
        const auto security_queue=scanner.wifi_security_queue_stats();
        const auto run=scanner.wifi_security_run_id();
        auto draw_setup = [&] {
            ImGui::SeparatorText("Monitoring setup — explicit controls");
            gui::note("Choose the receiver and acquisition settings here. Display navigation does not apply settings.");
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
            wifi_security_recording.clear();
            if (record_wifi_security) {
                // One file per connection under data/wifi_security/, bounded
                // to ~64 MB (current + one rotated file). Replay it with
                // build/wifi_security_replay.
                const std::filesystem::path dir = std::filesystem::path(PROJECT_ROOT_DIR) / "data" / "wifi_security";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                std::time_t now = std::time(nullptr);
                std::tm tm{};
                localtime_r(&now, &tm);
                char name[64];
                std::strftime(name, sizeof(name), "wifi-security-%Y%m%d-%H%M%S.ndjson", &tm);
                wifi_security_recording = (dir / name).string();
            }
            scanner.set_wifi_security_recording(wifi_security_recording);
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
        ImGui::SameLine();
        ImGui::Checkbox("Record Wi-Fi security events", &record_wifi_security);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Writes every capture's coverage record and every FCS-valid frame to "
                              "data/wifi_security/ (bounded to ~64 MB). Takes effect on the next Connect.");
        if (scanner_started && record_wifi_security != !wifi_security_recording.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1), "(applies on Reconnect)");
        }
        profile = device_profile(selected_device);
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
                if (active_band != modes[i].second) scanner.set_wifi_fixed_channel(std::nullopt);
                scanner.set_active_band(modes[i].second);
                active_band = modes[i].second;
            }
        }

        // --- Wi-Fi channel: sweep, or stay on one channel ---
        // Fixed-channel monitoring observes one channel near-continuously
        // (security plan, package B) instead of ~4 s per sweep.
        if (active_band == BAND_WIFI_2G4 || active_band == BAND_WIFI_5G) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            const auto& channels = active_band == BAND_WIFI_2G4 ? wifi_2g4_channels() : wifi_5g_channels();
            const std::optional<int> fixed = scanner.wifi_fixed_channel();
            const std::string preview = fixed ? "Channel " + std::to_string(*fixed) : std::string("Sweep all channels");
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::BeginCombo("Wi-Fi channel", preview.c_str())) {
                if (ImGui::Selectable("Sweep all channels", !fixed)) scanner.set_wifi_fixed_channel(std::nullopt);
                for (const auto& [ch, hz] : channels) {
                    char label[48];
                    std::snprintf(label, sizeof(label), "Channel %d (%.0f MHz)", ch, hz / 1e6);
                    if (ImGui::Selectable(label, fixed && *fixed == ch)) scanner.set_wifi_fixed_channel(ch);
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Stay on one channel: consecutive captures skip the retune, so far more of that "
                                  "channel's air is sampled. Other channels are not heard at all meanwhile.");
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

            static float lora_capture_seconds = float(LORA_LISTEN_DURATION_S);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputFloat("LoRa capture seconds (1-30)", &lora_capture_seconds, 1.0f)) {
                scanner.set_lora_capture_seconds(lora_capture_seconds);
                lora_capture_seconds = float(scanner.lora_capture_seconds());
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Choose a window longer than the expected packet, including its preamble. "
                "Reception pauses during processing/retuning; boundary packets can still be truncated.");
                if (ImGui::Checkbox("Legacy laboratory codecs (nonstandard)", &lora_laboratory_mode)) {
                scanner.set_lora_laboratory_mode(lora_laboratory_mode);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Experimental legacy codecs only. Disable for standard over-the-air decoding. "
                                  "Production derives timing, header and sync observations from IQ.");
            }
        }

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


        };
        auto monitor_band = [&](const std::string& band) {
            if(active_band!=band) scanner.set_wifi_fixed_channel(std::nullopt);
            scanner.set_active_band(band);active_band=band;
        };
        auto draw_rf = [&](const std::string& band) {
            gui::note("Retained RF groups are observations, not physical devices. Power is relative, not calibrated dBm.");
            if(band==BAND_SUB_GHZ && scanner.lora_lock_freq())
                gui::note("Frequency lock active: the wideband map is retained; the wideband energy scan is skipped.",true);
            for(const auto& range:track_ranges_for_band(band))draw_frequency_track(range,rf_rows[band]);
            if(band==BAND_SUB_GHZ) {
                auto locked=scanner.lora_lock_freq();
                if(locked && std::isfinite(*locked) && (*locked<863e6 || *locked>868e6)) {
                    const double mhz=*locked/1e6;
                    draw_frequency_track({"Additional view around locked receive frequency",mhz-.5,mhz+.5},rf_rows[band]);
                }
            }
            gui::note("Orange: LoRa-like | Named green row: decoded Wi-Fi AP | Narrowband: unverified BLE/Zigbee hint | Grey: unknown");
            draw_device_table(rf_rows[band],std::max(180.f,ImGui::GetContentRegionAvail().y-50),source_counts[band]);
        };
        auto draw_security = [&] {
            gui::note("Coverage and incidents span the current monitor run and retained store; they are not limited to the viewed Wi-Fi page.");
            draw_wifi_security_panel(*security,security_queue,run,wifi_security_recording,
                std::max(360.f,ImGui::GetContentRegionAvail().y-30),ui.security_first?"Incidents":nullptr,
                [&](const std::string& action,const std::string& key){scanner.wifi_security_command(action,key);},true);
            ui.security_first=false;
        };
        auto draw_cyclo = [&](const std::string& band) {
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
            bool enabled=scanner.wifi_cyclo_stats().enabled;
            if(ImGui::Checkbox("Enable optional DSP analysis",&enabled))scanner.set_wifi_cyclo_enabled(enabled);
            gui::note("Experimental; sampled independent windows, not full-channel coverage. Named identity remains unvalidated.",true);
            cyclo::draw_shadow_panel(scanner.wifi_cyclo_snapshot(),scanner.wifi_cyclo_stats(),band==BAND_WIFI_2G4?2:5);
#else
            gui::note("Experimental cyclostationary analysis is not compiled in this build.");
#endif
        };
        auto draw_wifi = [&](const std::string& band) {
            ImGui::PushID(band.c_str());
            if(band!=active_band) {
                gui::note(("Viewing retained "+gui::band_label(band)+" observations; the receiver is monitoring "+gui::band_label(active_band)+".").c_str(),true);
                if(ImGui::Button("Monitor this band"))monitor_band(band);
            }
            if(ImGui::Button("Security workspace")){ui.page=gui::Page::Security;ui.security_first=true;}
            ImGui::SameLine();gui::note("Security uses the complete run, not this filtered view.");
            if(ImGui::BeginTabBar("Wi-Fi views")) {
                if(ImGui::BeginTabItem("RF observations")){draw_rf(band);ImGui::EndTabItem();}
                if(ImGui::BeginTabItem("Packets")){
                    std::vector<WifiPacketRow> viewed;
                    for(const auto& row:wifi_rows)if((band==BAND_WIFI_2G4 && row.freq_mhz>=2400 && row.freq_mhz<2500)||
                        (band==BAND_WIFI_5G && row.freq_mhz>=5000 && row.freq_mhz<6000))viewed.push_back(row);
                    gui::note("Search retained rows only. Maximum 1,000 rows across the packet tail; not exhaustive RF coverage.");
                    draw_wifi_packet_table(viewed,std::max(180.f,ImGui::GetContentRegionAvail().y-110));ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Identities & RF clusters")){
                    gui::note("Persistent identities across bands. BSSIDs are claimed interfaces; clusters are provisional. No physical-device count.");
                    auto error=scanner.wifi_storage_error();if(!error.empty())gui::note(error.c_str(),true);
                    draw_wifi_master_table(wifi_identities,std::max(180.f,ImGui::GetContentRegionAvail().y-100),true);ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Experimental link analysis")){draw_cyclo(band);ImGui::EndTabItem();}
                if(ImGui::BeginTabItem("Coverage & limits")){gui::draw_capabilities(drones_available,cyclo_available);ImGui::EndTabItem();}
                ImGui::EndTabBar();
            }ImGui::PopID();
        };
        auto draw_lora = [&] {
            if(active_band!=BAND_SUB_GHZ){gui::note("Viewing retained LoRa data; receiver is monitoring another band.",true);
                if(ImGui::Button("Monitor LoRa band"))monitor_band(BAND_SUB_GHZ);}
            if(scanner.lora_laboratory_mode())gui::note("Nonstandard laboratory mode is active.",true);
            gui::note("LoRaWAN is structural parsing only. Attack rules, MIC verification and authenticated identity are unavailable.");
            if(ImGui::BeginTabBar("LoRa views")) {
                if(ImGui::BeginTabItem("RF observations")){draw_rf(BAND_SUB_GHZ);ImGui::EndTabItem();}
                if(ImGui::BeginTabItem("PHY packets")){
                    gui::note("SF/BW/polarity hypothesis rows; not unique packets. Recorded interoperability evidence covers tested setups only.");
                    draw_lora_packet_table(lora_rows,std::max(180.f,ImGui::GetContentRegionAvail().y-150));ImGui::EndTabItem();}
                if(ImGui::BeginTabItem("Master emitters")){
                    gui::note("Heuristic RF clusters matched on IRR/DC/IQ imbalance, not CFO. History retains about 1,000 readings, with compaction slack to 1,100.");
                    draw_lora_master_table(lora_identities,std::max(180.f,ImGui::GetContentRegionAvail().y-40));ImGui::EndTabItem();}
                if(ImGui::BeginTabItem("Capture evidence")){draw_lora_security_panel(*scanner.lora_security_snapshot());ImGui::EndTabItem();}
                if(ImGui::BeginTabItem("Coverage & limits")){gui::draw_capabilities(drones_available,cyclo_available);ImGui::EndTabItem();}
                ImGui::EndTabBar();
            }
            if(ImGui::Button("Save / offline review"))ui.page=gui::Page::Captures;
        };
        auto draw_captures = [&] {
            gui::note("LIVE save and OFFLINE results are separate. Source files remain read-only during review.");
            if (ImGui::CollapsingHeader("Save a LoRa IQ capture")) {
                static char save_directory[1024] = PROJECT_ROOT_DIR "/data/lora_captures";
                ImGui::InputText("Save under", save_directory, sizeof(save_directory));
                ImGui::BeginDisabled(active_band != BAND_SUB_GHZ || !status.connected || scanner.lora_capture_pending() || !save_directory[0]);
                if (ImGui::Button("Save next completed capture")) scanner.request_lora_capture_save(save_directory);
                ImGui::EndDisabled();
                ImGui::TextWrapped("One capture per click, including recordings with no detections. IQ and settings are saved together. Requires a connected receiver.");
                auto message = scanner.lora_capture_message();
                if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
            }

            if(active_band!=BAND_SUB_GHZ)gui::note("Save next LoRa capture requires LoRa acquisition. Change it explicitly in Monitoring setup.");
            draw_lora_replay_panel();
#ifdef RFMON_ENABLE_DJI_DRONEID
            ImGui::SeparatorText("Offline DJI interval review");
            gui::note("Use the existing bounded DJI decoder: raw cf32, explicit rate/centre/offset. No Wi-Fi Remote ID IQ replay.");
            if(ImGui::Button("Open DJI offline review")){ui.page=gui::Page::Drones;ui.drones_offline=true;}
#endif
            if(gui::section("Unsupported investigation workflows"))gui::note("Wi-Fi IQ/event replay adapters, a disk capture catalogue and evidence-bundle export are not implemented in this workspace.");
        };
        gui::Context context;
        context.radio=scanner.device_type()==SdrDeviceType::B210?"USRP B210":"USRP X310";
        context.band=active_band;context.step=status.active_step_label;context.error=status.error;
        if(status.wifi_fixed_channel)context.step+=" | fixed channel "+std::to_string(*status.wifi_fixed_channel);
        if(status.wifi_fixed_channel_invalid)context.error+=" Requested fixed channel is invalid for the active band; sweeping.";
        if(security_queue.events_dropped||security_queue.captures_dropped)context.error+=" Security queue loss reported.";
        const auto identity_error=scanner.wifi_storage_error();if(!identity_error.empty())context.error+=" Wi-Fi identity storage: "+identity_error;
        const auto lora_health=scanner.lora_security_snapshot();if(!lora_health->recording_error.empty())context.error+=" LoRa recording: "+lora_health->recording_error;
        context.started=scanner_started;context.connected=status.connected;context.stalled=status.rx_stalled;
        context.overflow=status.last_overflow;context.cycles=status.cycle_count;context.incidents=security->incidents_open;
        context.recording=wifi_security_recording;context.run=run;
        context.recording_pending=scanner_started && record_wifi_security!=!wifi_security_recording.empty();
        context.laboratory=scanner.lora_laboratory_mode();
        if(!security->storage_error.empty())context.error+=" Recording: "+security->storage_error;
        if(!security->persist_error.empty())context.error+=" Persistence: "+security->persist_error;
        auto draw_overview = [&] {
            gui::Overview data;
            data.captures=security->captures_ingested;data.groups=rf_rows[active_band].size();
            data.queue_depth=security_queue.depth;data.queue_peak=security_queue.high_water;
            data.queue_loss=security->queue_events_dropped||security->queue_captures_dropped;
            data.lane_enabled=status.wifi_lane_enabled;data.lane_wait=status.wifi_lane_wait_s;data.lane_peak=status.wifi_lane_high_water;
#ifdef RFMON_ENABLE_DJI_DRONEID
            data.dji_enabled=drones.live.running();data.rid_enabled=drones.remote_id.enabled();
#endif
            gui::draw_overview(ui,context,data,drones_available,[&]{
                if(active_band==BAND_SUB_GHZ && scanner.lora_lock_freq())gui::note("Frequency lock active: wideband map is retained; wideband scan is skipped.",true);
                for(const auto& range:track_ranges_for_band(active_band))draw_frequency_track(range,rf_rows[active_band]);
            });
        };
        gui::draw_workspace(ui,context,drones_available,[&](gui::Page page){
            if(ui.setup_open || page==gui::Page::Settings || ui.legacy)draw_setup();
            if(ui.legacy){
                if(ImGui::BeginTabBar("Legacy monitoring views")){
                    if(ImGui::BeginTabItem("Monitoring")){if(active_band==BAND_SUB_GHZ)draw_lora();else draw_wifi(active_band);ImGui::EndTabItem();}
                    if(ImGui::BeginTabItem("Security")){draw_security();ImGui::EndTabItem();}
                    if(ImGui::BeginTabItem("Captures")){draw_captures();ImGui::EndTabItem();}
#ifdef RFMON_ENABLE_DJI_DRONEID
                    if(ImGui::BeginTabItem("Drones")){drone::draw_panel(drones,gui::band_label(active_band),status.connected);ImGui::EndTabItem();}
#endif
                    if(ImGui::BeginTabItem("Display")){if(ImGui::Button("Return to modern workspace"))ui.legacy=false;ImGui::EndTabItem();}
                    ImGui::EndTabBar();
                }return;
            }
            switch(page){
            case gui::Page::Overview:draw_overview();break;
            case gui::Page::Wifi24:draw_wifi(BAND_WIFI_2G4);break;
            case gui::Page::Wifi5:draw_wifi(BAND_WIFI_5G);break;
            case gui::Page::Lora:draw_lora();break;
            case gui::Page::Security:draw_security();break;
            case gui::Page::Captures:draw_captures();break;
            case gui::Page::Drones:
#ifdef RFMON_ENABLE_DJI_DRONEID
                drone::draw_panel(drones,gui::band_label(active_band),status.connected,ui.drones_offline?"Offline IQ review":nullptr);ui.drones_offline=false;
#endif
                break;
            case gui::Page::Settings:
                if(gui::section("Display preferences",true)){
                    bool changed=ImGui::SliderFloat("UI scale",&ui.scale,1,2,"%.2fx");
                    changed=ImGui::Checkbox("Compact table density",&ui.compact)||changed;
                    if(changed)theme_dirty=true;
                    ImGui::Checkbox("Legacy presentation",&ui.legacy);
                    gui::note("Display preferences are session-only and never start acquisition or analysis.");
                }
                if(gui::section("Capabilities",true))gui::draw_capabilities(drones_available,cyclo_available);break;
            }
        });
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
