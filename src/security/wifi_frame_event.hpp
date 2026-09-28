// Data contracts between the receive chain and the passive Wi-Fi security
// monitor (docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md, package A step 4).
//
// Two record kinds flow from the scanner into the monitor:
//  - CaptureRecord: one per finite capture, INCLUDING empty/failed ones. It
//    is the coverage denominator: what was sampled, when, how continuously,
//    and what processing did with it. A rate is only ever "observed decoded
//    frames per usable observed second"; this record supplies both halves.
//  - FrameEvent: one FCS-valid MPDU recovered from one burst, with where it
//    sat in its capture, how its receive time is known, and the RF context.
//
// Deliberately free of UHD/DSP dependencies so the offline runner and tests
// build without a radio. Scanner converts its UHD timing into these types.
//
// Receive-time rules (never tighter than the evidence):
//  - UsrpDevice: first-sample time_spec from the radio. Sample-exact and
//    monotonic within one radio session, but counted from the radio's own
//    reset - device times are only comparable within the same
//    (run_id, radio_session). A reconnect starts a new session.
//  - HostOnly: only host wall-clock brackets are known; the uncertainty is
//    the bracket width (tens of microseconds to milliseconds), far too
//    coarse for tight TSF comparisons.
//  - After an overflow, samples are no longer contiguous with the anchor. If
//    the radio reported the resume time the anchor moves there; otherwise
//    every later sample in that capture has NO device time.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace rfmon::wifi_security {

constexpr int kEventSchema = 1;
// Legacy PSDU limits: OFDM LENGTH <= 4095 octets, 1 Mbps DSSS <= 8191.
// Anything longer is stored truncated and flagged, never silently dropped.
constexpr size_t kMaxStoredMpduBytes = 8192;

enum class ClockDomain { UsrpDevice, HostOnly, Unknown };
const char* clock_domain_name(ClockDomain d);
ClockDomain clock_domain_from_name(const std::string& s);

struct OverflowMark {
    size_t at_sample = 0;               // samples before this index are contiguous with the anchor
    std::optional<int64_t> resume_device_ns;  // device time of sample `at_sample`, if reported
};

struct CaptureRecord {
    int schema = kEventSchema;
    std::string run_id;         // unique per scanner/runner instance
    uint64_t capture_seq = 0;   // unique within run_id, increasing
    uint64_t radio_session = 0; // increments on every radio (re)connect
    std::string source = "live";  // "live" or "offline:<file>"

    std::string band;
    int channel = 0;
    double channel_hz = 0, capture_center_hz = 0;  // requested tuning
    double actual_rf_hz = 0, actual_dsp_hz = 0;    // tune result (0 = unknown)
    double requested_rate_hz = 0, sample_rate_hz = 0, requested_duration_s = 0;
    std::optional<double> gain_db;  // nullopt: AGC or unknown
    std::string antenna, device;

    size_t samples_requested = 0, samples_received = 0;
    ClockDomain clock = ClockDomain::Unknown;
    int64_t device_time_ns = 0;  // first sample; valid only when clock == UsrpDevice
    int64_t host_before_ns = 0, host_after_ns = 0;  // Unix ns bracket of the first sample (0 = unknown)
    std::vector<OverflowMark> overflows;
    bool timed_out = false, exception = false;
    bool retuned = true;  // false: same frequency as the previous capture, no tune/settle

    // Processing outcome, filled after the capture's bursts were handled.
    bool processed = false;  // false: empty/failed capture, nothing analysed
    size_t burst_cap = 0, bursts_detected = 0;
    bool burst_cap_reached = false;  // later transmissions in this capture were never looked at
    // Samples [0, analysed_samples) were examined for bursts. Equal to
    // samples_received unless the burst cap was reached, in which case it
    // ends at the last burst returned: the air after it was sampled but never
    // analysed, so it must not count as observed. 0 = not recorded.
    size_t analysed_samples = 0;
    size_t bursts_unknown = 0, bursts_narrowband = 0, bursts_dsss = 0, bursts_ofdm = 0;
    // Bursts past the identity/packet-row limit (WIFI_MAX_BURSTS_PER_CAPTURE):
    // analysed for security only.
    size_t bursts_beyond_identity_limit = 0;
    size_t dsss_decode_attempts = 0, dsss_security_only_attempts = 0, dsss_not_attempted = 0, dsss_fcs_valid = 0;
    size_t ofdm_decode_attempts = 0, ofdm_fcs_valid = 0;
    size_t events_submitted = 0, events_rejected_by_queue = 0;
    double processing_s = 0;

