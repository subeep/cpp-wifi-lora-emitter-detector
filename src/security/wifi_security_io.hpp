// NDJSON serialisation for security-monitor records, shared by the live
// recorder and the offline runner so a recording replays exactly.
//
// One JSON object per line, each with "kind":
//   "header"  - schema, run_id, writer; first line of every file
//   "capture" - CaptureRecord
//   "frame"   - FrameEvent (MPDU as lowercase hex, FCS included)
//   "loss"    - queue loss notice: events/coverage records dropped before
//               reaching the monitor, with the host-time span affected
//   "command" - ControlCommand (baseline freeze/unfreeze/reset)
// Unknown kinds and unknown fields are ignored on read (forward
// compatibility); a line that fails to parse is counted, never fatal.
#pragma once

#include <nlohmann/json.hpp>

#include "security/wifi_frame_event.hpp"

namespace rfmon::wifi_security {

// Operator action applied in ingestion order, and recorded, so a replay
// reproduces it: "baseline_freeze", "baseline_unfreeze", "baseline_reset".
// `key` is a baseline key, or "" for all baselines.
struct ControlCommand {
    std::string action, key;
    int64_t host_ns = 0;
};

struct LossNotice {
    uint64_t events_dropped = 0, captures_dropped = 0;
    int64_t first_host_ns = 0, last_host_ns = 0;  // span over which drops happened
};

nlohmann::json to_json(const CaptureRecord& c);
nlohmann::json to_json(const FrameEvent& e);
nlohmann::json to_json(const LossNotice& l);
nlohmann::json to_json(const ControlCommand& c);
ControlCommand command_from_json(const nlohmann::json& j);
nlohmann::json header_json(const std::string& run_id, const std::string& writer);

// Throw nlohmann::json exceptions on missing mandatory fields.
CaptureRecord capture_from_json(const nlohmann::json& j);
FrameEvent frame_from_json(const nlohmann::json& j);
LossNotice loss_from_json(const nlohmann::json& j);

std::string to_hex(const std::vector<uint8_t>& b);
bool from_hex(const std::string& s, std::vector<uint8_t>& out);

}  // namespace rfmon::wifi_security
