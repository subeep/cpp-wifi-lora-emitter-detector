// Consumer-side state of the passive Wi-Fi security monitor: ingestion,
// deduplication, parsing and coverage accounting (package A step 4).
//
// Pure and single-threaded. The live monitor drives it from its one
// consumer thread; the offline runner and tests drive it directly, so a
// recording replayed through it yields the same snapshot as the live run
// did. No detector rules yet - those are package C and will read the
// frames and coverage kept here.
//
// Guarantees (plan package A acceptance):
//  - Only an FCS-valid MPDU with a complete MAC header becomes an accepted
//    frame. The FCS is re-checked here from the bytes; the producer's flag
//    is not trusted on its own. A valid FCS over a truncated header is a real
//    transmission but cannot be attributed, so it is counted, not accepted.
//  - Ingesting the same (run, capture, sample, PHY) twice is idempotent.
//  - Identical bytes at different ingest keys are separate transmissions:
//    both are kept, the later marked `repeated_content` with a link to the
//    first. Duplicate processing and repeated transmission never merge.
//  - Everything is bounded: dedupe memory, repeat memory and the recent
//    frame buffer all have fixed capacities; forgetting is counted.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "security/wifi_frame_event.hpp"
#include "security/wifi_mac_frame.hpp"
#include "security/wifi_security_baseline.hpp"
#include "security/wifi_security_incidents.hpp"
#include "security/wifi_security_io.hpp"

