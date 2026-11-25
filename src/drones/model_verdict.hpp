#pragma once
// One answer per drone row to "which model is it?", with how sure we are.
//
// The evidence is ranked, strongest first:
//   Confirmed  - a readable DJI DroneID packet names the model (self-reported, unauthenticated).
//   Likely     - DroneID is present but encrypted: only models seen to encrypt it, and whose
//                link shows the row's timing group, remain.
//   Family     - only the video link was seen: its timing group gives the link family and the
//                models measured in it (the candidates, narrowed by the band it was heard in);
//                a classifier hint, if any, is named as a hint.
//   Reported   - only a Remote ID broadcast: it carries an ID and an aircraft type, no model.
//   Unknown    - DroneID was read but its product code is not in the table.
//   Classifier - only the universal classifier saw it (no link, DroneID or Remote ID); weakest.
// A Family row names the classifier's ranked models as its answer only when the package measured
// that naming on a dataset (receiver) it never trained on (UniversalClaim::naming_carries);
// otherwise they are a guess in the detail line and the answer stays the family with its candidates.
// Nothing here creates or removes a row; it only words what the row's evidence supports.
#include "evidence_claims.hpp"
#include <iterator>

namespace rfmon::drone_claims {

// Captures of a row that must name models before the classifier's list is shown (C4 row rule).
inline constexpr std::size_t minimum_named_classifier_captures = 2;
// A classifier model name becomes a row's answer only when the package measured it on a dataset
// (another lab's receiver) it never trained on: at least this many captures named there, and this
// share right both ways: of its own captures named, and of every time its name was given (names
// given to other drones count against it). Below that, receiver and setup dominate what the
// classifier learnt, so its name is shown as a guess beside the link's candidates (corpus check,
// 23 Nov 2026; both ways since 24 Nov 2026).
inline constexpr std::size_t minimum_other_dataset_named = 20;
inline constexpr double minimum_other_dataset_top1 = 0.8;

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

enum class Tier { Confirmed, Likely, Family, Reported, Unknown, Classifier };
inline const char* tier_name(Tier t) {
    switch (t) {
        case Tier::Confirmed: return "Confirmed";
        case Tier::Likely: return "Likely";
        case Tier::Family: return "Family";
        case Tier::Reported: return "Reported";
        case Tier::Classifier: return "Classifier";
        default: return "Unknown";
    }
}

struct Verdict {
    Tier tier = Tier::Unknown;
    std::string model;   // the answer to show: a model, a short list, or a family
    std::string basis;   // one line: what the answer rests on
    std::string detail;  // optional second line: what is not ruled out, hints, disagreements
    std::string link;        // OcuSync generation(s) the evidence leaves: "OcuSync 2", "O3 or O4"; "" = none
    std::string link_short;  // the same for tight labels: "OS2", "O3/O4", "~O3"
    std::string candidates;  // Family: the measured models left, "12 candidates: Mavic Pro, Mini 2, Mini 3 +9 more"
};

inline const char* type_words(const std::string& type) {
    if (type == "dji_ofdm") return "DJI OFDM link";
    if (type == "lightbridge") return "DJI Lightbridge link";
    if (type == "hopping_controller") return "Hopping remote controller";
    return "Drone-type signal";
}

inline std::vector<std::string> names_of(const UniversalClaim& c) {
    std::vector<std::string> out;
    for (const auto& m : c.models) out.push_back(m.name);
    return out;
}

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

// Classifier class names that differ from the model table's names.
inline std::string table_name(const std::string& model) {
    static const std::map<std::string, std::string> names = {
        {"Air 2", "Mavic Air 2"}, {"Matrice 300", "M300 RTK"}, {"Matrice 100", "M100"},
        {"Matrice 200", "M200"}, {"Matrice 210", "M210"}, {"Matrice 600 Pro", "M600 Pro"},
        {"Matrice 30T", "M30"}, {"Phantom 3", "Phantom 3 Series"}};    // DroneRFa names (E4)
    const auto it = names.find(model);
    return it == names.end() ? model : it->second;
}

// OcuSync generation of an advertised link name. Variants fold into their generation: O3+,
// O3 (FPV) and O3 Enterprise are O3; O4+ is O4; OcuSync 2 Enterprise is OcuSync 2. nullptr when
// the name fixes no generation (Lightbridge, Wi-Fi, a plain "OcuSync", unknown).
inline constexpr const char* generations[] = {"OcuSync 1", "OcuSync 2", "O3", "O4"};
inline const char* generation_of(std::string_view link) {
    if (link == "OcuSync 1") return generations[0];
    if (link.rfind("OcuSync 2", 0) == 0) return generations[1];
    if (link.rfind("O3", 0) == 0) return generations[2];
    if (link.rfind("O4", 0) == 0) return generations[3];
    return nullptr;
}
inline const char* generation_short(std::string_view g) {
    return g == "OcuSync 1" ? "OS1" : g == "OcuSync 2" ? "OS2" : g == "O3" ? "O3" : g == "O4" ? "O4" : "?";
}

// Bands each model's video link uses, from DJI's specification pages (checked 20 Nov 2026).
// 5.1 GHz (5.170-5.250) is listed only for the O4 aircraft, and only where local law allows it.
// Models not listed are unknown and never ruled out by band: Mavic Pro's page lists 5 GHz for
// the aircraft beside a 2.4/5 GHz Wi-Fi mode, so its OcuSync 1 bands are not settled.
enum : unsigned { band_24 = 1, band_51 = 2, band_58 = 4 };
struct ModelBands { const char* model; unsigned bands; };
inline constexpr ModelBands model_bands[] = {
    {"Mavic Air 2", band_24 | band_58}, {"Mavic 2", band_24 | band_58},
    {"Mavic 2 Enterprise Advanced", band_24 | band_58}, {"Mini 2", band_24 | band_58},
    {"Mini 3", band_24 | band_58}, {"M300 RTK", band_24 | band_58}, {"Phantom 4 Pro V2.0", band_24 | band_58},
    {"Mini 3 Pro", band_24 | band_58}, {"Air 2S", band_24 | band_58}, {"Mavic 3", band_24 | band_58},
    {"FPV", band_24 | band_58}, {"Mavic 3 Pro", band_24 | band_58}, {"M30", band_24 | band_58},
    {"Mini 4 Pro", band_24 | band_51 | band_58}, {"Avata 2", band_24 | band_51 | band_58},
    {"Air 3S", band_24 | band_51 | band_58}, {"Air 3", band_24 | band_51 | band_58},
    {"Mini 5 Pro", band_24 | band_51 | band_58},
};
inline unsigned bands_of(const std::string& model) {
    for (const auto& row : model_bands)
        if (model == row.model) return row.bands;
    return 0;
}
inline unsigned band_at(double hz) {
    if (hz >= 2.38e9 && hz <= 2.50e9) return band_24;
    if (hz >= 5.14e9 && hz <= 5.26e9) return band_51;
    if (hz >= 5.70e9 && hz <= 5.88e9) return band_58;
    return 0;
}
inline const char* band_words(unsigned band) {
    return band == band_24 ? "2.4 GHz" : band == band_51 ? "5.1 GHz" : band == band_58 ? "5.8 GHz" : "";
}

// Which OcuSync generations a row's evidence leaves, strongest evidence first:
//   a readable DroneID model gives its own generation (exact);
//   otherwise the generations of the models measured with the row's timing group (with an
//   encrypted DroneID, only the measured models seen to encrypt it);
//   the band the link was heard in then drops models whose link does not use that band
//   (models with unknown bands stay, and nothing is dropped if nothing would be left);
//   when the classifier's accepted models all share one of the remaining generations and its
//   naming held on another receiver (naming_carries), that generation is `likely`, a hint;
//   `names` keeps what the other evidence allows.
// Empty names: no generation (Lightbridge, Wi-Fi, no link and no DroneID, unknown model).
struct LinkGenerations {
    std::vector<std::string> names;  // in generation order
    bool exact{};                    // from a readable DroneID model
    std::string likely;              // the classifier's generation, narrower than names
    std::string ruled_out;           // what the band ruled out, in words
};
inline int generation_index(const std::string& g) {
    for (int i = 0; i < int(std::size(generations)); ++i)
        if (g == generations[i]) return i;
    return int(std::size(generations));
}
// The measured models a row's link evidence leaves: those whose link shows the row's timing group
// (any group when there is none), only models seen to encrypt DroneID when it is encrypted, and
// then those whose video uses the band the link was heard in. Models with unknown bands stay, and
// the band drops nothing when it would drop everything. `with_generation`: OcuSync models only.
struct Candidates { std::vector<const ModelLink*> kept, dropped; unsigned heard{}; };
inline Candidates measured_candidates(const Detection& row, bool with_generation) {
    Candidates out;
    const bool encrypted = row.packet && row.packet->encrypted;
    for (const auto& m : model_links) {
        if (!m.measured || !m.group || (with_generation && !generation_of(m.link))) continue;
        if (!row.group.empty() && row.group != m.group && !(m.also && row.group == m.also)) continue;
        if (encrypted && droneid_mode(m.model) != 'e' && droneid_mode(m.model) != 'b') continue;
        out.kept.push_back(&m);
    }
    out.heard = row.group.empty() || !(row.link_low_hz > 0) ? 0 : band_at((row.link_low_hz + row.link_high_hz) / 2);
    if (out.heard) {
        std::vector<const ModelLink*> kept, dropped;
        for (const auto* m : out.kept) {
            const unsigned b = bands_of(m->model);
            (b == 0 || (b & out.heard) ? kept : dropped).push_back(m);
        }
        if (!kept.empty() && !dropped.empty()) out.kept = std::move(kept), out.dropped = std::move(dropped);
    }
    return out;
}

inline LinkGenerations link_generations(const Detection& row) {
    LinkGenerations out;
    const auto by_order = [](const std::string& a, const std::string& b) { return generation_index(a) < generation_index(b); };
    if (row.packet && row.packet->readable && !row.packet->model.empty()) {
        if (const auto* m = model_link(row.packet->model))
            if (const char* g = generation_of(m->link)) out.names = {g}, out.exact = true;
        return out;
    }
    if (row.group.empty() && !(row.packet && row.packet->encrypted)) return out;
    const auto c = measured_candidates(row, true);
    if (!c.dropped.empty()) {
        std::vector<std::string> gone;
        for (const auto* d : c.dropped) {
            const std::string g = generation_of(d->link);
            const bool stays = std::any_of(c.kept.begin(), c.kept.end(), [&](const auto* k) { return g == generation_of(k->link); });
            if (!stays && std::find(gone.begin(), gone.end(), g) == gone.end()) gone.push_back(g);
        }
        std::sort(gone.begin(), gone.end(), by_order);
        if (!gone.empty())
            out.ruled_out = std::string("Heard at ") + band_words(c.heard) + ": not " + join(gone) +
                            " (no measured model of " + (gone.size() == 1 ? "it" : "them") + " uses that band for video)";
    }
    const auto& models = c.kept;
    for (const auto* m : models)
        if (std::find(out.names.begin(), out.names.end(), generation_of(m->link)) == out.names.end())
            out.names.push_back(generation_of(m->link));
    std::sort(out.names.begin(), out.names.end(), by_order);
    if (out.names.size() > 1 && row.universal && row.universal->accepted && row.universal->naming_carries &&
        !row.universal->models.empty()) {
        std::string shared;
        for (const auto& s : row.universal->models) {
            const auto* m = model_link(table_name(s.name));
            const char* g = m ? generation_of(m->link) : nullptr;
            if (!g || (!shared.empty() && shared != g)) { shared.clear(); break; }
            shared = g;
        }
        if (std::find(out.names.begin(), out.names.end(), shared) != out.names.end()) out.likely = shared;
    }
    return out;
}
// "OcuSync 2", "O3 or O4", "O3 (from classifier)"; "" when there is none.
inline std::string generation_text(const LinkGenerations& g) {
    return g.likely.empty() ? join(g.names) : g.likely + " (from classifier)";
}
// For tight labels: "OS2", "OS1/OS2/O3/O4", "~O3" for the classifier's generation.
inline std::string generation_short_text(const LinkGenerations& g) {
    if (!g.likely.empty()) return std::string("~") + generation_short(g.likely);
    std::string out;
    for (const auto& n : g.names) out += (out.empty() ? "" : "/") + std::string(generation_short(n));
    return out;
}
// Model names with their advertised link: "Mini 2 (OcuSync 2), Mavic 3 (O3+)".
inline std::string with_links(const std::vector<std::string>& models) {
    std::string out;
    for (const auto& m : models) {
        const auto* row = model_link(m);
        out += (out.empty() ? "" : ", ") + m + (row && *row->link ? std::string(" (") + row->link + ")" : "");
    }
    return out;
}

// "Mini 2", "3 candidates: Inspire 2, M100, M210", "12 candidates: Mavic Pro, Mavic Air 2, Mini 2 +9 more".
inline std::string candidates_text(const std::vector<const ModelLink*>& models, std::size_t shown = 3) {
    if (models.empty()) return {};
    if (models.size() == 1) return models.front()->model;
    std::string out = std::to_string(models.size()) + " candidates: ";
    for (std::size_t i = 0; i < models.size() && i < shown; ++i) out += (i ? ", " : "") + std::string(models[i]->model);
    if (models.size() > shown) out += " +" + std::to_string(models.size() - shown) + " more";
    return out;
}

// The classifier's ranked models for a detail line: a ranking when its naming held on another
// receiver, otherwise a guess that is not the answer.
inline std::string classifier_models_text(const UniversalClaim& u) {
    const std::string counts = std::to_string(u.named) + " of " + std::to_string(u.captures) + " captures";
    if (u.naming_carries) return "Classifier ranks: " + join(names_of(u), ", ") + " (" + counts + ")";
    return "Classifier guess, not the answer (its model names have not held on a receiver it was not trained on): " +
           join(names_of(u), ", ") + " (" + counts + ")";
}

inline Verdict verdict(const Detection& row) {
    Verdict v;
    const auto gens = link_generations(row);
    v.link = generation_text(gens);
    v.link_short = generation_short_text(gens);
    const auto in_group = models_in(row.group);
    const std::string link = row.group.empty() ? ""
                             : row.group == "32.8kHz" ? "Lightbridge link, 10.9 kHz timing"
                                                      : row.group.substr(0, row.group.size() - 3) + " kHz link";
    // A link's name: its OcuSync generation(s) when the evidence gives any, else the timing family.
    const std::string family = gens.names.empty() ? std::string(link_family(row.group)) : "DJI " + v.link;
    // What narrowed (or did not narrow) the generation, for the detail line.
    std::string generation_note = gens.ruled_out.empty() ? "" : gens.ruled_out + ". ";
    if (!gens.likely.empty()) generation_note += "The link alone allows " + join(gens.names) + ". ";
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
        if (!row.group.empty()) v.detail = family + " (" + link + ")";
        return v;
    }
    const auto* u = row.universal ? &*row.universal : nullptr;
    const auto classifier_counts = [&] {
        return std::to_string(u->named) + " of " + std::to_string(u->captures) + " captures";
    };
    if (row.packet && row.packet->encrypted) {
        v.tier = Tier::Likely;
        v.model = in_group.encrypting.empty() ? "DJI, recent model" : join(in_group.encrypting);
        v.basis = row.group.empty() ? "Encrypted DJI DroneID; no video link seen yet"
                                    : "Encrypted DJI DroneID with a " + link;
        if (!in_group.not_ruled_out.empty() && !row.group.empty())
            v.detail = "Not ruled out (DroneID behaviour unknown): " + join(in_group.not_ruled_out, ", ");
        if (u && u->accepted && !u->models.empty())
            v.detail += (v.detail.empty() ? "" : ". ") + classifier_models_text(*u);
        return v;
    }
    if (row.packet) {   // checksum-valid DroneID that is neither readable nor a known encrypted form
        v.tier = Tier::Family;
        v.model = row.group.empty() ? "DJI aircraft" : family;
        v.basis = "DJI DroneID present, identity not readable";
        if (!row.group.empty()) v.candidates = candidates_text(measured_candidates(row, false).kept);
        if (!in_group.measured.empty() && !row.group.empty())
            v.detail = generation_note + "Measured in this group: " + with_links(in_group.measured);
        return v;
    }
    if (!row.group.empty()) {
        v.tier = Tier::Family;
        v.model = family;
        v.basis = "Video link only (" + link + "); no DroneID decoded";
        if (u && u->accepted && !u->models.empty() && u->naming_carries) {
            v.model += ": likely " + join(names_of(*u));
            v.basis += "; the classifier names the model inside this family (" + classifier_counts() + ")";
            if (!u->accuracy.empty()) v.detail = "Classifier measured on unseen data: " + u->accuracy + ". ";
        } else if (u && u->accepted && !u->models.empty()) {
            v.detail = classifier_models_text(*u) + ". ";
            if (!u->accuracy.empty()) v.detail += "Measured: " + u->accuracy + ". ";
        } else if (u && u->typed) {
            v.detail = "Classifier: " + std::string(type_words(u->type)) + " agrees, model unknown (named in " +
                       classifier_counts() + "; needs " + std::to_string(minimum_named_classifier_captures) + "). ";
        }
        if (u && u->disagreements)
            v.detail += "Classifier type disagrees with the link in " + std::to_string(u->disagreements) + " of " +
                        std::to_string(u->captures) + " captures; no model taken from those. ";
        if (row.classifier_hint)
            v.detail += "Classifier hint: " + row.classifier_hint->model + " (" + std::to_string(row.classifier_hint->votes) +
                       " of " + std::to_string(row.classifier_hint->voting_captures) + " link captures). ";
        if (!u || !u->accepted || u->models.empty() || !u->naming_carries)
            v.candidates = candidates_text(measured_candidates(row, false).kept);
        v.detail += generation_note + "Measured in this group: " + with_links(in_group.measured);
        return v;
    }
    if (u && u->classifier_only && row.remote_id.empty()) {
        v.tier = Tier::Classifier;
        const bool named = u->accepted && !u->models.empty();
        v.model = named && u->naming_carries ? std::string(type_words(u->type)) + ": likely " + join(names_of(*u))
                                             : type_words(u->type);
        v.basis = "Classifier only: no link timing, DroneID or Remote ID (development model; "
                  "background false-row rate not yet established)";
        v.detail = std::to_string(u->typed) + " typed captures, models named in " + classifier_counts();
        if (named && !u->naming_carries) v.detail += ". " + classifier_models_text(*u);
        if (!u->accuracy.empty()) v.detail += ". Measured on unseen data: " + u->accuracy;
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

// The answer with its OcuSync generation when the model text does not already name it:
// "Mini 2 · OcuSync 2", "Mavic 3 or Mini 5 Pro · O3 or O4"; a Family answer already is the generation.
// A Family answer adds its candidates: "DJI OcuSync 2 or O3 · 9 candidates: Mavic Air 2, Mini 2, Mini 3 +6 more".
inline std::string model_and_link(const Verdict& v) {
    std::string out = v.link.empty() || v.model.find(v.link) != std::string::npos ? v.model : v.model + " · " + v.link;
    return v.candidates.empty() ? out : out + " · " + v.candidates;
}

}  // namespace rfmon::drone_claims
