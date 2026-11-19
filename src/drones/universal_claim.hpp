#pragma once
// The universal classifier in the Drones view (plan stage D, C4 v3 decision rule).
//
// The worker sends one decision per capture: a signal type (or none), the ranked models of that
// type and whether they pass the package's naming limits. Here, per row:
//   - Family check: a row with a measured link only accepts the classifier's answer when its type
//     is that link's family (15/30 kHz -> DJI OFDM, 32.8 kHz -> Lightbridge); a different type is
//     counted as a disagreement and gives no model. Named models whose measured link group does
//     not fit the row's group are dropped.
//   - Agreement: a model list is shown only when at least `minimum_named_captures` captures of
//     the row named models; the list is ranked by the mean score over those captures.
//   - Classifier-only rows (no link, DroneID or Remote ID) open when two typed captures of one
//     type fall on one band slot within `classifier_row_window`; later captures within
//     `classifier_row_merge` extend the row. They are off by default: the background false-row
//     rate is not yet established (scorecard 2026-11-14).
// The classifier never overrides DroneID or the measured family (order of evidence, plan 1).
#include "model_verdict.hpp"
#include "../ml_inference.hpp"
#include <deque>
#include <set>

namespace rfmon::drone_claims {

inline constexpr std::size_t minimum_named_captures = minimum_named_classifier_captures;
inline constexpr auto classifier_row_window = std::chrono::seconds(200);
inline constexpr auto classifier_row_merge = std::chrono::seconds(300);
inline constexpr auto classifier_row_lifetime = std::chrono::seconds(60);

inline std::string expected_type(const std::string& group) {
    if (group == "32.8kHz") return "lightbridge";
    if (group == "15kHz" || group == "30kHz" || group == "15kHz+30kHz") return "dji_ofdm";
    return {};
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

// Can a model the classifier names be on a link with this group? Unknown models (no measured
// group, or not in the table) are not ruled out.
inline bool fits_group(const std::string& model, const std::string& group) {
    const auto* link = model_link(table_name(model));
    if (!link || !link->group || !link->measured || group.empty()) return true;
    return compatible(link->group, group) || (link->also && compatible(link->also, group));
}

inline std::string accuracy_text(const ml::Snapshot& s, const std::string& model) {
    const auto it = s.accuracy.find(model);
    if (it == s.accuracy.end()) return {};
    const auto& a = it->second;
    std::string out;
    if (a.same_unit_named)
        out += "same unit: right in " + std::to_string(a.same_unit_right) + " of " +
               std::to_string(a.same_unit_named) + " named captures";
    if (a.unseen_captures) {
        if (!out.empty()) out += "; ";
        out += "other units of this model named in " + std::to_string(a.unseen_named) + " of " +
               std::to_string(a.unseen_captures) + " captures";
    }
    return out;
}

// Mean score over the named captures, ranked, at most three.
inline std::vector<UniversalModelScore> rank(const std::map<std::string, double>& sums, std::size_t named) {
    std::vector<UniversalModelScore> out;
    for (const auto& [name, sum] : sums) out.push_back({name, named ? sum / double(named) : 0.});
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (out.size() > 3) out.resize(3);
    return out;
}

inline bool matches(const ml::Result& r, const Key& key, const std::string& hash) {
    return r.universal && r.capture_decision.present && r.model_hash == hash && r.run == key.run &&
           r.session == key.session && r.capture == key.capture && std::isfinite(r.center) &&
           std::abs(r.center - key.center) <= 1e3;
}

// One capture decision: run, session, capture and centre (kHz). Used to count each decision once.
inline std::string capture_id(const ml::Result& r) {
    return r.run + "/" + std::to_string(r.session) + "/" + std::to_string(r.capture) + "/" +
           std::to_string(std::llround(r.center / 1e3));
}

// Fold capture decisions into a claim. `group` empty = classifier-only (no family check).
inline UniversalClaim claim_from(const std::vector<const ml::UniversalCapture*>& decisions,
                                 const std::string& group, const ml::Snapshot& s) {
    UniversalClaim c;
    const auto family = expected_type(group);
    std::map<std::string, std::size_t> types;
    std::map<std::string, double> sums;
    for (const auto* d : decisions) {
        ++c.captures;
        if (d->type.empty()) continue;
        if (!family.empty() && d->type != family) { ++c.disagreements; continue; }
        ++c.typed;
        ++types[d->type];
        if (!d->named) continue;
        std::vector<ml::UniversalModel> kept;
        for (const auto& m : d->models)
            if (fits_group(m.name, group)) kept.push_back(m);
        if (kept.empty()) continue;
        ++c.named;
        for (const auto& m : kept) sums[m.name] += m.score;
    }
    if (!family.empty()) c.type = c.typed ? family : "";
    else if (!types.empty()) c.type = std::max_element(types.begin(), types.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    c.accepted = c.named >= minimum_named_captures;
    if (c.accepted) {
        c.models = rank(sums, c.named);
        if (!c.models.empty()) c.accuracy = accuracy_text(s, c.models.front().name);
    }
    return c;
}

// D3: rows with a measured link get the classifier's model inside their family.
inline void add_universal_claims(std::map<std::string, Detection>& rows, const ml::Snapshot& s) {
    const bool ready = s.enabled && s.ready && s.universal;
    for (auto& [id, row] : rows) {
        if (!(row.universal && row.universal->classifier_only)) row.universal.reset();
        if (!ready || row.group.empty() || row.link_captures.empty()) continue;
        if (row.packet && row.packet->readable && !row.packet->model.empty()) continue;   // DroneID decides
        std::vector<const ml::UniversalCapture*> decisions;
        for (const auto& key : row.link_captures)
            for (const auto& r : s.history)
                if (matches(r, key, s.model_hash)) { decisions.push_back(&r.capture_decision); break; }
        if (!decisions.empty()) row.universal = claim_from(decisions, row.group, s);
    }
}

// D5: classifier-only rows. Keeps capture decisions for classifier_row_merge, because the
// worker's history holds only the last ~33 captures.
class ClassifierSightings {
public:
    struct Sighting {
        Key key;
        Clock::time_point observed;
        ml::UniversalCapture decision;
    };

    void ingest(const ml::Snapshot& s, Clock::time_point now) {
        if (!s.universal) return;
        if (s.model_hash != hash_) { sightings_.clear(); seen_.clear(); hash_ = s.model_hash; }
        for (const auto& r : s.history) {
            if (!r.universal || !r.capture_decision.present || r.model_hash != hash_ || !std::isfinite(r.center)) continue;
            const auto id = capture_id(r);
            if (!seen_.insert(id).second) continue;
            seen_order_.push_back(id);
            if (r.capture_decision.type.empty()) continue;
            // History is newest first: insert in time order (expiry and the row rule rely on it).
            Sighting x{{r.run, r.session, r.capture, r.center}, std::min(r.completed, now), r.capture_decision};
            const auto at = std::upper_bound(sightings_.begin(), sightings_.end(), x.observed,
                                             [](Clock::time_point t, const Sighting& y) { return t < y.observed; });
            sightings_.insert(at, std::move(x));
        }
        while (!sightings_.empty() && now - sightings_.front().observed > classifier_row_merge) sightings_.pop_front();
        while (seen_order_.size() > 4096) { seen_.erase(seen_order_.front()); seen_order_.pop_front(); }
    }

    // Rows for slots no link row covers. `enabled` false returns nothing (the default).
    void add_rows(std::map<std::string, Detection>& rows, const ml::Snapshot& s, Clock::time_point now, bool enabled) const {
        if (!enabled || !s.universal) return;
        std::map<std::pair<long long, std::string>, std::vector<const Sighting*>> slots;
        for (const auto& x : sightings_)
            slots[{std::llround(x.key.center / 1e6), x.decision.type}].push_back(&x);
        for (const auto& [slot, list] : slots) {
            const auto centre = double(slot.first) * 1e6;
            if (covered(rows, centre)) continue;
            // C4 row rule (universal_c4.background_rows): a row starts at a capture whose next capture
            // follows within the window; captures within the merge time extend it. A capture that
            // starts no row is not part of a later one. The latest row is the one shown.
            std::vector<const Sighting*> run;
            bool open = false;
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (open && list[i]->observed - run.back()->observed <= classifier_row_merge) { run.push_back(list[i]); continue; }
                open = false;
                run.clear();
                if (i + 1 < list.size() && list[i + 1]->observed - list[i]->observed <= classifier_row_window) {
                    open = true;
                    run.push_back(list[i]);
                }
            }
            if (!open || now - run.back()->observed > classifier_row_lifetime) continue;
            std::vector<const ml::UniversalCapture*> decisions;
            for (const auto* x : run) decisions.push_back(&x->decision);
            auto claim = claim_from(decisions, "", s);
            claim.classifier_only = true;
            claim.first_observed = run.front()->observed;
            claim.last_observed = run.back()->observed;
            const auto id = "classifier:" + slot.second + "@" + std::to_string(slot.first) + "MHz";
            auto& row = rows[id];
            row.id = id;
            row.band = centre < 3e9 ? "2.4 GHz" : "5 GHz";
            row.link_low_hz = row.link_high_hz = centre;
            row.universal = std::move(claim);
        }
    }
    std::size_t size() const { return sightings_.size(); }

private:
    static bool covered(const std::map<std::string, Detection>& rows, double centre) {
        for (const auto& [id, row] : rows) {
            if (row.universal && row.universal->classifier_only) continue;
            if (row.timing && centre >= row.link_low_hz - link_join_hz && centre <= row.link_high_hz + link_join_hz) return true;
            // DroneID without a measured link: the packet was heard on the drone's own link.
            // (Remote ID rows are not used: their frequency is the broadcast channel.)
            if (row.packet) {
                const double at = row.packet->frequency > 0 ? row.packet->frequency : row.packet_key.center;
                if (std::abs(centre - at) <= link_join_hz) return true;
            }
        }
        return false;
    }
    std::string hash_;
    std::deque<Sighting> sightings_;
    std::set<std::string> seen_;
    std::deque<std::string> seen_order_;
};

}  // namespace rfmon::drone_claims
