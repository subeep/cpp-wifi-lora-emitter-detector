#include "security/wifi_frame_event.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

namespace rfmon::wifi_security {

const char* clock_domain_name(ClockDomain d) {
    switch (d) {
        case ClockDomain::UsrpDevice: return "usrp_device";
        case ClockDomain::HostOnly: return "host_only";
        case ClockDomain::Unknown: return "unknown";
    }
    return "unknown";
}

ClockDomain clock_domain_from_name(const std::string& s) {
    if (s == "usrp_device") return ClockDomain::UsrpDevice;
    if (s == "host_only") return ClockDomain::HostOnly;
    return ClockDomain::Unknown;
}

namespace {
int64_t samples_to_ns(size_t samples, double rate) {
    return rate > 0 ? int64_t(std::llround(double(samples) / rate * 1e9)) : 0;
}
// Last overflow at or before `sample`, or nullptr.
const OverflowMark* governing_overflow(const CaptureRecord& c, size_t sample) {
    const OverflowMark* m = nullptr;
    for (const auto& o : c.overflows)
        if (o.at_sample <= sample) m = &o;
    return m;
}
}  // namespace

bool sample_after_overflow(const CaptureRecord& c, size_t sample) {
    return governing_overflow(c, sample) != nullptr;
}

std::optional<int64_t> device_time_of_sample(const CaptureRecord& c, size_t sample) {
    if (c.clock != ClockDomain::UsrpDevice || c.sample_rate_hz <= 0) return std::nullopt;
    const OverflowMark* o = governing_overflow(c, sample);
    if (!o) return c.device_time_ns + samples_to_ns(sample, c.sample_rate_hz);
    if (!o->resume_device_ns) return std::nullopt;  // gap length unknown
    return *o->resume_device_ns + samples_to_ns(sample - o->at_sample, c.sample_rate_hz);
}

FrameEvent make_frame_event(const CaptureRecord& capture, size_t sample_start, size_t sample_length,
                            const std::string& phy, int rate_mbps, bool security_decode_only,
                            const std::vector<uint8_t>& mpdu, bool fcs_valid) {
    FrameEvent e;
    e.run_id = capture.run_id;
    e.capture_seq = capture.capture_seq;
    e.radio_session = capture.radio_session;
    e.sample_start = sample_start;
    e.sample_length = sample_length;
    e.band = capture.band;
    e.channel = capture.channel;
    e.channel_hz = capture.channel_hz;
    e.capture_center_hz = capture.capture_center_hz;
    e.sample_rate_hz = capture.sample_rate_hz;
    e.phy = phy;
    e.rate_mbps = rate_mbps;
    e.security_decode_only = security_decode_only;
    e.fcs_valid = fcs_valid;
    e.mpdu_truncated = mpdu.size() > kMaxStoredMpduBytes;
    e.mpdu.assign(mpdu.begin(), mpdu.begin() + long(std::min(mpdu.size(), kMaxStoredMpduBytes)));

    e.clock = capture.clock;
    e.after_overflow = sample_after_overflow(capture, sample_start);
    e.device_time_ns = device_time_of_sample(capture, sample_start);
    // Host estimate: bracket midpoint plus sample offset. After an overflow
    // it is only valid if the device resume time re-anchors it.
    if (capture.host_before_ns > 0 && capture.host_after_ns >= capture.host_before_ns && capture.sample_rate_hz > 0) {
        const int64_t mid = capture.host_before_ns + (capture.host_after_ns - capture.host_before_ns) / 2;
        const int64_t half = (capture.host_after_ns - capture.host_before_ns + 1) / 2;
        if (!e.after_overflow) {
            e.host_time_ns = mid + samples_to_ns(sample_start, capture.sample_rate_hz);
            e.host_uncertainty_ns = half;
        } else if (e.device_time_ns && capture.clock == ClockDomain::UsrpDevice) {
            e.host_time_ns = mid + (*e.device_time_ns - capture.device_time_ns);
            e.host_uncertainty_ns = half;
        }
    }
    return e;
}

std::string new_run_id() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const uint64_t t = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    std::random_device rd;
    const uint32_t r = rd();
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llx-%08x", static_cast<unsigned long long>(t), r);
    return buf;
}

}  // namespace rfmon::wifi_security
