#include "drones/evidence_remote_id.hpp"
#include "drones/classifier_hint.hpp"
#include "drones/evidence_packet.hpp"
#include "drones/evidence_log.hpp"
#include <unistd.h>

#include <iostream>
#include <stdexcept>

using namespace rfmon::drone_claims;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() try {
    const auto now = Clock::now();
    Store store;
    const Key key{"run1", 2, 3, 2452e6};
    Region unusable{key, {}, {}};
    require(std::string(assess(unusable, now).activity) == "uncertain", "unusable reported absence");
    rfmon::symbol_timing::Result timing;
    timing.run_id = key.run;
    timing.radio_session = key.session;
    timing.capture_seq = key.capture;
    timing.center = key.center;
    timing.observed = now - std::chrono::seconds(1);
    store.timing(timing);
    auto claim = assess(store.rows().front(), now);
    require(std::string(claim.observation) == "partial" && std::string(claim.activity) == "uncertain",
            "no group on a partial capture reported absence");
    timing.timing_group = "15kHz";
    store.timing(timing);
    claim = assess(store.rows().front(), now);
    require(claim.group == "15kHz" && !claim.candidates.empty() && claim.identities.empty(),
            "timing did not produce candidates alone");

    nlohmann::json packet = {
        {"protocol", "DJI DroneID"}, {"outer_crc24", "true"}, {"identity_readable", "true"},
        {"payload_version", "2"}, {"reported_model", "DJI Mini 4 Pro"}, {"reported_serial", "serial"},
        {"center_hz", 2452e6}, {"receiver_source", "LIVE / run run1 / session 2 / capture 3 / sample 0"},
        {"acquired_steady_s", std::chrono::duration<double>(timing.observed.time_since_epoch()).count()}
    };
    ingest_native_packet(store, packet.dump(), now);
    ingest_native_packet(store, packet.dump(), now);
    claim = assess(store.rows().front(), now);
    require(store.rows().size() == 1 && claim.conflict && claim.identities.size() == 1 &&
            claim.identities.front().serial == "serial", "matched packet or conflict was lost");
    packet["receiver_source"] = "LIVE / run run1 / session 9 / capture 3 / sample 0";
    packet["identity_readable"] = "false";
    packet["payload_version"] = 3;
    packet["reported_model"] = "";
    packet["reported_serial"] = "";
    ingest_native_packet(store, packet.dump(), now);
    claim = assess(store.rows().back(), now);
    require(store.rows().size() == 2 && claim.group.empty() && !claim.conflict &&
            claim.identities.size() == 1 && !claim.identities.front().readable &&
            claim.identities.front().version == 3 && std::string(claim.activity) == "drone link present (DroneID)",
            "checksum-valid decode should stand alone across capture sessions");
    packet["outer_crc24"] = false;
    packet["receiver_source"] = "LIVE / run run1 / session 9 / capture 4 / sample 0";
    ingest_native_packet(store, packet.dump(), now);
    require(store.rows().size() == 2, "bad CRC admitted");
    store.expire(now + lifetime);
    require(store.rows().size() == 2, "packets should outlive timing");
    store.expire(now + packet_lifetime);
    require(store.rows().empty(), "expired evidence remains current");

    const Key second{"run1", 2, 4, 2432e6};
    timing.observed = now;
    timing.timing_group = "15kHz";
    store.timing(timing);
    auto second_timing = timing;
    second_timing.capture_seq = second.capture;
    second_timing.center = second.center;
    store.timing(second_timing);
    auto rows = detections(store, now);
    require(rows.size() == 1 && rows.begin()->first == "band:2.4 GHz/15kHz@2430", "captures did not merge by band/group");

    store.decoded(key, {"Air 2S", "shared", true, now, 2, key.center});
    store.decoded(second, {"Air 2S", "shared", true, now, 2, second.center});
    rows = detections(store, now);
    require(rows.size() == 1 && rows.count("serial:shared") && !rows.at("serial:shared").conflict,
            "serial merge or mixed 15 kHz compatibility failed");

    const auto log_path = std::filesystem::temp_directory_path() /
        ("rfmon-drone-evidence-" + std::to_string(getpid()) + ".jsonl");
    {
        DetectionLog log(log_path);
        log.record(rows);
        log.record(rows);
        store.expire(now + lifetime + std::chrono::seconds(1));
        rows = detections(store, now + lifetime + std::chrono::seconds(1));
        require(rows.size() == 1 && !rows.at("serial:shared").timing, "packet expired with timing");
        log.record(rows);
        store.expire(now + packet_lifetime + std::chrono::seconds(1));
        log.record(detections(store, now + packet_lifetime + std::chrono::seconds(1)));
        require(!log.failed(), "log write failed");
    }
    std::ifstream events(log_path);
    std::string line;
    for (const auto* event : {"appeared", "updated", "expired"}) {
        require(bool(std::getline(events, line)), "missing log event");
        const auto json = nlohmann::json::parse(line);
        require(json.at("event") == event && json.at("evidence").contains("droneid"), "wrong log event");
    }
    require(!std::getline(events, line), "duplicate log event");
    std::filesystem::remove(log_path);

    store.decoded(key, {"", "", false, now, 3, key.center, true});
    store.decoded({key.run, key.session, 5, 5180e6}, {"", "unknown", true, now, 2, 5180e6, false, 87});
    rows = detections(store, now);
    require(display_identity(rows.at("band:2.4 GHz/unresolved")) ==
            "DJI DroneID present, identity encrypted", "encrypted state lost");
    require(display_identity(rows.at("serial:unknown")) == "Unknown DJI product code 87", "product code lost");

    // The link and its DroneID packet seen on different visits are one drone, not two rows.
    {
        Store s;
        auto link = timing;
        link.capture_seq = 20; link.center = 2437e6; link.observed = now; link.timing_group = "15kHz";
        s.timing(link);
        rows = detections(s, now);
        require(rows.size() == 1 && rows.count("band:2.4 GHz/15kHz@2435") && display_identity(rows.begin()->second) == "Link only (15kHz)",
                "link-only row missing");
        s.decoded({"run1", 2, 21, 2416e6}, {"Mavic Air 2", "1WNBX", true, now, 2, 2416e6, false, 58});
        rows = detections(s, now);
        require(rows.size() == 1 && rows.count("serial:1WNBX") && rows.at("serial:1WNBX").group == "15kHz" &&
                rows.at("serial:1WNBX").timing && !rows.at("serial:1WNBX").conflict, "link was not folded into the DroneID row");
        // An encrypted packet takes the link's group the same way.
        Store e;
        e.timing(link);
        e.decoded({"run1", 2, 22, 2416e6}, {"", "", false, now, 0, 2416e6, true});
        rows = detections(e, now);
        require(rows.size() == 1 && rows.begin()->second.group == "15kHz" &&
                display_identity(rows.begin()->second) == "DJI DroneID present, identity encrypted", "encrypted row did not take the link");
        // A link in the other band, or with an incompatible group, stays its own row.
        auto other = link;
        other.capture_seq = 23; other.center = 5745e6;
        s.timing(other);
        auto fast = link;
        fast.capture_seq = 24; fast.center = 2462e6; fast.timing_group = "32.8kHz";
        s.timing(fast);
        rows = detections(s, now);
        require(rows.size() == 3 && rows.count("band:5 GHz/15kHz@5745") && rows.count("band:2.4 GHz/32.8kHz@2460"), "unrelated links were folded");
        // Evidence outlives one sweep, and a later capture of the same centre without a link
        // withdraws the sighting at once.
        Store w;
        w.timing(link);
        const auto later = now + std::chrono::seconds(30);
        w.expire(later);
        require(detections(w, later).size() == 1, "a link must stay current between revisits of its centre");
        auto gone = link;
        gone.capture_seq = 25; gone.observed = later; gone.timing_group = nullptr;
        w.timing(gone);
        require(detections(w, later).empty(), "a later capture without the link did not withdraw it");
    }
    // Separate link clusters and conservative one-to-one association.
    {
        Store s;
        auto link = timing;
        link.timing_group = "15kHz";
        link.center = 2412e6;
        link.observed = now;
        link.capture_seq = 60;
        s.timing(link);
        const auto id = detections(s, now).begin()->first;
        for (const auto center : {2401e6, 2419e6}) {
            auto adjacent = link;
            adjacent.center = center;
            adjacent.capture_seq = ++link.capture_seq;
            adjacent.observed += std::chrono::seconds(1);
            s.timing(adjacent);
        }
        const auto later = now + std::chrono::seconds(2);
        rows = detections(s, later);
        require(rows.size() == 1 && rows.count(id) && rows.at(id).link_low_hz == 2401e6 &&
                rows.at(id).link_high_hz == 2419e6, "nearby centres split or changed the ID");
        auto withdrawn = link;
        withdrawn.center = 2401e6;
        withdrawn.observed = later;
        withdrawn.capture_seq = 64;
        withdrawn.timing_group = nullptr;
        s.timing(withdrawn);
        rows = detections(s, later);
        require(rows.size() == 1 && rows.count(id) && rows.at(id).link_low_hz == 2412e6,
                "withdrawing one centre removed or renamed the cluster");

        auto far = link;
        far.center = 2462e6;
        far.observed = later;
        far.capture_seq = 65;
        s.timing(far);
        require(detections(s, later).size() == 2, "distant links were merged");
        s.decoded({"run1", 2, 66, 2446e6}, {"Mini 2", "first", true, later, 2, 2446e6, false, 63});
        rows = detections(s, later);
        require(rows.size() == 3 && rows.at("serial:first").group.empty(), "ambiguous links were folded");

        Store bridged;
        for (const auto center : {2400e6, 2420e6, 2440e6}) {
            auto sample = link;
            sample.center = center;
            bridged.timing(sample);
        }
        const auto bridged_rows = detections(bridged, now);
        require(bridged_rows.size() == 1, "adjacent sightings did not form one cluster");
        withdrawn.center = 2420e6;
        bridged.timing(withdrawn);
        const auto split_rows = detections(bridged, later);
        require(split_rows.size() == 2 && split_rows.count(bridged_rows.begin()->first),
                "split clusters reused the same ID");

        Store two_packets;
        two_packets.timing(far);
        for (const auto* serial : {"first", "second"})
            two_packets.decoded({"run1", 2, 66, 2446e6}, {"Mini 2", serial, true, later, 2, 2446e6, false, 63});
        rows = detections(two_packets, later);
        require(rows.size() == 3 && rows.at("serial:first").group.empty() &&
                rows.at("serial:second").group.empty(), "two serials shared a link");
        far.observed = later + packet_lifetime;
        far.capture_seq++;
        two_packets.timing(far);
        s.timing(far);
        link.observed = far.observed;
        link.capture_seq = 68;
        s.timing(link);
        const auto after_packet = far.observed + std::chrono::seconds(1);
        two_packets.expire(after_packet);
        s.expire(after_packet);
        require(detections(two_packets, after_packet).size() == 1 &&
                detections(s, after_packet).size() == 2, "ambiguous links held a name");
    }
    // Exact capture joins, group filtering and one vote per capture.
    {
        Store s;
        auto link = timing;
        link.center = 2412e6;
        link.timing_group = "15kHz";
        link.observed = now;
        link.capture_seq = 100;
        s.timing(link);
        ++link.capture_seq;
        s.timing(link);
        rfmon::ml::Snapshot classifier;
        classifier.enabled = classifier.ready = true;
        classifier.classes = "DJI_Air_2\nDJI_Mini_2\nDJI_Mini_4_Pro\nDJI_Phantom_4_Pro_Plus\n";
        std::set<std::string> unmapped;
        const auto refresh = [&] { add_classifier_hints(rows, classifier, unmapped); };
        rfmon::ml::Result vote;
        vote.run = link.run_id;
        vote.session = link.radio_session;
        vote.center = link.center;
        vote.capture = 100;
        vote.top = {{"DJI_Air_2", .9}, {"Parrot_Disco", .09}, {"DJI_Mini_4_Pro", .01}};
        classifier.history.push_back(vote);
        vote.capture = 999;
        classifier.history.push_back(vote);
        rows = detections(s, now);
        refresh();
        refresh();
        require(unmapped.size() == 1 && !rows.begin()->second.classifier_hint,
                "unmatched capture or one vote produced a hint");
        vote.capture = 101;
        ++vote.session;
        classifier.history.push_back(vote);
        refresh();
        require(!rows.begin()->second.classifier_hint, "another session supplied a vote");
        --vote.session;
        classifier.history.push_back(vote);
        vote.interval_index = 1;
        classifier.history.push_back(vote);
        refresh();
        const auto hint = rows.begin()->second.classifier_hint;
        require(hint && hint->model == "Mavic Air 2" && hint->votes == 2 && hint->voting_captures == 2,
                "missing group-filtered capture hint");
        for (auto& result : classifier.history)
            result.top = {{"DJI_Mini_4_Pro", .89}, {"Parrot_Disco", .10}, {"DJI_Air_2", .01}};
        refresh();
        require(!rows.begin()->second.classifier_hint, "weak third candidate voted");
        for (auto& result : classifier.history)
            result.top = {{"DJI_Air_2", .49}, {"DJI_Mini_4_Pro", .48}};
        refresh();
        require(!rows.begin()->second.classifier_hint, "low-score top candidate voted");
        require(std::string(classifier_model("DJI_Avata_2")) == "Avata 2" &&
                std::string(classifier_model("DJI_Mavic_3_Pro")) == "Mavic 3 Pro", "new aliases missing");
        rows.begin()->second.group = "30kHz";
        refresh();
        require(!rows.begin()->second.classifier_hint &&
                rows.begin()->second.only_classifier_class == "Mini 4 Pro", "single class looked like a ranked hint");
        require(std::string(classifier_model("DJI_Phantom_4_Pro_Plus")) == "Phantom 4 Pro",
                "Phantom 4 Pro Plus alias missing");
        for (auto& result : classifier.history)
            result.top = {{"DJI_Air_2", .9}};
        const Key packet_key{link.run_id, link.radio_session, 101, link.center};
        s.decoded(packet_key, {"", "", false, now, 3, link.center, true});
        rows = detections(s, now);
        refresh();
        require(rows.size() == 1 && rows.begin()->second.classifier_hint, "encrypted link lost its hint");
        Store named;
        link.capture_seq = 100;
        named.timing(link);
        link.capture_seq = 101;
        named.timing(link);
        named.decoded(packet_key, {"Mavic Air 2", "named", true, now, 2, link.center});
        rows = detections(named, now);
        refresh();
        require(rows.size() == 1 && rows.count("serial:named") &&
                !rows.begin()->second.classifier_hint, "named row received a hint");
        classifier.enabled = false;
        rows = detections(s, now);
        refresh();
        require(unmapped.size() == 1 &&
                std::none_of(rows.begin(), rows.end(), [](const auto& row) { return bool(row.second.classifier_hint); }),
                "disabled classifier affected rows");
        Store packet_only;
        packet_only.decoded(packet_key, {"", "", false, now, 3, link.center, true});
        classifier.enabled = true;
        rows = detections(packet_only, now);
        refresh();
        require(!rows.begin()->second.classifier_hint, "packet-only row received a hint");
    }
    // A named drone keeps its name while a link that can be the same aircraft is still seen.
    {
        Store s;
        auto link = timing;
        link.capture_seq = 40; link.center = 2412e6; link.observed = now; link.timing_group = "15kHz";
        s.timing(link);
        s.decoded({"run1", 2, 41, 2446e6}, {"Mini 2", "5FSC", true, now, 2, 2446e6, false, 63});
        // 100 s later the packet is past its own lifetime; the link was seen again 10 s ago.
        auto again = link;
        again.capture_seq = 42; again.observed = now + std::chrono::seconds(90);
        s.timing(again);
        auto t = now + std::chrono::seconds(50);
        s.expire(t);                                   // refreshes run all the time in the app
        t = now + std::chrono::seconds(100);
        s.expire(t);
        rows = detections(s, t);
        require(rows.size() == 1 && rows.count("serial:5FSC") && rows.at("serial:5FSC").group == "15kHz",
                "the name was lost while the link was still seen");
        // The link moves to 5.8 GHz: still one row, because the table gives Mini 2 a 15 kHz link.
        auto moved = link;
        moved.capture_seq = 43; moved.center = 5805e6; moved.observed = now + std::chrono::seconds(130);
        s.timing(moved);
        t = now + std::chrono::seconds(140);
        s.expire(t);
        rows = detections(s, t);
        require(rows.size() == 1 && rows.count("serial:5FSC") && rows.at("serial:5FSC").band == "5 GHz" &&
                rows.at("serial:5FSC").group == "15kHz", "a dual-band link in the other band was not folded into the named row");
        // The hold has an end: 300 s after the packet the name goes even if a link is still seen.
        auto late = moved;
        late.capture_seq = 44; late.observed = now + std::chrono::seconds(290);
        s.timing(late);
        t = now + std::chrono::seconds(295);
        s.expire(t);
        require(detections(s, t).count("serial:5FSC"), "the name went before the hold ended");
        late.capture_seq = 45; late.observed = now + std::chrono::seconds(300);
        s.timing(late);
        t = now + std::chrono::seconds(301);
        s.expire(t);
        rows = detections(s, t);
        require(rows.size() == 1 && rows.count("band:5 GHz/15kHz@5805"), "the name was held past the hold limit");
        // No link, or a link the model does not use, holds nothing.
        Store n;
        n.decoded({"run1", 2, 46, 2446e6}, {"Mini 2", "5FSC", true, now, 2, 2446e6, false, 63});
        auto wrong = link;
        wrong.capture_seq = 47; wrong.observed = now + std::chrono::seconds(50); wrong.timing_group = "32.8kHz";
        n.timing(wrong);
        t = now + std::chrono::seconds(55);
        n.expire(t);
        rows = detections(n, t);
        require(rows.size() == 2 && rows.count("band:2.4 GHz/32.8kHz@2410") && rows.at("serial:5FSC").group.empty(),
                "a link the model does not use was folded into its row");
        t = now + std::chrono::seconds(61);
        n.expire(t);
        rows = detections(n, t);
        require(rows.size() == 1 && rows.count("band:2.4 GHz/32.8kHz@2410"), "an unrelated link held the name");
        // An encrypted packet has no model: its own band holds it, the other band does not.
        Store e;
        e.decoded({"run1", 2, 48, 2446e6}, {"", "", false, now, 0, 2446e6, true});
        auto far = link;
        far.capture_seq = 49; far.center = 5805e6; far.observed = now + std::chrono::seconds(50);
        e.timing(far);
        t = now + std::chrono::seconds(61);
        e.expire(t);
        rows = detections(e, t);
        require(rows.size() == 1 && rows.count("band:5 GHz/15kHz@5805"), "a link in the other band held an unnamed packet");
    }
    // The log reports changes in what a row says, not every new capture of the same drone.
    {
        const auto quiet_path = std::filesystem::temp_directory_path() / ("rfmon-drone-quiet-" + std::to_string(getpid()) + ".jsonl");
        std::filesystem::remove(quiet_path);
        {
            DetectionLog log(quiet_path);
            Store s;
            auto link = timing;
            link.capture_seq = 30; link.center = 2437e6; link.observed = now; link.timing_group = "30kHz";
            s.timing(link);
            log.record(detections(s, now));
            link.capture_seq = 31; link.observed = now + std::chrono::seconds(1);
            s.timing(link);
            log.record(detections(s, now + std::chrono::seconds(1)));
            log.record({});
            log.max_bytes = 1;  // the next line starts a new file; the old one is kept as .1
            log.record(detections(s, now + std::chrono::seconds(1)));
            require(!log.failed(), "quiet log write failed");
        }
        std::ifstream quiet(quiet_path);
        int lines = 0;
        while (std::getline(quiet, line)) ++lines;
        auto rotated = quiet_path; rotated += ".1";
        require(std::filesystem::exists(rotated), "log did not rotate at its size bound");
        std::ifstream old(rotated);
        while (std::getline(old, line)) ++lines;
        require(lines == 3, "expected appeared, expired, appeared and no update for a repeat sighting");
        std::filesystem::remove(quiet_path); std::filesystem::remove(rotated);
    }
    {
        const auto path = std::filesystem::temp_directory_path() /
            ("rfmon-classifier-hints-" + std::to_string(getpid()) + ".jsonl");
        std::filesystem::remove(path);
        {
            DetectionLog log(path);
            std::map<std::string, Detection> detections;
            auto& row = detections["link"];
            row.group = "30kHz";
            row.classifier_hint = ClassifierHint{"Mini 4 Pro", 2, 2};
            log.record(detections);
            row.classifier_hint = ClassifierHint{"Mini 4 Pro", 3, 4};
            log.record(detections);
            row.classifier_hint = ClassifierHint{"Air 3S", 3, 4};
            log.record(detections);
        }
        std::ifstream input(path);
        require(bool(std::getline(input, line)), "missing initial hint log");
        auto first = nlohmann::json::parse(line);
        require(first.at("evidence").at("classifier_hint").at("votes") == 2 &&
                first.at("evidence").at("identity_display") == "Link only (30kHz)",
                "hint log was not separate from identity");
        require(bool(std::getline(input, line)), "missing changed hint log");
        auto second = nlohmann::json::parse(line);
        require(second.at("event") == "updated" &&
                second.at("evidence").at("classifier_hint").at("model") == "Air 3S",
                "winner change did not update the log");
        require(!std::getline(input, line), "vote count alone triggered a log update");
        std::filesystem::remove(path);
    }
    {
        using Json = nlohmann::json;
        Json message{{"type", "Basic ID"}, {"id_type", 1}, {"reported_id", "RID123"},
                     {"ua_type_name", "Helicopter / multirotor"}, {"age_s", 1.0},
                     {"source", {{"channel_hz", 2412e6}}}};
        Json snapshot{{"enabled", true}, {"tracks", Json::array({
            Json{{"key", "wifi-track"}, {"transport", "Wi-Fi"}, {"messages", Json::array({message})}}
        })}};
        auto sightings = remote_id_sightings(snapshot, now);
        Store s;
        rows = detections(s, now, sightings);
        require(rows.size() == 1 && rows.count("rid:wifi-track") &&
                rows.begin()->second.remote_id.front().reported_id == "RID123", "Remote ID row missing");
        require(detections(s, now + remote_id_lifetime, sightings).empty(), "stale Remote ID survived");

        auto link = timing;
        link.center = 2412e6;
        link.timing_group = "15kHz";
        link.observed = now;
        s.timing(link);
        rows = detections(s, now, sightings);
        require(rows.size() == 2 && rows.count("rid:wifi-track"), "Remote ID merged with an anonymous link");
        s.decoded({"run1", 2, 500, 2412e6}, {"Mini 2", "RID123", true, now, 2, 2412e6});
        rows = detections(s, now, sightings);
        require(rows.size() == 1 && rows.count("serial:RID123") &&
                rows.begin()->second.remote_id.size() == 1, "exact serial did not merge");
        auto prefixed = sightings;
        prefixed.front().reported_id = "ABCD6RID123";
        require(detections(s, now, prefixed).size() == 1, "manufacturer-prefixed serial did not merge");
        prefixed.front().reported_id = "ABCD5RID123";
        require(detections(s, now, prefixed).size() == 2, "wrong encoded serial length matched");
        snapshot["tracks"][0]["messages"][0]["id_type"] = 2;
        require(detections(s, now, remote_id_sightings(snapshot, now)).count("rid:wifi-track"),
                "registration matched a DroneID serial");
        snapshot["tracks"][0]["messages"][0]["age_s"] = 46.0;
        require(remote_id_sightings(snapshot, now).empty(), "adapter accepted stale messages");

        const auto path = std::filesystem::temp_directory_path() /
            ("rfmon-remote-id-" + std::to_string(getpid()) + ".jsonl");
        std::filesystem::remove(path);
        {
            DetectionLog log(path);
            log.record(rows);
            auto refreshed = rows;
            refreshed.begin()->second.remote_id.front().observed += std::chrono::seconds(1);
            log.record(refreshed);
        }
        std::ifstream input(path);
        require(bool(std::getline(input, line)), "missing Remote ID log");
        const auto event = Json::parse(line);
        require(event.at("evidence").at("remote_id").at(0).at("reported_id") == "RID123" &&
                event.at("evidence").at("remote_id").at(0).at("self_reported") == true,
                "Remote ID log lost identity provenance");
        require(!std::getline(input, line), "Remote ID refresh alone changed the log");
        std::filesystem::remove(path);
    }
    // Model table: every row is usable, and only measured groups can raise a conflict.
    {
        for (const auto& row : model_links) {
            require(model_link(row.model) == &row, "duplicate model in the link table");
            const std::string group = row.group ? row.group : "";
            require(group.empty() || group == "15kHz" || group == "30kHz" || group == "15kHz+30kHz" || group == "32.8kHz",
                    "model table names a group the detector cannot report");
            require(!row.also || row.group, "second group without a first");
        }
        require(std::string(known_group("DJI Mini 4 Pro")) == "30kHz" && std::string(known_group("Phantom 4")) == "32.8kHz" &&
                std::string(known_group("DJI Mavic 3")) == "15kHz", "measured group lost");
        require(!known_group("Mavic 2") && !known_group("Mavic Mini") && !known_group("no such model"),
                "an unmeasured model must not raise conflicts");
        const auto fifteen = candidates_for("15kHz"), both = candidates_for("15kHz+30kHz");
        require(fifteen.find("Mavic Air 2 (OcuSync 2)") < fifteen.find("expected from link type") &&
                fifteen.find("Mavic 2 (OcuSync 2)") > fifteen.find("expected from link type") &&
                fifteen.find("Mavic 2 (OcuSync 2)") != std::string::npos, "15 kHz candidates not split into measured and expected");
        require(fifteen.find("Air 2S") != std::string::npos && both.find("Mavic 3 (O3+)") != std::string::npos &&
                both.find("Mini 3 Pro") == std::string::npos, "second groups not listed");
        require(candidates_for("32.8kHz").find("Phantom 4 (Lightbridge 2)") != std::string::npos &&
                candidates_for("30kHz").find("Air 3S (O4)") != std::string::npos, "group candidates missing");
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
