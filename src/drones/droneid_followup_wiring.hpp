#pragma once
// Connects the DroneID follow-up queue to the app's detectors. Kept out of the queue's own
// header so the scanner does not depend on the detectors.
//
// In main.cpp, after `symbol_timing` and `drones` exist and before the Scanner is built:
//     rfmon::droneid_followup::Followup droneid_followup;
//     rfmon::droneid_followup::connect(droneid_followup, symbol_timing, drones);
// and after the Scanner is built:
//     scanner.set_droneid_followup(&droneid_followup);
// The queue must be declared before the scanner so it is destroyed after it.
#include "addon.hpp"
#include "droneid_followup.hpp"
#include "droneid_packet.hpp"
#include "symbol_timing_monitor.hpp"

namespace rfmon::droneid_followup {

inline double steady_seconds(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

// `timing` and `drones` must outlive `followup`'s use by the scanner.
inline void connect(Followup& followup, const symbol_timing::Monitor& timing, drone::Addon& drones) {
    followup.set_link_source([&timing] {
        std::vector<LinkSighting> out;
        for (const auto& r : timing.snapshot().history)
            if (r.timing_group) out.push_back({r.center, steady_seconds(r.observed), r.timing_group});
        return out;
    });
    followup.set_packet_source([&drones] {
        std::vector<PacketSighting> out;
        if (!drones.native.running()) return out;
        for (const auto& row : drones.native.snapshot().native_packets) {
            drone::droneid::NativePacket p;
            if (!drone::droneid::parse_native_packet(row, p)) continue;
            out.push_back({p.channel_hz > 0 ? p.channel_hz : p.center_hz, p.acquired_steady_s,
                           p.identity_readable || p.identity_encrypted});
        }
        return out;
    });
    drones.native_priority = [&followup](uint64_t capture_seq) { return followup.is_followup_capture(capture_seq); };
}

}  // namespace rfmon::droneid_followup