    double observed_seconds() const { return sample_rate_hz > 0 ? double(samples_received) / sample_rate_hz : 0.0; }
    // Seconds actually examined; a capped capture without a recorded span
    // counts as zero rather than as fully observed.
    double analysed_seconds() const {
        if (sample_rate_hz <= 0) return 0.0;
        if (analysed_samples > 0) return double(std::min(analysed_samples, samples_received)) / sample_rate_hz;
        return burst_cap_reached ? 0.0 : observed_seconds();
    }
    // Complete detector input is required for learning and declaring quiet.
    bool security_usable() const {
        return processed && contiguous() && !burst_cap_reached && events_rejected_by_queue == 0 &&
               std::isfinite(sample_rate_hz) && sample_rate_hz > 0 && analysed_seconds() > 0;
    }
    bool contiguous() const { return overflows.empty() && !timed_out && !exception && samples_received > 0; }
};

struct FrameEvent {
    int schema = kEventSchema;
    std::string run_id;
    uint64_t capture_seq = 0, radio_session = 0;
    size_t sample_start = 0, sample_length = 0;  // burst window within the capture

    std::string band;
    int channel = 0;  // monitored channel, NOT proof of the transmitter's channel
    double channel_hz = 0, capture_center_hz = 0, sample_rate_hz = 0;
    std::string phy;  // "DSSS" or "OFDM"
    int rate_mbps = 0;
    bool security_decode_only = false;

    bool fcs_valid = false;  // as reported by the decoder; re-checked on ingestion
    std::vector<uint8_t> mpdu;  // includes the trailing FCS
    bool mpdu_truncated = false;

    ClockDomain clock = ClockDomain::Unknown;
    std::optional<int64_t> device_time_ns;  // burst start, when derivable
    std::optional<int64_t> host_time_ns;    // burst start estimate from the host bracket
    int64_t host_uncertainty_ns = 0;        // +/- around host_time_ns; 0 with no host estimate
    bool after_overflow = false;            // an overflow preceded this burst in its capture

    double power_db = 0, bandwidth_hz = 0, duration_us = 0, confidence = 0;
    std::optional<double> fp_cfo_ppm, fp_irr_db, fp_iq_eps, fp_iq_phi_deg, fp_dc_dbc, fp_snr_db, fp_evm_pct,
        fp_sync_corr;
    std::optional<std::string> fp_gate_reason;  // fingerprint attempted but gated out
};

// Identity of one ingestion. Two events with the same key are the same burst
// processed twice (idempotent); the same BYTES under different keys are
// separate over-the-air transmissions and are both kept.
using IngestKey = std::tuple<std::string, uint64_t, size_t, std::string>;
inline IngestKey ingest_key(const FrameEvent& e) { return {e.run_id, e.capture_seq, e.sample_start, e.phy}; }

// Device time of a sample within a capture, following the overflow rules
// above. nullopt when not derivable.
std::optional<int64_t> device_time_of_sample(const CaptureRecord& c, size_t sample);
bool sample_after_overflow(const CaptureRecord& c, size_t sample);

// Builds an event from one decoded burst, deriving its receive time from
// the capture record. MPDU bytes beyond kMaxStoredMpduBytes are truncated
// and flagged.
FrameEvent make_frame_event(const CaptureRecord& capture, size_t sample_start, size_t sample_length,
                            const std::string& phy, int rate_mbps, bool security_decode_only,
                            const std::vector<uint8_t>& mpdu, bool fcs_valid);

// Unique-enough run identifier: hex of host time and a random value.
std::string new_run_id();

}  // namespace rfmon::wifi_security
