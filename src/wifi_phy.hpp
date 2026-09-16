// Wi-Fi modulation classification: DSSS/CCK (802.11b-style) vs OFDM
// (802.11a/g/n/ac/ax-style).
//
// This deliberately never decodes anything. It runs two independent
// correlators directly against each WiFi channel's own already-
// captured per-channel IQ buffer (see config.hpp's scan_plan_for_band()
// - this project's existing finite-capture-per-scan-step model is kept
// as-is here; no new streaming/hopping architecture):
//   - a Barker-11 matched filter, looking for the periodic peak train
//     a real 802.11b DSSS/CCK preamble produces (one peak per chip-
//     sequence-length regardless of the preamble's DBPSK bit values,
//     since matched-filter magnitude doesn't care about a data-bit
//     sign flip).
//   - a Schmidl-Cox delayed-conjugate autocorrelator, looking for the
//     sustained near-1.0 plateau a repeated OFDM short training symbol
//     (L-STF) produces.
// Both always run on every WiFi channel capture, unconditionally - a
// correlator hit IS the detection signal here, not a downstream
// bandwidth-based gate (see PROJECT_STATUS.md / session notes on why:
// this project's own noise-floor-relative, contiguous-run energy
// detector could never reliably measure >=8MHz on real, moderate-SNR
// 2.4GHz traffic, so requiring that before even trying modulation
// classification meant the correlators never got a real chance to run
// at all). Occupied bandwidth is now purely descriptive, computed via
// estimate_occupied_bandwidth_hz() below only once a correlator has
// already confirmed a hit - never used to gate detection.

#pragma once

#include <complex>
#include <vector>

namespace rfmon::wifi {

enum class ModClass { Unknown, DSSS, OFDM };

struct ModClassification {
    ModClass mod = ModClass::Unknown;
    // Mean normalized correlation of whichever correlator won, in
    // [0,1] - 0 when nothing qualified (mod == Unknown). Not a
    // probability, but genuinely comparable BETWEEN the two branches
    // and bounded, which the previous run-length ratio was not: that
    // one grew without limit with burst duration, so a longer signal
    // outranked a better-matching one, and DSSS/OFDM numbers were on
    // different scales entirely. Callers may compare these across
    // sub-captures and across modulations.
    double confidence = 0.0;

