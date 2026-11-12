#pragma once

#include "evidence_claims.hpp"
#include "../ml_inference.hpp"
#include <set>
#include <sstream>

namespace rfmon::drone_claims {
// Require two independently keyed captures so one capture's intervals cannot supply a hint.
inline constexpr std::size_t minimum_hint_captures = 2;
// Require at least half the uncalibrated score mass before a top class may vote.
inline constexpr double minimum_top_hint_score = .5;

inline const char* classifier_model(std::string_view label) {
    struct Alias { std::string_view label; const char* model; };
    static constexpr Alias aliases[] = {
        {"DJI_Avata_2", "Avata 2"}, {"DJI_Mavic_3_Pro", "Mavic 3 Pro"},
        {"DJI_Air_2", "Mavic Air 2"}, {"DJI_Air_2S", "Air 2S"}, {"DJI_FPV", "FPV"},
        {"DJI_Inspire_2", "Inspire 2"}, {"DJI_Matrice_100", "M100"},
        {"DJI_Matrice_210", "M210"}, {"DJI_Matrice_300", "M300 RTK"},
        {"DJI_Mavic_3", "Mavic 3"}, {"DJI_Mavic_Mini", "Mavic Mini"},
        {"DJI_Mavic_Pro", "Mavic Pro"}, {"DJI_Mini_2", "Mini 2"},
        {"DJI_Mini_4_Pro", "Mini 4 Pro"}, {"DJI_Mini_5_Pro", "Mini 5 Pro"},
        {"DJI_Phantom_4", "Phantom 4"}, {"DJI_Phantom_4_Pro_Plus", "Phantom 4 Pro"},
    };
    for (const auto& alias : aliases)
        if (alias.label == label) return alias.model;
    return nullptr;
}

inline std::pair<std::string, std::size_t> unique_winner(
    const std::map<std::string, std::size_t>& votes) {
    std::pair<std::string, std::size_t> best;
    for (const auto& [model, count] : votes) {
        if (count > best.second) best = {model, count};
        else if (count == best.second) best.first.clear();
    }
    return best;
}

inline void add_classifier_hints(std::map<std::string, Detection>& rows,
                                 const ml::Snapshot& classifier,
                                 std::set<std::string>& unmapped) {
    const bool ready = classifier.enabled && classifier.ready;
    std::set<std::string> models;
    if (ready) {
        std::istringstream classes(classifier.classes);
        for (std::string label; std::getline(classes, label);)
            if (const auto* model = classifier_model(label)) models.emplace(model);
        for (const auto& result : classifier.history)
            if (result.model_hash == classifier.model_hash)
                for (const auto& candidate : result.top)
                    if (!classifier_model(candidate.label)) unmapped.insert(candidate.label);
    }
    for (auto& [id, row] : rows) {
        row.classifier_hint.reset();
        row.only_classifier_class.clear();
        if (!ready || row.group.empty() || row.link_captures.empty() ||
            (row.packet && row.packet->readable && !row.packet->model.empty())) continue;
        std::set<std::string> eligible;
        for (const auto& model : models) {
            const auto* entry = model_link(model);
            if (entry && ((entry->group && row.group == entry->group) ||
                          (entry->also && row.group == entry->also))) eligible.insert(model);
        }
        if (eligible.size() == 1) row.only_classifier_class = *eligible.begin();
        if (eligible.size() < 2) continue;
        std::map<std::string, std::size_t> votes;
        std::size_t voting_captures = 0;
        for (const auto& key : row.link_captures) {
            std::map<std::string, std::size_t> interval_votes;
            std::map<std::size_t, bool> seen;
            for (const auto& result : classifier.history) {
                if (result.run != key.run || result.session != key.session || result.capture != key.capture ||
                    !std::isfinite(result.center) || std::abs(result.center - key.center) > 1e3 ||
                    result.model_hash != classifier.model_hash ||
                    !seen.emplace(result.interval_index, true).second) continue;
                if (result.top.empty()) continue;
                const auto& candidate = result.top.front();
                const auto* model = classifier_model(candidate.label);
                if (model && eligible.count(model) && candidate.score >= minimum_top_hint_score)
                    ++interval_votes[model];
            }
            const auto capture_winner = unique_winner(interval_votes);
            if (capture_winner.first.empty()) continue;
            ++votes[capture_winner.first];
            ++voting_captures;
        }
        const auto winner = unique_winner(votes);
        if (voting_captures >= minimum_hint_captures && !winner.first.empty())
            row.classifier_hint = ClassifierHint{winner.first, winner.second, voting_captures};
    }
}
}
