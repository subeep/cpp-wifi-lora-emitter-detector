// Universal classifier in the Drones view: family check, agreement, verdict wording, and
// classifier-only rows (off by default) through tracks and alerts.
#include "drones/universal_claim.hpp"
#include "drones/drone_tracks.hpp"

#include <iostream>
#include <stdexcept>

using namespace rfmon;
using namespace rfmon::drone_claims;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

static ml::Snapshot snapshot() {
    ml::Snapshot s;
    s.enabled = s.ready = s.universal = true;
    s.model_hash = "pkg";
    // As in the v3 package: right on its own unit, wrong on every capture of another dataset.
    s.accuracy["Mini 4 Pro"] = ml::ModelAccuracy{40, 30, 29, 20, 1, 1, 34, 12, 0};
    return s;
}

static ml::Result decision(uint64_t capture, double centre, std::string type, bool named,
                           std::vector<ml::UniversalModel> models = {}, Clock::time_point when = {}) {
    ml::Result r;
    r.universal = true;
    r.model_hash = "pkg";
    r.run = "run";
    r.session = 1;
    r.capture = capture;
    r.center = centre;
    r.completed = when;
    r.capture_decision.present = true;
    r.capture_decision.type = std::move(type);
    r.capture_decision.named = named;
    r.capture_decision.models = std::move(models);
    return r;
}

static const std::vector<ml::UniversalModel> o4 = {{"Mini 4 Pro", .8}, {"Air 3S", .15}, {"Avata 2", .05}};

static Detection link_row(const std::string& group, std::size_t captures, double centre = 5785e6) {
    Detection row;
    row.id = "band:5 GHz/" + group + "@5785";
    row.band = "5 GHz";
    row.group = group;
    row.candidates = candidates_for(group);
    symbol_timing::Result timing;
    timing.center = centre;
    row.timing = timing;
    row.link_low_hz = row.link_high_hz = centre;
    for (std::size_t i = 0; i < captures; ++i) row.link_captures.push_back({"run", 1, 100 + i, centre});
    return row;
}