namespace rfmon::wifi_security {

struct StateLimits {
    BaselineLimits baseline;
    IncidentLimits incidents;
    size_t published_incidents = 200;  // newest incidents mirrored into the snapshot
    size_t dedupe_captures = 4096;     // captures whose ingest keys are remembered
    size_t repeat_memory = 65536;      // retry-invariant hashes remembered for repeat links
    size_t recent_frames = 4096;       // accepted frames retained for inspection / later rules
    size_t loss_notices = 256;         // queue-loss notices retained
};

struct ProcessedFrame {
    FrameEvent event;
    MacFrame frame;
    bool repeated_content = false;          // same retry-invariant bytes seen before at another key
    std::optional<IngestKey> first_seen_as; // where those bytes were first seen
};

// Per monitored (band, channel) coverage. Seconds are sampled seconds, not
// wall-clock; "contiguous" excludes captures with overflow/timeout.
struct ChannelCoverage {
    std::string band;
    int channel = 0;
    uint64_t captures = 0, failed_captures = 0, overflow_captures = 0, burst_cap_captures = 0;
    uint64_t device_time_captures = 0, host_only_captures = 0;
    // sampled: received; contiguous: received without overflow/timeout;
    // analysed: actually examined for bursts (excludes air past the burst cap).
    double sampled_s = 0, contiguous_s = 0, analysed_s = 0, processing_s = 0;
    double dead_s = 0;  // unsampled device time immediately before this channel's captures
    uint64_t bursts = 0, dsss_attempts = 0, dsss_fcs_valid = 0, dsss_not_attempted = 0;
    uint64_t ofdm_attempts = 0, ofdm_fcs_valid = 0, frames_accepted = 0;
    uint64_t events_rejected_by_queue = 0;
    uint64_t bursts_beyond_identity_limit = 0;  // security-only tail past the packet-row limit
};

// Receiver timeline from device time: how much of the elapsed time within
// each radio session was actually sampled. Gaps are only measured between
// consecutive captures of one session; a gap after an overflow of unknown
// length, a restart or a new radio session is counted as unknown, never as
// observed or as a measured gap.
struct TimelineSummary {
    uint64_t radio_sessions = 0, gaps = 0, unknown_gaps = 0, out_of_order = 0, retune_skipped = 0;
    double sampled_s = 0, dead_s = 0, max_gap_s = 0;
    double duty() const { return sampled_s + dead_s > 0 ? sampled_s / (sampled_s + dead_s) : 0.0; }
};

struct SecuritySnapshot {
    int schema = kEventSchema;
    uint64_t captures_ingested = 0, captures_duplicate = 0;
    uint64_t frames_ingested = 0;       // every frame event offered
    uint64_t frames_accepted = 0;       // FCS-valid, first ingestion
    uint64_t frames_duplicate = 0;      // same ingest key again (idempotent)
    uint64_t frames_rejected_fcs = 0;   // FCS invalid / absent on re-check
    uint64_t frames_rejected_malformed = 0;  // FCS valid but MAC header truncated: not attributable
    uint64_t frames_repeated_content = 0;
    uint64_t frames_truncated_storage = 0;
    uint64_t frames_without_device_time = 0, frames_after_overflow = 0;
    uint64_t frames_security_only = 0;
    uint64_t dedupe_forgotten_captures = 0;  // captures evicted from dedupe memory
    uint64_t queue_events_dropped = 0, queue_captures_dropped = 0;
    uint64_t input_lines_rejected = 0;       // offline runner: unparseable lines
    std::map<std::string, uint64_t> frames_by_type;   // "Mgmt/Deauthentication" -> n
    std::map<std::string, uint64_t> frames_by_phy;    // "DSSS"/"OFDM"
    std::map<std::string, uint64_t> body_states;      // parser body state -> n
    std::map<std::pair<std::string, int>, ChannelCoverage> coverage;  // keyed (band, channel)
    std::deque<LossNotice> losses;                    // oldest first, bounded
    std::deque<ProcessedFrame> recent;                // oldest first, bounded
    std::string storage_error;                        // recorder problems (live monitor)
    uint64_t recorded_bytes = 0;
    TimelineSummary timeline;
    std::vector<BaselineSummary> baselines;
    bool baselines_frozen = false;                    // global freeze requested
    uint64_t baseline_pending_dropped = 0;
    std::vector<Incident> incidents;                  // newest `published_incidents`, oldest first
    size_t incidents_open = 0, incidents_total = 0;
    uint64_t incidents_evicted = 0;
    uint64_t commands_applied = 0;
    size_t baselines_restored = 0, incidents_restored = 0;  // from persisted state at start
    std::string persist_error;                        // persisted-state load/save problems (live monitor)
};

class SecurityState {
public:
    explicit SecurityState(StateLimits limits = {});
    void ingest(const CaptureRecord& c);
    // Returns true when the frame was accepted as a new FCS-valid frame.
    bool ingest(const FrameEvent& e);
    void ingest(const LossNotice& l);
    void ingest(const ControlCommand& c);
    // Detector rules (package C) report here; none exist in package B.
    uint64_t observe(const IncidentObservation& o);
    void note_input_rejected() { ++snap_.input_lines_rejected; }
    const BaselineStore& baselines() const { return baselines_; }
    const IncidentStore& incidents() const { return incidents_; }
    // Persisted across restarts: closed baseline windows and incidents.
    nlohmann::json export_persistent() const;
    void import_persistent(const nlohmann::json& j);
    // Deterministic: depends only on what was ingested, in order.
    const SecuritySnapshot& snapshot() const { return snap_; }

private:
    ChannelCoverage& coverage_for(const std::string& band, int channel);
    void refresh_baselines();
    void refresh_incidents();
    void note_timeline(const CaptureRecord& c, ChannelCoverage& cov);

    StateLimits limits_;
    SecuritySnapshot snap_;
    BaselineStore baselines_;
    IncidentStore incidents_;
    struct SessionClock { bool end_known = false; int64_t end_ns = 0; };
    std::map<std::pair<std::string, uint64_t>, SessionClock> sessions_;  // (run, radio session)
    // Dedupe: capture -> sample/PHY keys seen, evicted oldest-capture-first.
    std::map<std::pair<std::string, uint64_t>, std::set<std::pair<size_t, std::string>>> seen_frames_;
    std::deque<std::pair<std::string, uint64_t>> seen_order_;
    std::set<std::pair<std::string, uint64_t>> captures_seen_;
    std::deque<std::pair<std::string, uint64_t>> captures_order_;
    std::unordered_map<uint64_t, IngestKey> repeat_first_;
    std::deque<uint64_t> repeat_order_;
};

// Stable JSON view of a snapshot (maps are ordered, so output is
// deterministic). `include_recent` adds every retained frame with its
// parsed one-line summary.
nlohmann::json snapshot_json(const SecuritySnapshot& s, bool include_recent = false);

}  // namespace rfmon::wifi_security
