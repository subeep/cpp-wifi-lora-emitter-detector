#pragma once
// One answer per drone row to "which model is it?", with how sure we are.
//
// The evidence is ranked, strongest first:
//   Confirmed  - a readable DJI DroneID packet names the model (self-reported, unauthenticated).
//   Likely     - DroneID is present but encrypted: only models seen to encrypt it, and whose
//                link shows the row's timing group, remain.
//   Family     - only the video link was seen: its timing group gives the link family and the
//                models measured in it; a classifier hint, if any, is named as a hint.
//   Reported   - only a Remote ID broadcast: it carries an ID and an aircraft type, no model.
//   Unknown    - DroneID was read but its product code is not in the table.
// Nothing here creates or removes a row; it only words what the row's evidence supports.
#include "evidence_claims.hpp"

namespace rfmon::drone_claims {

// How each model's DroneID was found in recordings: 'r' readable, 'e' encrypted (INFP/CRYP),
// 'b' both (depends on firmware or region). Models not listed are unknown. Sources: DRFF-R2
// (Air 2, Air 2S, Mini 3 Pro readable; Mavic 3, Mini 4 Pro, Mini 5 Pro encrypted), RFUAV
// (Mini 3 readable; Mini 4 Pro, Avata 2 encrypted), KU Leuven (Mini 2, M300 RTK, Mavic 2
// Enterprise Advanced readable), fixtures (Mavic Air 2, Mini 2), live X310 (Mini 2 readable).
// Mavic 3 is 'b': encrypted in DRFF-R2, and listed with a readable code (68) in the public table.
struct DroneIdMode { const char* model; char mode; };
inline constexpr DroneIdMode droneid_modes[] = {
    {"Mavic Air 2", 'r'}, {"Air 2S", 'r'}, {"Mini 2", 'r'}, {"Mini 3", 'r'}, {"Mini 3 Pro", 'r'},
    {"M300 RTK", 'r'}, {"Mavic 2 Enterprise Advanced", 'r'},
    {"Mini 4 Pro", 'e'}, {"Mini 5 Pro", 'e'}, {"Avata 2", 'e'}, {"Mavic 3", 'b'},
};
inline char droneid_mode(const std::string& model) {
    for (const auto& row : droneid_modes)
        if (model == row.model) return row.mode;
    return '?';
}

inline const char* link_family(const std::string& group) {
    if (group == "32.8kHz") return "DJI Lightbridge family";
    if (group == "30kHz") return "DJI O4 family";
    if (group == "15kHz+30kHz") return "DJI O3 family";
    if (group == "15kHz") return "DJI OcuSync family";   // OcuSync 1/2, most O3 and O4+ models
    return "Drone-type link";
}

enum class Tier { Confirmed, Likely, Family, Reported, Unknown };
inline const char* tier_name(Tier t) {
    switch (t) {
        case Tier::Confirmed: return "Confirmed";
        case Tier::Likely: return "Likely";
        case Tier::Family: return "Family";
        case Tier::Reported: return "Reported";
        default: return "Unknown";
    }
}

struct Verdict {
    Tier tier = Tier::Unknown;
    std::string model;   // the answer to show: a model, a short list, or a family
    std::string basis;   // one line: what the answer rests on
    std::string detail;  // optional second line: what is not ruled out, hints, disagreements
};

inline std::string join(const std::vector<std::string>& names, const char* last = " or ") {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i)
        out += (i == 0 ? "" : i + 1 == names.size() ? last : ", ") + names[i];
    return out;
}

// Models whose link shows `group` ("" = any group), split by what is known of their DroneID.
struct GroupModels { std::vector<std::string> measured, expected, encrypting, not_ruled_out; };
inline GroupModels models_in(const std::string& group) {
    GroupModels out;
    for (const auto& row : model_links) {
        if (!row.group) continue;
        if (!group.empty() && group != row.group && !(row.also && group == row.also)) continue;
        (row.measured ? out.measured : out.expected).push_back(row.model);
        const char mode = droneid_mode(row.model);
        if (mode == 'e' || mode == 'b') out.encrypting.push_back(row.model);
        else if (mode == '?') out.not_ruled_out.push_back(row.model);
    }
    return out;
}

inline Verdict verdict(const Detection& row) {
    Verdict v;
    const auto in_group = models_in(row.group);
    const std::string link = row.group.empty() ? "" : row.group.substr(0, row.group.size() - 3) + " kHz link";
    if (row.packet && row.packet->readable && !row.packet->model.empty()) {
        v.tier = Tier::Confirmed;
        v.model = row.packet->model;
        v.basis = "DJI DroneID names the model (self-reported)";
        if (row.conflict) v.detail = "The link seen in the same capture does not fit this model";
        else if (!row.group.empty()) v.detail = "The " + link + " fits this model";
        else v.detail = "No video link seen for it yet";
        return v;
    }
    if (row.packet && row.packet->readable) {
        v.tier = Tier::Unknown;
        v.model = "DJI, product code " + std::to_string(row.packet->product_code) + " not in table";
        v.basis = "DJI DroneID read, code not yet tied to a model";
        if (!row.group.empty()) v.detail = std::string(link_family(row.group)) + " (" + link + ")";
        return v;
    }
    if (row.packet && row.packet->encrypted) {
        v.tier = Tier::Likely;
        v.model = in_group.encrypting.empty() ? "DJI, recent model" : join(in_group.encrypting);
        v.basis = row.group.empty() ? "Encrypted DJI DroneID; no video link seen yet"
                                    : "Encrypted DJI DroneID with a " + link;
        if (!in_group.not_ruled_out.empty() && !row.group.empty())
            v.detail = "Not ruled out (DroneID behaviour unknown): " + join(in_group.not_ruled_out, ", ");
        return v;
    }
    if (row.packet) {   // checksum-valid DroneID that is neither readable nor a known encrypted form
        v.tier = Tier::Family;
        v.model = row.group.empty() ? "DJI aircraft" : link_family(row.group);
        v.basis = "DJI DroneID present, identity not readable";
        if (!in_group.measured.empty() && !row.group.empty()) v.detail = "Measured in this group: " + join(in_group.measured, ", ");
        return v;
    }
    if (!row.group.empty()) {
        v.tier = Tier::Family;
        v.model = link_family(row.group);
        v.basis = "Video link only (" + link + "); no DroneID decoded";
        if (row.classifier_hint)
            v.detail = "Classifier hint: " + row.classifier_hint->model + " (" + std::to_string(row.classifier_hint->votes) +
                       " of " + std::to_string(row.classifier_hint->voting_captures) + " link captures). ";
        v.detail += "Measured in this group: " + join(in_group.measured, ", ");
        return v;
    }
    if (!row.remote_id.empty()) {
        v.tier = Tier::Reported;
        const auto& rid = row.remote_id.front();
        v.model = rid.ua_type.empty() ? "Remote ID aircraft" : "Remote ID aircraft (" + rid.ua_type + ")";
        v.basis = "Remote ID broadcast (self-reported); it carries no model name";
        return v;
    }
    v.model = "Unresolved";
    v.basis = "No usable evidence";
    return v;
}

}  // namespace rfmon::drone_claims
