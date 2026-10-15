#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

namespace drone {
// One CRC-validated DroneID packet, timed on the USRP sample clock (continuous across retunes).
struct PacketRecord {
    uint16_t sequence{};
    double rx_time{}, frequency{}, power{};
    std::chrono::steady_clock::time_point acquired_at{};
};
struct IntervalEstimate {
    bool valid{};
    double median_s{}, min_s{}, max_s{};
    size_t pairs{};
    uint64_t received{}, expected{}; // over the accepted pair span
};
struct ChannelCount {
    double frequency{};
    size_t packets{};
};
struct BroadcastTrack {
    std::deque<PacketRecord> packets; // oldest first by rx_time
    IntervalEstimate interval;
    std::vector<ChannelCount> channels; // most packets first
};

constexpr size_t kTrackCapacity=256;
// Per-broadcast spacing outside this range means a counter reset or a bad pairing, not a rate.
constexpr double kMinStepSeconds=.01, kMaxStepSeconds=5.0;
constexpr unsigned kMaxSequenceJump=1000;

inline unsigned sequence_step(uint16_t from, uint16_t to) {
    return static_cast<uint16_t>(to-from);
}

// Median of Δt/Δsequence over consecutive packets; robust to wraparound, duplicates and resets.
inline IntervalEstimate estimate_interval(const std::deque<PacketRecord>& packets) {
    IntervalEstimate out;
    std::vector<double> steps;
    uint64_t skipped_broadcasts=0;
    for (size_t i=1;i<packets.size();++i) {
        const auto& a=packets[i-1]; const auto& b=packets[i];
        const unsigned dseq=sequence_step(a.sequence,b.sequence);
        const double dt=b.rx_time-a.rx_time;
        if (!dseq || dseq>kMaxSequenceJump || !(dt>0)) continue;
        const double step=dt/dseq;
        if (step<kMinStepSeconds || step>kMaxStepSeconds) continue;
        steps.push_back(step);
        skipped_broadcasts+=dseq-1;
    }
    if (steps.size()<2) return out;
    out.valid=true; out.pairs=steps.size();
    out.min_s=*std::min_element(steps.begin(),steps.end());
    out.max_s=*std::max_element(steps.begin(),steps.end());
    std::nth_element(steps.begin(),steps.begin()+steps.size()/2,steps.end());
    out.median_s=steps[steps.size()/2];
    out.received=out.pairs+1;
    out.expected=out.received+skipped_broadcasts;
    return out;
}

inline std::vector<ChannelCount> count_channels(const std::deque<PacketRecord>& packets) {
    std::vector<ChannelCount> out;
    for (const auto& p:packets) {
        auto it=std::find_if(out.begin(),out.end(),[&](const ChannelCount& c){return std::abs(c.frequency-p.frequency)<1000;});
        if (it==out.end()) out.push_back({p.frequency,1}); else ++it->packets;
    }
    std::stable_sort(out.begin(),out.end(),[](const ChannelCount& a,const ChannelCount& b){return a.packets>b.packets;});
    return out;
}

// Keeps rx_time order even when bursts in one batch decode out of order; drops exact re-decodes.
inline bool record_packet(BroadcastTrack& track, const PacketRecord& packet) {
    for (const auto& p:track.packets)
        if (p.sequence==packet.sequence && std::abs(p.rx_time-packet.rx_time)<1e-3) return false;
    auto at=std::upper_bound(track.packets.begin(),track.packets.end(),packet.rx_time,
        [](double t,const PacketRecord& p){return t<p.rx_time;});
    track.packets.insert(at,packet);
    if (track.packets.size()>kTrackCapacity) track.packets.pop_front();
    track.interval=estimate_interval(track.packets);
    track.channels=count_channels(track.packets);
    return true;
}

// Sequence numbers unwrapped across 16-bit rollover, for plotting against time/channel.
inline std::vector<double> unwrapped_sequences(const std::deque<PacketRecord>& packets) {
    std::vector<double> out;
    double value=0;
    for (size_t i=0;i<packets.size();++i) {
        const unsigned step=i ? sequence_step(packets[i-1].sequence,packets[i].sequence) : 0;
        value=i && step<=kMaxSequenceJump ? value+step : packets[i].sequence; // restart after a reset
        out.push_back(value);
    }
    return out;
}
}
