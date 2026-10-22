#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rfmon::wifi_master {
struct EnrolmentEvent {
    uint64_t id = 0;
    int64_t ts = 0;
    std::string kind, detail, source;
};
struct RfProfileSummary {
    std::string context, phy, source;
    std::array<double,6> centre{}, spread{}, reference{}, deviation{};
    unsigned valid = 0;
    size_t captures = 0;
    int64_t first_ts = 0, last_ts = 0;
    bool has_reference = false, review = false;
    std::vector<float> cfo_history;
};
struct WifiEnrolmentInfo {
    std::string policy = "unreviewed", alias, note, role = "Observed interface";
    bool imported = false, digital_review = false;
    uint64_t observations = 0, accepted = 0, rejected = 0, history_total = 0, reference_version = 0;
    int64_t seen_24 = 0, seen_5 = 0;
    std::string last_source, last_rejection;
    std::vector<EnrolmentEvent> history;
    std::vector<RfProfileSummary> profiles;
    std::vector<RfProfileSummary> archived_profiles; // latest 64 retired context summaries
};
struct EnrolmentCommand {
    std::string key, action, alias, note;
};
inline bool has_rf_review(const WifiEnrolmentInfo& e) {
    for(const auto& p:e.profiles)if(p.review)return true;
    for(const auto& p:e.archived_profiles)if(p.review)return true;
    return false;
}
inline const char* enrolment_label(const WifiEnrolmentInfo& e) {
    if(e.policy=="approved") return "APPROVED";
    if(e.policy=="disallowed") return "DISALLOWED";
    if(e.policy=="acknowledged") return "UNAPPROVED";
    return e.imported ? "REVIEW REQUIRED" : "NEW DEVICE";
}
}
