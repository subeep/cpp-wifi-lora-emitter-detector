// Keyless LoRaWAN structural rules over CRC-valid frames.
//
// Every rule here is chosen so that missed frames can only hide an incident,
// never create one. The receiver samples channels in finite windows, so a
// frame-counter GAP means nothing; these rules look only at things a gap
// cannot produce: a counter going backwards, the same counter carrying
// different bytes, identical bytes arriving again long after the original,
// a join nonce used twice, a burst of join requests (an observed count is a
// lower bound), and an address heard from a different RF fingerprint cluster.
//
// No keys exist here: the MIC is never verified, so a frame "from" a DevAddr
// is a frame naming that DevAddr. Incidents are evidence for review, not
// authenticated attribution.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rfmon::lora_rules {

// Repeats of identical bytes inside this window are ordinary LoRaWAN
// retransmissions (NbTrans, confirmed retries) or the same reception decoded
// on two hypotheses; later repeats are replay or counter-reset evidence.
constexpr double RETRANSMIT_WINDOW_S = 60.0;
constexpr double JOIN_FLOOD_WINDOW_S = 60.0;
constexpr size_t JOIN_FLOOD_COUNT = 15;
// An address needs this many cluster-tagged frames, mostly from one cluster,
// before a frame from another cluster is reported.
constexpr size_t RF_BASELINE_MIN = 3;
constexpr double RF_BASELINE_SHARE = 0.8;
constexpr size_t MAX_INCIDENTS = 512;
constexpr size_t MAX_TRACKED = 4096;  // per state map; least recently seen evicted

struct Frame {
    double ts = 0;  // Unix seconds of the coarse packet start
    std::vector<uint8_t> payload;
    double freq_hz = 0, bw_hz = 0;
    int sf = 0;
    std::optional<int> rf_cluster;  // persistent LoRa master cluster, when associated
    uint64_t capture_seq = 0;
};

struct Incident {
    uint64_t id = 0;
    double first_ts = 0, last_ts = 0;
    std::string rule, severity, subject, summary, evidence;
    uint64_t count = 0;
    bool open = true;
};

struct Stats {
    uint64_t frames = 0, data_frames = 0, join_requests = 0, ignored = 0, retransmissions = 0;
};

struct Snapshot {
    std::vector<Incident> incidents;  // newest first
    Stats stats;
};

// Rule names, also shown in the GUI's rule list.
inline constexpr const char* RULE_REPLAY = "Frame replayed";
inline constexpr const char* RULE_FCNT_REGRESSION = "Frame counter went backwards";
inline constexpr const char* RULE_FCNT_REUSE = "Frame counter reused with different bytes";
inline constexpr const char* RULE_DEVNONCE_REUSE = "Join DevNonce reused";
inline constexpr const char* RULE_JOIN_FLOOD = "Join-request burst";
inline constexpr const char* RULE_RF_MISMATCH = "Address heard from a different RF cluster";
inline constexpr const char* RULE_DISALLOWED = "Disallowed RF emitter heard";

class Engine {
public:
    void ingest(const Frame& frame);
    // A fingerprint reading matched a cluster the operator marked disallowed.
    void note_disallowed_emitter(int cluster, const std::string& label, double ts, double freq_hz);
    // Cluster `from` was merged into `to` in the master list.
    void remap_cluster(int from, int to);
    bool acknowledge(uint64_t id);
    void clear_closed();
    Snapshot snapshot() const;

private:
    struct FrameSeen { double first = 0, last = 0; uint64_t capture = 0; };
    struct CounterState {
        uint16_t last = 0;
        double last_ts = 0;
        std::map<uint16_t, std::string> bytes_by_fcnt;  // bounded recent counters
        std::deque<uint16_t> order;
    };
    struct RfState { std::map<int, size_t> clusters; size_t total = 0; double last_ts = 0; };

    void raise(const std::string& rule, const std::string& severity, const std::string& subject,
               const std::string& summary, const std::string& evidence, double ts);
    template <class M, class F> void bound(M& m, F last_seen);

    std::map<std::string, FrameSeen> frames_;      // payload hex -> timing
    std::map<std::string, CounterState> counters_;  // "DevAddr up/down" -> counter state
    std::map<std::string, std::map<uint16_t, double>> nonces_;  // DevEUI -> nonce -> last ts
    std::map<std::string, RfState> rf_;             // device key -> cluster tally
    std::deque<std::pair<double, std::string>> joins_;  // recent join requests (ts, DevEUI)
    double join_flood_until_ = 0;
    std::deque<Incident> incidents_;  // newest at front
    uint64_t next_id_ = 1;
    Stats stats_;
};

}  // namespace rfmon::lora_rules