    // OFDM-only (mod == ModClass::OFDM), for src/wifi_fingerprint.cpp -
    // this is what was previously computed transiently inside
    // classify_modulation()/schmidl_cox_evidence() and then discarded.
    // Indices are into the SAME buffer the caller passed to
    // classify_modulation() as `iq` - safe to reuse directly, because
    // remove_dc()/mix_to_baseband() are both sample-index-preserving
    // (same length in, same length out, one sample in maps to exactly
    // that sample out), so an index into the internal DC-removed,
    // baseband-mixed copy is numerically identical to the same index
    // into the caller's own original `iq`.
    bool has_preamble_range = false;
    size_t l_stf_start = 0;   // where the L-STF autocorrelation plateau began
    size_t l_stf_length = 0;  // plateau run length, in samples
    // Structural estimate, not independently verified: L-STF's own
    // measured plateau end, plus the standard's 1.6us L-LTF cyclic-
    // prefix guard. Matches this project's existing pattern for
    // structural-not-measured positions (see lora_phy.cpp's SFD
    // handling) - a wrong alignment here is caught downstream by
    // wifi_fingerprint.cpp's EVM/sync_corr quality gate, not assumed
    // correct on faith.
    size_t l_ltf_start = 0;
    size_t l_ltf_length = 0;  // 2 * 3.2us worth of samples at sample_rate_hz
    // Coarse CFO from the Schmidl-Cox plateau's own delay-and-conjugate
    // sum (phase(P), the textbook estimator - previously computed
    // nowhere, only |P|^2 was used for the plateau test itself).
    // Unambiguous range is +/-1/(2*L*Ts) = +/-625kHz at the 0.8us short
    // symbol period - coarser but far more robust than the fine L-LTF
    // estimate wifi_fingerprint.cpp refines it with.
    double cfo_coarse_hz = 0.0;
};

// Minimum occupied bandwidth for a burst to be plausibly Wi-Fi at all.
//
// Both 802.11b DSSS (~22MHz main lobe) and 802.11a/g/n OFDM (~16.6MHz
// occupied) are wideband; nothing in Wi-Fi is narrow. The 2.4GHz band
// is however full of narrowband emitters - Bluetooth hops in 1MHz
// channels, BLE in 2MHz, Zigbee in 2MHz - and a Barker correlator has
// no inherent reason to reject them.
//
// This is not hypothetical: live captures produced "DSSS" rows at
// 0.23, 0.39, 0.47, 0.62 and 0.70MHz with durations of 27us to 1731us,
// which match Bluetooth packet structure far better than any Wi-Fi
// frame. Real Wi-Fi bursts in the same captures measured 6-16MHz, so
// a 4MHz floor sits in a wide empty gap between the two populations
// and does not need tuning to separate them.
constexpr double MIN_WIFI_BANDWIDTH_HZ = 4e6;

// One candidate transmission found by detect_bursts() - a slice of the
// capture where energy rose above the local noise floor for long enough
// to plausibly be a frame.
struct BurstWindow {
    size_t start = 0;   // sample index into the capture
    size_t length = 0;  // in samples
    double peak_db = 0.0;
};

// Splits a capture into individual transmission events by energy, so
// each can be classified and reported on its own instead of collapsing
// a whole buffer into one verdict per channel.
//
// This exists for two reasons at once. The obvious one is that a packet
// list needs packets. The less obvious one is cost: the correlators are
// O(N) and O(N*template_len) over whatever they are handed, and a
// 1-second capture at 20 Msps is 20 million samples, so classifying
// whole buffers made a full 2.4GHz sweep take minutes. Real frames
// occupy a tiny fraction of that - gating the correlators behind this
// cheap envelope pass cuts the work by orders of magnitude.
//
// `threshold_db` is above the measured noise floor (the 25th percentile
// of the block envelope, which stays robust when the band is busy).
// Bursts shorter than a plausible frame are dropped, and at most
// `max_bursts` are returned in chronological order.
std::vector<BurstWindow> detect_bursts(const std::complex<float>* x, size_t n,
                                        double sample_rate_hz, double threshold_db,
                                        size_t max_bursts);

// Nominal 802.11 beacon interval: 100 TU, 1 TU = 1024us. Every BSS
// beacons on this cadence with its own independent phase, which is what
// makes co-channel emitters separable without decoding anything.
constexpr double BEACON_INTERVAL_S = 0.1024;

// One inferred transmitting source on a channel, recovered by finding a
// repeating beacon cadence - NOT a decoded identity. See
// find_beacon_sources().
struct BeaconSource {
    double first_offset_s = 0.0;  // when in the capture its train starts
    double period_s = 0.0;        // measured interval, ~BEACON_INTERVAL_S
    int burst_count = 0;          // how many beacons of this train were seen
    double mean_power_db = 0.0;
};

// Groups bursts into beacon trains, so one channel can report several
// distinct sources instead of collapsing to a single row.
//
// Why timing rather than a signal property: a radio's beacon cadence is
// a per-BSS clock phase, and independent BSSes land anywhere in the
// 102.4ms window. Allowing ~2ms for CSMA deferral and TSF drift that is
// ~50 distinguishable slots - far more separating power than carrier
// frequency offset offers (~6 slots on an 8us OFDM preamble, ~25 on a
// long DSSS burst), and it needs no new DSP at all: detect_bursts()
// already reports each burst's sample offset.
//
// It also self-filters. Data frames arrive whenever traffic demands and
// so form no consistent cadence; only a genuine beacon train produces
// evenly spaced repeats, so ordinary traffic falls out rather than
// inflating the count.
//
// `burst_starts_s`/`burst_power_db` are parallel arrays of every
// classified burst in ONE capture, in any order. Trains shorter than
// `min_repeats` are discarded. Returns one entry per inferred source.
//
// IMPORTANT: this under-counts and never over-counts. Two BSSes whose
// phases coincide merge into one train, and virtual/multi-BSSID
// networks sharing one radio are physically identical here by
// construction - one transmitter, one clock. Treat the result as "at
// least N sources", never as an exact device count.
std::vector<BeaconSource> find_beacon_sources(const std::vector<double>& burst_starts_s,
                                               const std::vector<double>& burst_power_db,
                                               double tolerance_s = 1e-3, int min_repeats = 5);

// Receiver LO leakage sits at exactly the tuned centre - i.e. DC of the
// raw capture, before any mixing. Subtracting the complex mean kills
// it at the one point in the chain where its position is known exactly
// - MUST run before mix_to_baseband(), after which the spike is no
// longer at DC and a mean is no longer the right tool. Exposed (not
// file-local) so callers outside classify_modulation() - specifically
// wifi_fingerprint.cpp, which needs the SAME baseband domain
// classify_modulation() measured ModClassification's L-STF/L-LTF
// indices and CFO in, not raw un-mixed samples - can reproduce that
// domain exactly rather than approximating it.
std::vector<std::complex<float>> remove_dc(const std::vector<std::complex<float>>& iq);

// Mixes `iq` (captured at `sample_rate_hz`, centered on the tuned
// frequency) down so the candidate at `freq_offset_hz` away from that
// tuned center lands at 0 Hz. Phase starts at 0 at iq[0] and
// accumulates linearly (wrapped to +/-pi) - callers needing phase
// continuity with a PREVIOUS call against a different sub-range of the
// same physical capture (see remove_dc()'s comment on why
// wifi_fingerprint.cpp needs this) must mix one contiguous range
// covering everything they need in a single call, not stitch together
// separately-mixed pieces.
std::vector<std::complex<float>> mix_to_baseband(const std::vector<std::complex<float>>& iq,
                                                  double sample_rate_hz, double freq_offset_hz);

// Mixes `iq` (captured at `sample_rate_hz`, centered on the tuned
// frequency) down so the candidate at `freq_offset_hz` away from that
// tuned center lands at 0 Hz, decimates for correlator cost, and runs
// both correlators against the result. `try_dsss` should be false for
// 5GHz captures - Barker/CCK/DSSS does not exist in that band, so
// skipping it there both saves time and rules out a whole class of
// impossible false positives.
ModClassification classify_modulation(const std::vector<std::complex<float>>& iq,
                                       double sample_rate_hz, double capture_center_hz,
                                       double segment_center_hz, bool try_dsss);

// Peak-relative, non-contiguous occupied-bandwidth estimate, computed
// directly from raw time-domain IQ - no baseband mixing needed, since
// it's purely self-referential to wherever the signal's own peak sits.
// Adopted from a separate working prototype that measured real Wi-Fi
// channel widths this project's own detector.cpp::find_segments()
// (noise-floor-relative threshold, contiguous-run requirement, shared
// with the LoRa/sub-GHz path and NOT touched by this) couldn't reliably
// recover: a small FFT (coarser bins smooth over per-subcarrier fades)
// and a threshold relative to the signal's OWN peak - not an
// independently-estimated noise floor, which can itself be biased
// upward once a wide, weak signal already occupies much of the capture
// - report the full span between the outermost bins that clear it,
// tolerant of interior dips rather than requiring one contiguous run.
//
// `threshold_db` is relative to the peak and therefore negative (e.g.
// the default -6.0 means "the -6dB points", matching the prototype this
// was adopted from) - a positive value can never be crossed, since
// nothing exceeds the peak itself.
double estimate_occupied_bandwidth_hz(const std::complex<float>* x, size_t n,
                                       double sample_rate_hz, double threshold_db = -6.0);

// Mean power of raw IQ samples, same "10*log10(power)" family as
// spectrum.cpp's PSD - lets a correlator-confirmed detection compete on
// roughly comparable terms in DeviceRegistry's per-cycle "highest
// peak_db wins" bucket resolution (registry.cpp's update_cycle()).
// Not calibrated to numerically match PSD-per-bin power exactly (a
// different measurement - per-FFT-bin density vs. a plain time-domain
// mean) - good enough for a same-cycle ranking heuristic; revisit if
// that assumption doesn't hold up in practice.
double estimate_mean_power_db(const std::complex<float>* x, size_t n);

}  // namespace rfmon::wifi