int main() try {
    // Accepted: two captures name models inside the 30 kHz (O4) family.
    {
        auto s = snapshot();
        s.history = {decision(100, 5785e6, "dji_ofdm", true, o4), decision(101, 5785e6, "dji_ofdm", true, o4),
                     decision(102, 5785e6, "dji_ofdm", false)};
        std::map<std::string, Detection> rows{{"r", link_row("30kHz", 3)}};
        add_universal_claims(rows, s);
        const auto& u = *rows["r"].universal;
        require(u.accepted && u.captures == 3 && u.typed == 3 && u.named == 2, "agreement counts wrong");
        require(u.models.size() == 3 && u.models[0].name == "Mini 4 Pro" && u.models[0].score == .8, "ranking wrong");
        // Its naming did not hold on another dataset: the answer is the family and its candidates,
        // the classifier's models a guess in the detail line.
        require(!u.naming_carries, "naming carried without an other-dataset measurement that passed");
        auto v = verdict(rows["r"]);
        require(v.tier == Tier::Family && v.model == "DJI O4" && v.candidates == "3 candidates: Mini 4 Pro, Avata 2, Air 3S" &&
                    model_and_link(v) == "DJI O4 · 3 candidates: Mini 4 Pro, Avata 2, Air 3S",
                "family with its candidates not shown");
        require(has(v.detail, "Classifier guess, not the answer") && has(v.detail, "Mini 4 Pro, Air 3S, Avata 2 (2 of 3 captures)") &&
                    has(v.detail, "right in 29 of 30") && has(v.detail, "another dataset (receiver): right in 0 of 12 named of 34"),
                "classifier guess or measured accuracy missing");
        // Measured to hold on another dataset: the classifier names the model inside the family.
        s.accuracy["Mini 4 Pro"] = ml::ModelAccuracy{40, 30, 29, 20, 1, 1, 40, 30, 28};
        add_universal_claims(rows, s);
        require(!rows["r"].universal->naming_carries, "naming carried without the names given to other drones measured");
        require(has(verdict(rows["r"]).detail, "names given to other drones not measured"), "unmeasured names-given not stated");
        // Its name also given to other drones often: right in only 28 of 38 times given.
        s.accuracy["Mini 4 Pro"].other_dataset_given_to_others = 10;
        add_universal_claims(rows, s);
        require(!rows["r"].universal->naming_carries, "a name often given to other drones carried");
        s.accuracy["Mini 4 Pro"].other_dataset_given_to_others = 2;          // 28 of 30 times given
        add_universal_claims(rows, s);
        require(rows["r"].universal->naming_carries, "naming that held on another dataset did not carry");
        v = verdict(rows["r"]);
        require(v.model == "DJI O4: likely Mini 4 Pro, Air 3S or Avata 2" && v.candidates.empty(),
                "family with a carried classifier model not shown");
        require(has(v.basis, "2 of 3 captures") && has(v.detail, "right in 29 of 30") &&
                has(v.detail, "its name given to other drones 2 times"), "basis or measured accuracy missing");
        // Measured leaving one dataset out: counted in views, and said so.
        s.accuracy["Mini 4 Pro"].other_dataset_unit = "views";
        add_universal_claims(rows, s);
        require(has(verdict(rows["r"]).detail, "right in 28 of 30 named of 40 views"), "other-dataset unit not shown");
        // Too few named captures on the other dataset carry nothing, however many were right.
        s.accuracy["Mini 4 Pro"] = ml::ModelAccuracy{40, 30, 29, 20, 1, 1, 40, minimum_other_dataset_named - 1,
                                                     minimum_other_dataset_named - 1};
        s.accuracy["Mini 4 Pro"].other_dataset_given_to_others = 0;
        add_universal_claims(rows, s);
        require(!rows["r"].universal->naming_carries && verdict(rows["r"]).model == "DJI O4", "too few other-dataset captures carried");
        // Not measured on another dataset at all: says so.
        s.accuracy["Mini 4 Pro"] = ml::ModelAccuracy{40, 30, 29, 20, 1, 1};
        add_universal_claims(rows, s);
        require(has(verdict(rows["r"]).detail, "not measured on another dataset"), "missing other-dataset test not stated");
    }
    // Masked out: models whose measured link group does not fit the row are dropped.
    {
        auto s = snapshot();
        const std::vector<ml::UniversalModel> mixed = {{"Mini 2", .6}, {"Mini 4 Pro", .3}, {"DroneRFb A (code 84)", .1}};
        s.history = {decision(100, 5785e6, "dji_ofdm", true, mixed), decision(101, 5785e6, "dji_ofdm", true, mixed)};
        std::map<std::string, Detection> rows{{"r", link_row("30kHz", 2)}};
        add_universal_claims(rows, s);
        const auto& u = *rows["r"].universal;
        require(u.accepted && u.models.size() == 2 && u.models[0].name == "Mini 4 Pro" &&
                u.models[1].name == "DroneRFb A (code 84)", "15 kHz model not masked out of a 30 kHz row");
    }
    // Disagreement: a hopping-controller answer on a 30 kHz link gives no model.
    {
        auto s = snapshot();
        s.history = {decision(100, 5785e6, "hopping_controller", true, {{"WLtoys", .9}}),
                     decision(101, 5785e6, "hopping_controller", true, {{"WLtoys", .9}})};
        std::map<std::string, Detection> rows{{"r", link_row("30kHz", 2)}};
        add_universal_claims(rows, s);
        const auto& u = *rows["r"].universal;
        require(!u.accepted && u.disagreements == 2 && u.type.empty() && u.models.empty(), "family disagreement accepted");
        const auto v = verdict(rows["r"]);
        require(v.model == "DJI O4" && has(v.detail, "disagrees with the link in 2 of 2"), "disagreement not stated");
    }
    // Too few captures: one named capture shows the type, not a model.
    {
        auto s = snapshot();
        s.history = {decision(100, 5785e6, "dji_ofdm", true, o4), decision(101, 5785e6, "dji_ofdm", false)};
        std::map<std::string, Detection> rows{{"r", link_row("30kHz", 2)}};
        add_universal_claims(rows, s);
        require(!rows["r"].universal->accepted && rows["r"].universal->models.empty(), "one named capture accepted");
        require(has(verdict(rows["r"]).detail, "model unknown (named in 1 of 2 captures; needs 2)"), "too few captures not stated");
    }
    // DroneID decides: no classifier claim on a row with a readable model; other models ignored.
    {
        auto s = snapshot();
        s.history = {decision(100, 5785e6, "dji_ofdm", true, o4), decision(101, 5785e6, "dji_ofdm", true, o4)};
        auto row = link_row("30kHz", 2);
        row.packet = Packet{"Mini 4 Pro", "X", true};
        std::map<std::string, Detection> rows{{"r", row}};
        add_universal_claims(rows, s);
        require(!rows["r"].universal, "classifier claim on a DroneID-confirmed row");
        s.history[0].model_hash = s.history[1].model_hash = "old";
        rows = {{"r", link_row("30kHz", 2)}};
        add_universal_claims(rows, s);
        require(!rows["r"].universal, "results of another model file used");
        s.universal = false;
        add_universal_claims(rows, s);
        require(!rows["r"].universal, "claims without a universal model");
    }
    // Classifier-only rows: off by default; open on a second typed capture; expire; no storm.
    {
        const Clock::time_point t0{std::chrono::seconds(1000)};
        auto s = snapshot();
        ClassifierSightings sightings;
        s.history = {decision(1, 2440e6, "hopping_controller", false, {}, t0)};
        sightings.ingest(s, t0);
        std::map<std::string, Detection> rows;
        sightings.add_rows(rows, s, t0, true);
        require(rows.empty(), "one typed capture opened a row");
        s.history.insert(s.history.begin(), decision(2, 2440.4e6, "hopping_controller", true, {{"WLtoys", .9}}, t0 + std::chrono::seconds(30)));
        sightings.ingest(s, t0 + std::chrono::seconds(30));
        sightings.ingest(s, t0 + std::chrono::seconds(31));          // the same history again: no duplicates
        require(sightings.size() == 2, "capture decisions duplicated");
        sightings.add_rows(rows, s, t0 + std::chrono::seconds(31), false);
        require(rows.empty(), "classifier-only rows shown while switched off");
        sightings.add_rows(rows, s, t0 + std::chrono::seconds(31), true);
        require(rows.size() == 1 && rows.begin()->first == "classifier:hopping_controller@2440MHz", "row not opened");
        const auto& row = rows.begin()->second;
        require(row.universal->classifier_only && row.universal->typed == 2 && !row.universal->accepted, "row claim wrong");
        const auto v = verdict(row);
        require(v.tier == Tier::Classifier && v.model == "Hopping remote controller" && has(v.basis, "not yet established"),
                "classifier-only verdict wrong");
        require(display_identity(row) == "Classifier only (hopping_controller)", "identity text wrong");

        // A link row near the slot takes precedence.
        std::map<std::string, Detection> covered{{"link", link_row("15kHz", 1, 2445e6)}};
        sightings.add_rows(covered, s, t0 + std::chrono::seconds(31), true);
        require(covered.size() == 1, "classifier row duplicated a link row");

        // Tracks: one track, one lower-severity alert; refreshes add no alerts; expiry closes nothing early.
        DroneTracks tracks;
        auto events = tracks.update(rows, t0 + std::chrono::seconds(31));
        require(events.size() == 1 && events[0].event == "opened" && events[0].lower_severity, "no single low-severity alert");
        for (int i = 32; i < 60; ++i) {
            rows.clear();
            sightings.add_rows(rows, s, t0 + std::chrono::seconds(i), true);
            for (const auto& e : tracks.update(rows, t0 + std::chrono::seconds(i)))
                require(e.event != "opened", "alert storm from a steady classifier row");
        }
        rows.clear();
        sightings.add_rows(rows, s, t0 + std::chrono::seconds(30) + classifier_row_lifetime + std::chrono::seconds(1), true);
        require(rows.empty(), "classifier row did not expire");
    }
    // Sightings stay in time order when one snapshot brings several new captures (history is newest
    // first): the older one expires first and the pair still opens a row.
    {
        const Clock::time_point t0{std::chrono::seconds(1000)};
        auto s = snapshot();
        ClassifierSightings sightings;
        s.history = {decision(3, 2440e6, "hopping_controller", false, {}, t0 + std::chrono::seconds(100)),
                     decision(2, 2440e6, "hopping_controller", false, {}, t0 + std::chrono::seconds(60)),
                     decision(1, 2440e6, "hopping_controller", false, {}, t0)};
        sightings.ingest(s, t0 + std::chrono::seconds(100));
        require(sightings.size() == 3, "sightings missing");
        std::map<std::string, Detection> rows;
        sightings.add_rows(rows, s, t0 + std::chrono::seconds(100), true);
        require(rows.size() == 1 && rows.begin()->second.universal->typed == 3, "time-ordered run not opened");
        // At t0+301 s only capture 1 is past classifier_row_merge: exactly one is dropped.
        s.history.clear();
        sightings.ingest(s, t0 + std::chrono::seconds(301));
        require(sightings.size() == 2, "expiry dropped the wrong sightings");
    }
    // C4 row rule: a capture that did not open a row (next one more than the window later) is not
    // part of the row opened after it, even inside the merge time.
    {
        const Clock::time_point t0{std::chrono::seconds(1000)};
        auto s = snapshot();
        ClassifierSightings sightings;
        s.history = {decision(3, 2440e6, "hopping_controller", false, {}, t0 + std::chrono::seconds(260)),
                     decision(2, 2440e6, "hopping_controller", false, {}, t0 + std::chrono::seconds(250)),
                     decision(1, 2440e6, "hopping_controller", true, {{"WLtoys", .9}}, t0)};
        sightings.ingest(s, t0 + std::chrono::seconds(260));
        std::map<std::string, Detection> rows;
        sightings.add_rows(rows, s, t0 + std::chrono::seconds(260), true);
        require(rows.size() == 1, "row not opened by the later pair");
        const auto& u = *rows.begin()->second.universal;
        require(u.captures == 2 && u.named == 0, "a capture before the opening joined the row");
    }
    // One decision per run, session, capture and centre: a new session reusing capture numbers counts.
    {
        auto a = decision(7, 2440e6, "dji_ofdm", false), b = a;
        b.session = 2;
        require(capture_id(a) != capture_id(b), "sessions share a capture id");
        b = a; b.center += 400;                                      // same capture, same kHz slot
        require(capture_id(a) == capture_id(b), "one capture counted twice");
    }
    // A classifier-only track's first and last seen are its captures' times, not the refresh time.
    {
        const Clock::time_point t0{std::chrono::seconds(1000)};
        auto s = snapshot();
        ClassifierSightings sightings;
        s.history = {decision(2, 2440e6, "hopping_controller", false, {}, t0 + std::chrono::seconds(30)),
                     decision(1, 2440e6, "hopping_controller", false, {}, t0)};
        const auto later = t0 + std::chrono::seconds(50);
        sightings.ingest(s, later);
        std::map<std::string, Detection> rows;
        sightings.add_rows(rows, s, later, true);
        DroneTracks tracks;
        tracks.update(rows, later);
        require(tracks.tracks().size() == 1, "no classifier track");
        const auto& track = tracks.tracks().begin()->second;
        require(track.first_seen == t0 && track.last_seen == t0 + std::chrono::seconds(30),
                "classifier track times are the refresh time, not the captures'");
    }
    // A DroneID row without a measured link covers its slot too; a Remote ID row does not (its
    // frequency is the broadcast channel, not the drone's link).
    {
        const Clock::time_point t0{std::chrono::seconds(1000)};
        auto s = snapshot();
        ClassifierSightings sightings;
        s.history = {decision(2, 2440e6, "dji_ofdm", false, {}, t0 + std::chrono::seconds(30)),
                     decision(1, 2440e6, "dji_ofdm", false, {}, t0)};
        sightings.ingest(s, t0 + std::chrono::seconds(30));
        Detection droneid;
        droneid.id = "serial:X";
        droneid.packet = Packet{"Mini 3 Pro", "X", true};
        droneid.packet->frequency = 2444.5e6;
        droneid.packet_key = {"run", 1, 9, 2440e6};
        std::map<std::string, Detection> rows{{"serial:X", droneid}};
        sightings.add_rows(rows, s, t0 + std::chrono::seconds(30), true);
        require(rows.size() == 1, "classifier row duplicated a DroneID row");
        Detection rid;
        rid.id = "rid:T";
        rid.remote_id.push_back(RemoteIdSighting{"T", "ID", "", "wifi", false, 2437e6, t0});
        rows = {{"rid:T", rid}};
        sightings.add_rows(rows, s, t0 + std::chrono::seconds(30), true);
        require(rows.size() == 2, "a Remote ID broadcast channel hid a classifier row");
    }
    std::cout << "Universal classifier claims: family check, agreement, verdicts, classifier-only rows: PASS\n";
    // Display only: models ranked but not named, with the limit each capture missed. Nothing
    // about the row's answer changes; a row DroneID names keeps the classifier as a check.
    {
        auto s = snapshot();
        const auto unnamed = [](uint64_t capture, double top, std::optional<double> distance) {
            auto r = decision(capture, 5785e6, "dji_ofdm", false, {{"Mini 2", top}, {"Mini 3", .3}, {"Air 2S", .1}});
            r.capture_decision.score_limit = .4957;
            r.capture_decision.distance_limit = 15.951;
            r.capture_decision.distance = distance;
            return r;
        };
        s.history = {unnamed(100, .40, 9.0), unnamed(101, .42, 10.0), unnamed(102, .61, 21.5)};
        std::map<std::string, Detection> rows{{"r", link_row("15kHz", 3)}};
        add_universal_claims(rows, s);
        const auto& u = *rows["r"].universal;
        require(!u.accepted && u.named == 0 && u.typed == 3 && u.models.empty(), "unnamed captures must not name a model");
        require(u.unnamed == 3 && u.below_score == 2 && u.beyond_distance == 1, "not-named reasons counted wrong");
        require(u.guesses.size() == 3 && u.guesses[0].name == "Mini 2" && std::abs(u.guesses[0].score - (.40 + .42 + .61) / 3) < 1e-9,
                "best guesses ranked by mean score over unnamed captures");
        require(u.last_score && *u.last_score == .61 && u.last_distance && *u.last_distance == 21.5 &&
                    u.score_limit && *u.score_limit == .4957,
                "latest not-named numbers kept");
        const auto v = verdict(rows["r"]);
        require(v.tier == Tier::Family && !has(v.model, "likely"), "guesses never reach the verdict");

        auto confirmed = link_row("15kHz", 3);
        Packet p;
        p.model = "Mini 2";
        p.readable = true;
        confirmed.packet = p;
        std::map<std::string, Detection> named_rows{{"c", confirmed}};
        add_universal_claims(named_rows, s);
        const auto& row = named_rows["c"];
        require(!row.universal && row.classifier_check && row.classifier_check->guesses[0].name == "Mini 2",
                "a DroneID-named row keeps the classifier only as a check");
        require(verdict(row).tier == Tier::Confirmed && verdict(row).model == "Mini 2", "DroneID still decides");
        std::cout << "PASS not-named guesses (display only) and the DroneID classifier check\n";
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
}
