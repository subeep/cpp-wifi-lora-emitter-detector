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
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "config.hpp"
#include "registry.hpp"
#include "scanner.hpp"

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

void draw_device_table(const std::vector<DeviceRow>& devices) {
    if (devices.empty()) {
        ImGui::TextDisabled("No active emitters right now.");
        return;
    }

    static ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                   ImGuiTableFlags_ScrollY;
    ImVec2 outer_size(0.0f, ImGui::GetContentRegionAvail().y);
    if (!ImGui::BeginTable("devices", 8, flags, outer_size)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Freq (MHz)",
                            ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed,
                            100.0f);
    ImGui::TableSetupColumn("BW (kHz)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Protocol guess", ImGuiTableColumnFlags_WidthStretch);
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
                    case 1: less = a.bandwidth_khz < b.bandwidth_khz; break;
                    case 2: less = a.protocol_guess < b.protocol_guess; break;
                    case 3: less = a.power_db < b.power_db; break;
                    case 4: less = a.hit_count < b.hit_count; break;
                    case 5: less = a.age_s < b.age_s; break;
                    case 6: less = a.last_seen_s_ago < b.last_seen_s_ago; break;
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
        ImGui::Text("%.1f", d.bandwidth_khz);
        ImGui::TableNextColumn();
        ImGui::TextColored(category_color(d.protocol_guess), "%s", d.protocol_guess.c_str());
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

    Scanner scanner;
    scanner.start();

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

        ScannerStatus status = scanner.status();
        std::string active_band = scanner.active_band();

        // --- Status line ---
        if (!status.connected) {
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

        ImGui::Spacing();

        // --- Controls ---
        if (ImGui::SliderFloat("Threshold (dB above noise floor)", &threshold_db, 3.0f, 30.0f,
                                "%.1f")) {
            scanner.set_threshold_db(threshold_db);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("AGC", &agc)) {
            scanner.set_gain(agc ? std::optional<double>() : std::optional<double>(gain_db));
        }
        if (!agc) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderFloat("Gain (dB)", &gain_db, 0.0f, 70.0f, "%.0f")) {
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
        draw_device_table(scanner.snapshot(active_band));

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
