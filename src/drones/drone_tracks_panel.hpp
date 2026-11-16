#pragma once

#include "drone_tracks.hpp"
#include "imgui.h"
#include <ctime>

namespace rfmon::drone_claims {
// Shared by the live history and the recorded-fixture GUI test.
inline void draw_drone_history(const DroneTracks& drone_tracks, Clock::time_point now) {
    ImGui::SeparatorText("Drone history");
    for (const auto& alert : drone_tracks.alerts()) {
        ImGui::TextWrapped("%s%s", alert.lower_severity ? "Advisory · " : "", alert.alert.c_str());
    }
    std::vector<const DroneTrack*> history;
    for (const auto& [key, track] : drone_tracks.tracks()) history.push_back(&track);
    std::sort(history.begin(), history.end(), [](const auto* a, const auto* b) {
        return std::tie(a->last_seen, a->id) > std::tie(b->last_seen, b->id);
    });
    const auto wall = std::chrono::system_clock::now();
    const auto show_time = [&](Clock::time_point at) {
        const auto local = std::chrono::system_clock::to_time_t(wall -
            std::chrono::duration_cast<std::chrono::system_clock::duration>(now - at));
        char text[32]{};
        std::tm parts{};
        if (localtime_r(&local, &parts)) std::strftime(text, sizeof text, "%d %b %H:%M:%S", &parts);
        ImGui::TextUnformatted(text);
    };
    if (history.empty()) ImGui::TextUnformatted("No drone tracks yet");
    else if (ImGui::BeginTable("drone_history", 10,
               ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 300))) {
        for (const auto* heading : {"Identity", "Best model verdict", "First seen", "Last seen",
                                    "Time present", "Appearances", "Bands", "Evidence",
                                    "Last reported positions", "State"})
            ImGui::TableSetupColumn(heading);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto* track : history) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", track->identity.c_str());
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s · %s", tier_name(track->best.tier), track->best.model.c_str());
            ImGui::TextWrapped("%s", track->best.basis.c_str());
            ImGui::TableNextColumn(); show_time(track->first_seen);
            ImGui::TableNextColumn(); show_time(track->last_seen);
            ImGui::TableNextColumn(); ImGui::Text("%.0f s", std::chrono::duration<double>(track->time_present).count());
            ImGui::TableNextColumn(); ImGui::Text("%zu", track->appearances);
            ImGui::TableNextColumn();
            for (const auto& band : track->bands) ImGui::TextUnformatted(band.c_str());
            ImGui::TableNextColumn();
            for (const auto& source : track->evidence) ImGui::TextUnformatted(source.c_str());
            if (track->encrypted) ImGui::TextUnformatted("Encrypted identity seen");
            if (track->conflict) ImGui::TextUnformatted("Possible conflict seen");
            ImGui::TableNextColumn();
            const auto show_position = [&](const char* role, const auto& fix) {
                if (!fix) return;
                ImGui::TextWrapped("%s %.6f, %.6f (%s, self-reported, unauthenticated)",
                                   role, fix->latitude, fix->longitude, fix->source.c_str());
                show_time(fix->observed);
            };
            show_position("Aircraft", track->positions.aircraft);
            show_position("Pilot", track->positions.pilot);
            show_position("Home / takeoff", track->positions.home);
            show_position("Operator", track->positions.operator_location);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(track->present ? "Present" : "Gone");
        }
        ImGui::EndTable();
    }
}
}
