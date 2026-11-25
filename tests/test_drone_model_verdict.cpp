#include "drones/model_verdict.hpp"

#include <iostream>
#include <stdexcept>

using namespace rfmon::drone_claims;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

int main() try {
    // Every model with a DroneID mode is in the model table.
    for (const auto& row : droneid_modes) require(model_link(row.model), "DroneID mode for a model missing from the table");

    Detection named;
    named.packet = Packet{"Mini 2", "5FSC", true, {}, 2, 2446e6, false, 63};
    auto v = verdict(named);
    require(v.tier == Tier::Confirmed && v.model == "Mini 2" && has(v.detail, "No video link"), "readable DroneID must confirm the model");
    named.group = "15kHz";
    require(has(verdict(named).detail, "15 kHz link fits"), "agreeing link not stated");
    named.conflict = true;
    require(has(verdict(named).detail, "does not fit"), "conflict not stated");

    Detection code;
    code.packet = Packet{"", "6C5C", true, {}, 2, 2446e6, false, 88};
    code.group = "15kHz";
    v = verdict(code);
    require(v.tier == Tier::Unknown && has(v.model, "88") && has(v.detail, "OcuSync"), "unknown code not reported with its family");

    // Encrypted DroneID narrows a group to the models seen to encrypt it.
    Detection enc;
    enc.packet = Packet{"", "", false, {}, 0, 2446e6, true};
    enc.group = "30kHz";
    v = verdict(enc);
    require(v.tier == Tier::Likely && v.model == "Mini 4 Pro or Avata 2", "30 kHz with encrypted DroneID not narrowed");
    require(has(v.detail, "Air 3S") && !has(v.model, "Air 3S"), "models of unknown DroneID behaviour must be listed apart");
    enc.group = "15kHz";
    v = verdict(enc);
    require(has(v.model, "Mini 5 Pro") && has(v.model, "Mavic 3") && !has(v.model, "Mini 2") && !has(v.detail, "Mini 2"),
            "15 kHz with encrypted DroneID must drop models that send readable DroneID");
    enc.group.clear();
    v = verdict(enc);
    require(v.tier == Tier::Likely && has(v.model, "Avata 2") && has(v.basis, "no video link"), "encrypted DroneID without a link");

    // Link only: a family, never a single model; the classifier appears only as a hint.
    Detection link;
    link.group = "32.8kHz";
    v = verdict(link);
    require(v.tier == Tier::Family && v.model == "DJI Lightbridge family" && has(v.detail, "Phantom 4") && !has(v.detail, "Classifier"),
            "link-only row must give the family");
    link.group = "30kHz";
    link.classifier_hint = ClassifierHint{"Mini 4 Pro", 3, 4};
    v = verdict(link);
    require(v.tier == Tier::Family && v.model == "DJI O4" && has(v.detail, "Classifier hint: Mini 4 Pro (3 of 4"),
            "classifier hint must stay a hint");

    Detection rid;
    rid.remote_id.push_back({"track", "1581F5FSC", "Helicopter (or Multirotor)", "Wi-Fi", true, 2437e6, {}});
    v = verdict(rid);
    require(v.tier == Tier::Reported && has(v.model, "Multirotor") && has(v.basis, "no model name"), "Remote ID row verdict");
    // Remote ID joined to a named DroneID row keeps the DroneID answer.
    named.remote_id = rid.remote_id;
    require(verdict(named).tier == Tier::Confirmed, "Remote ID must not weaken a confirmed model");


    // OcuSync generation: named instead of "OcuSync family", narrowed by DroneID, band and classifier.
    require(std::string(generation_of("O3+")) == "O3" && std::string(generation_of("O3 (FPV)")) == "O3" &&
                std::string(generation_of("O4+")) == "O4" && std::string(generation_of("OcuSync 2 Enterprise")) == "OcuSync 2" &&
                std::string(generation_of("OcuSync 1")) == "OcuSync 1" && !generation_of("OcuSync") &&
                !generation_of("Lightbridge 2") && !generation_of("Enhanced Wi-Fi"),
            "generation_of: variants fold into their generation; names without one give none");
    for (const auto& row : model_bands) require(model_link(row.model), "band table names a model missing from the link table");
    require(band_at(2444.5e6) == band_24 && band_at(5200e6) == band_51 && band_at(5785e6) == band_58 && band_at(5500e6) == 0,
            "band_at");

    Detection exact;
    exact.packet = Packet{"Mini 2", "5FSC", true, {}, 2, 2446e6, false, 63};
    exact.group = "15kHz";
    v = verdict(exact);
    require(v.model == "Mini 2" && v.link == "OcuSync 2" && v.link_short == "OS2" && link_generations(exact).exact,
            "readable DroneID gives its model's generation");
    exact.packet->model = "Mavic 3";
    require(verdict(exact).link == "O3", "O3+ model shows as O3");

    Detection o;
    o.group = "15kHz";
    v = verdict(o);
    require(v.tier == Tier::Family && v.model == "DJI OcuSync 1, OcuSync 2, O3 or O4" && v.link_short == "OS1/OS2/O3/O4",
            "15 kHz alone lists every generation measured with it, never a bare OcuSync");
    require(has(v.detail, "Mavic 3 (O3+)") && has(v.detail, "Mini 5 Pro (O4+)") && has(v.detail, "Mini 2 (OcuSync 2)"),
            "detail gives each measured model's advertised link");
    o.link_low_hz = o.link_high_hz = 5785e6;
    v = verdict(o);
    require(v.model == "DJI OcuSync 1, OcuSync 2, O3 or O4" && !has(v.detail, "Heard at"),
            "5.8 GHz rules nothing out (Mavic Pro's bands unknown, the rest use 5.8 GHz)");
    o.link_low_hz = o.link_high_hz = 5200e6;
    v = verdict(o);
    require(v.model == "DJI OcuSync 1 or O4" && has(v.detail, "Heard at 5.1 GHz: not OcuSync 2 or O3"),
            "5.1 GHz drops generations none of whose measured models use it");
    o.link_low_hz = o.link_high_hz = 0;

    Detection o3;
    o3.group = "15kHz+30kHz";
    require(verdict(o3).model == "DJI O3" && verdict(o3).link_short == "O3", "15+30 kHz is O3");
    Detection lb;
    lb.group = "32.8kHz";
    require(verdict(lb).model == "DJI Lightbridge family" && verdict(lb).link.empty(), "Lightbridge has no OcuSync generation");
    require(has(verdict(lb).basis, "Lightbridge link, 10.9 kHz timing") && display_identity(lb) == "Link only (Lightbridge timing)",
            "Lightbridge rows are worded by their link, not by the 32.8 kHz group name");

    Detection enc15;
    enc15.packet = Packet{"", "", false, {}, 0, 2446e6, true};
    enc15.group = "15kHz";
    require(verdict(enc15).link == "O3 or O4", "encrypted DroneID with 15 kHz: Mavic 3 (O3+) or Mini 5 Pro (O4+)");
    enc15.group.clear();
    require(verdict(enc15).link == "O3 or O4", "encrypted DroneID alone: the encrypting models' generations");

    Detection cls;
    cls.group = "15kHz";
    cls.universal = UniversalClaim{};
    cls.universal->accepted = true;
    cls.universal->named = cls.universal->captures = 3;
    cls.universal->models = {{"Mini 3 Pro", .7}, {"Air 2S", .2}};
    // Naming not shown to hold on another receiver: no generation hint and no model, the family's
    // candidates instead, and the classifier's models only as a guess.
    v = verdict(cls);
    require(v.link == "OcuSync 1, OcuSync 2, O3 or O4" && !has(v.model, "likely") &&
                has(v.candidates, "12 candidates: Mavic Pro, Mavic Air 2, Mavic 2 Enterprise Advanced +9 more") &&
                has(v.detail, "Classifier guess, not the answer"),
            "an unproven classifier names neither a model nor a generation");
    cls.universal->naming_carries = true;
    v = verdict(cls);
    require(v.model == "DJI O3 (from classifier): likely Mini 3 Pro or Air 2S" && v.link_short == "~O3" &&
                has(v.detail, "The link alone allows OcuSync 1, OcuSync 2, O3 or O4"),
            "classifier models sharing one generation make it the likely one, as a hint");
    cls.universal->models = {{"Air 2", .7}, {"Mini 2", .2}};
    require(verdict(cls).link == "OcuSync 2 (from classifier)", "classifier names map to table names (Air 2 = Mavic Air 2)");
    cls.universal->models = {{"Mini 2", .7}, {"Mini 3 Pro", .2}};
    require(verdict(cls).link == "OcuSync 1, OcuSync 2, O3 or O4", "classifier models across generations narrow nothing");
    cls.universal->models = {{"Mini 3 Pro", .9}, {"Phantom 4", .1}};
    require(verdict(cls).link == "OcuSync 1, OcuSync 2, O3 or O4", "a classifier model with no OcuSync generation narrows nothing");

    Detection unknown_code;
    unknown_code.packet = Packet{"", "6C5C", true, {}, 2, 2446e6, false, 88};
    unknown_code.group = "15kHz";
    require(has(verdict(unknown_code).detail, "DJI OcuSync 1, OcuSync 2, O3 or O4 (15 kHz link)"), "unknown code names the generations");

    require(std::string(tier_name(Tier::Likely)) == "Likely" && join({"a", "b", "c"}) == "a, b or c", "wording helpers");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
