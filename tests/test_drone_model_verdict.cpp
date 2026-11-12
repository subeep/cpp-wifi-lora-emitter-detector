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
    require(v.tier == Tier::Family && v.model == "DJI O4 family" && has(v.detail, "Classifier hint: Mini 4 Pro (3 of 4"),
            "classifier hint must stay a hint");

    Detection rid;
    rid.remote_id.push_back({"track", "1581F5FSC", "Helicopter (or Multirotor)", "Wi-Fi", true, 2437e6, {}});
    v = verdict(rid);
    require(v.tier == Tier::Reported && has(v.model, "Multirotor") && has(v.basis, "no model name"), "Remote ID row verdict");
    // Remote ID joined to a named DroneID row keeps the DroneID answer.
    named.remote_id = rid.remote_id;
    require(verdict(named).tier == Tier::Confirmed, "Remote ID must not weaken a confirmed model");

    require(std::string(tier_name(Tier::Likely)) == "Likely" && join({"a", "b", "c"}) == "a, b or c", "wording helpers");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
