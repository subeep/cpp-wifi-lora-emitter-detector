// Owns the B210 and a background scan thread. Continuously scans
// whichever band is currently "active" (as set from the UI thread),
// keeping one persistent DeviceRegistry per band so switching modes and
// back doesn't lose an accumulated list - only the active band's
// registry is updated while it's selected; the other two stay frozen.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "coordinated_schedule.hpp"
#include "drones/droneid_followup.hpp"
#include "lora_capture.hpp"
#include "lora_master.hpp"
#include "lora_observation.hpp"
#include "registry.hpp"
#include "sdr_capture.hpp"
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
#include "cyclostationary/shadow_worker.hpp"
#endif
#include "security/wifi_security_monitor.hpp"
#include "security/lora_security.hpp"
#include "security/lora_rules.hpp"
#include "lora_channel_plan.hpp"
#include "serial_lane.hpp"
#include "wifi_master.hpp"
#include "wifi_enrolment.hpp"
#include "wifi_ofdm_rx.hpp"

namespace rfmon {

struct ScannerStatus {
    bool connected = false;
    std::string error;              // non-empty if SDR init failed
    std::string active_band;
    std::string active_step_label;  // which scan step is in flight right now
    int cycle_count = 0;            // cycles completed for the active band since it became active
    bool last_overflow = false;
    // True once several scan steps/LoRa listen calls in a row have come
    // back with zero samples (a real capture failure, not just an
    // ordinary "nothing above threshold" result) - see
    // Scanner::note_capture_health(). Distinct from `connected`: the
    // SDR handle is still alive, but samples have stopped flowing.
    bool rx_stalled = false;
    std::optional<int> wifi_fixed_channel;  // set when a fixed-channel request is in effect
    bool wifi_fixed_channel_invalid = false; // requested channel is not in the active band
    // Wi-Fi processing lane (capture/processing overlap; see config.hpp).
    // While it is on, cycle_count and the Wi-Fi tables are published by the
    // lane after processing, so they can trail active_step_label by one step.
    bool wifi_lane_enabled = false;
    double wifi_lane_wait_s = 0;  // cumulative time acquisition waited for the lane
    size_t wifi_lane_high_water = 0;  // max jobs queued+running (bounded: one step + one marker)
    double wifi_requested_rate_hz = 0;
    std::optional<double> wifi_actual_rate_hz; // latest nonempty capture; absent on failure/start
};

// Coordinated mode coverage ledger (upgradedplan.md §4/§6). Times are
// coord::now_steady_s() seconds. "Analysed" means admitted to the visit's
// processing lane; a lane that is still busy drops the visit (counted) so
// acquisition never waits for analysis.
struct CoordinatedSlotStatus {
    std::string band;
    bool enabled = false;
    uint64_t visits = 0, valid_visits = 0, analysed_visits = 0, lane_drops = 0;
    double acquired_s = 0, analysed_s = 0;  // valid sample time / admitted sample time
    size_t cursor = 0, list_size = 0;
    double last_visit_end_s = -1;
    // Measured average revisit per centre: time between the first and latest
    // cursor wrap, divided by the list visits made in between.
    double last_sweep_s = 0;
};
struct CoordinatedStatus {
    bool active = false;          // the scan thread is executing coordinated mode
    std::string conflict;         // non-empty: mode refused (fixed channel / LoRa lock set)
    std::string config_error;     // non-empty: schedule invalid, nothing acquired
    std::string config_hash;
    uint64_t cycle = 0;           // cycles completed in this coordinated run
    double last_cycle_s = 0;      // measured duration of the last completed cycle
    coord::Estimate estimate;     // configured cycle/sweep times (no host overhead)
    std::optional<coord::VisitRecord> current;  // visit being acquired now
    std::vector<CoordinatedSlotStatus> slots;
    uint64_t overflows = 0, empty_captures = 0;
    // DroneID follow-up visits (extra to the schedule; not counted in the slots above).
    uint64_t followup_visits = 0, followup_valid = 0, followup_analysed = 0;
    double followup_acquired_s = 0;
};

// One detected Wi-Fi transmission - a single burst found by
// wifi::detect_bursts() and then classified on its own window, rather
// than a whole-capture verdict. FCS-valid DSSS/OFDM beacon/probe responses
// additionally carry decoded identity and the persistent record key.
struct WifiPacketRow {
    std::string time;  // HH:MM:SS
    double freq_mhz = 0.0;
    int channel = 0;
    std::string modulation;  // "DSSS" or "OFDM"
    double power_db = 0.0;
    double bandwidth_khz = 0.0;
    double duration_us = 0.0;
    double confidence = 0.0;  // [0,1], comparable across modulations
    std::optional<wifi::BeaconInfo> identity;  // FCS-valid decoded beacon/probe response
    std::string decode_status;
    int ofdm_rate_mbps = 0;
    size_t psdu_length = 0;
    bool fcs_valid = false;  // recovered MPDU passed its FCS (any frame type, DSSS or OFDM)
    std::string enrolment_source; // exact burst key for asynchronous inventory association
    std::string master_key;  // persistent BSSID or accepted fingerprint-cluster key
    // MAC type/subtype of an FCS-valid MPDU, e.g. "Mgmt/Deauthentication"
    // (security/wifi_mac_frame.hpp). Empty when nothing FCS-valid was recovered.
    std::string frame_type;
    // Short DSSS burst decoded for security analysis only: never offered to
    // beacon-cadence clustering, identity recording or fingerprinting (see
    // wifi_burst_policy.hpp), even when it decodes as a beacon.
    bool security_decode_only = false;

    // RF fingerprint (see wifi_fingerprint.hpp) - unset when extraction
    // wasn't attempted for this row (OFDM without a usable L-LTF range,
    // or a DSSS burst too short to be beacon-plausible - see
    // scanner.cpp's own comment on that scope limit) or was gated out
    // (fp_gate_reason set in that case). irr_db/iq_eps/iq_phi_deg stay
    // unset on every DSSS row even when the rest of the fingerprint
    // passed - see wifi_fingerprint.cpp's own comment on why a
    // real-valued BPSK reference can't resolve those three via this
    // technique, not a gating decision.
    std::optional<double> fp_cfo_ppm;
    std::optional<double> fp_irr_db;
    std::optional<double> fp_iq_eps;
    std::optional<double> fp_iq_phi_deg;
    std::optional<double> fp_dc_dbc;
    std::optional<double> fp_dc_ang_deg;
    std::optional<double> fp_snr_db;
    std::optional<double> fp_evm_pct;
    std::optional<double> fp_sync_corr;
    std::optional<std::string> fp_gate_reason;  // set only when attempted but gated out
    // 802.11n/ac/ax header of an OFDM burst whose payload format is not
    // decoded ("" for legacy / DSSS), and the networks it points to: by
    // HE BSS colour or VHT partial AID matched against beacons decoded in
    // this session on overlapping channels. A hint, not an identity.
    std::string phy_format;
    std::string network_hint;
};

class Scanner {
public:
    using WifiIqObserver = std::function<void(const std::vector<std::complex<float>>&, const wifi_security::CaptureRecord&)>;
    using WifiFrameObserver = std::function<void(const wifi_security::FrameEvent&)>;
    Scanner();
    // Optional immutable processing-tail observer; configured before any receiver starts.
    explicit Scanner(WifiIqObserver observer, WifiFrameObserver frames = {}) : Scanner() { wifi_iq_observer_ = std::move(observer); wifi_frame_observer_ = std::move(frames); }
    // Same as Scanner(), with every persistent store rooted at `data_root`
    // instead of <project>/data (tests use a temporary directory).
    explicit Scanner(const std::string& data_root);
    // Both: observers plus a separate data root (verification tools).
    Scanner(const std::string& data_root, WifiIqObserver observer, WifiFrameObserver frames = {}) : Scanner(data_root) {
        wifi_iq_observer_ = std::move(observer);
        wifi_frame_observer_ = std::move(frames);
    }
    ~Scanner();
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
    bool set_wifi_cyclo_enabled(bool enable) { return cyclo_shadow_.set_enabled(enable); }
    cyclo::ShadowStats wifi_cyclo_stats() const { return cyclo_shadow_.stats(); }
    std::shared_ptr<const cyclo::ShadowResult> wifi_cyclo_snapshot() const { return cyclo_shadow_.snapshot(); }
#endif

    void start();  // spawns the background thread and connects to the SDR
    void stop();   // signals the thread to stop and joins it

    void set_active_band(const std::string& band);
    std::string active_band() const;

    // Which physical SDR to connect to. Only takes effect on the next
    // start() - changing it while running does not hot-swap hardware;
    // the caller should stop(), set_device_type(), then start() again
    // (see main.cpp's device selector, which does exactly this).
    void set_device_type(SdrDeviceType type);
    SdrDeviceType device_type() const;

    // Wi-Fi IQ output rate, shared by both Wi-Fi bands. Configure while stopped;
    // nullopt restores the existing device default. Explicit requests bypass the
    // historical transport default; UHD reports the actual supported rate.
    static bool valid_wifi_sample_rate(std::optional<double> hz);
    bool set_wifi_sample_rate(std::optional<double> hz);
    std::optional<double> wifi_sample_rate() const;

    void set_threshold_db(double db);
    double threshold_db() const;

    void set_gain(std::optional<double> gain_db);  // nullopt = AGC
    std::optional<double> gain() const;

    // Pins the LoRa PHY listen step to one exact frequency every cycle
    // instead of rotating LORA_LISTEN_CHANNELS_HZ - useful when a known
    // real device's configured frequency doesn't fall on that list (see
    // newrocktest/TARANGMINI_LORA_FINDINGS.md for how this comes up in
    // practice). nullopt (default) = cycle all channels as usual.
    // One-shot save of the next LoRa capture, consumed by the scan worker.
    void request_lora_capture_save(const std::string& directory);
    std::string lora_capture_message() const;
    bool lora_capture_pending() const;
    void set_lora_lock_freq(std::optional<double> freq_hz);
    std::optional<double> lora_lock_freq() const;

    // Fixed-channel Wi-Fi monitoring (security plan, package B): stay on one
    // channel of the active Wi-Fi band instead of sweeping, so a channel is
    // observed near-continuously rather than for ~4 s per sweep. nullopt =
    // sweep all channels (default). A channel not in the active band's plan
    // leaves the sweep unchanged; the status line reports it.
    void set_wifi_fixed_channel(std::optional<int> channel);
    std::optional<int> wifi_fixed_channel() const;

    // Explicit opt-in to the old experimental codecs; production never falls back.
    void set_lora_laboratory_mode(bool skip);
    bool lora_laboratory_mode() const;
    void set_lora_capture_seconds(double seconds);
    double lora_capture_seconds() const;

    // LoRa listen channel plan (default LORA_LISTEN_CHANNELS_HZ). With
    // wideband listening on, channels that fit in one capture are received
    // together and derived per channel (lora_channel_plan.hpp); off, one
    // channel per cycle as before. A frequency lock overrides both.
    // Coordinated mode keeps its own derived channels.
    void set_lora_channels(std::vector<double> channels_hz);
    std::vector<double> lora_channels() const;
    void set_lora_wideband_listen(bool on);
    bool lora_wideband_listen() const;
    // Wideband capture width: nullopt = narrowest rate that fits the plan;
    // default 20 MHz. A capture that also covers 863-868 MHz replaces the
    // separate sub-GHz energy scan.
    void set_lora_wideband_rate(std::optional<double> rate_hz);
    std::optional<double> lora_wideband_rate() const;
    // Whether the current plan's single capture stands in for the energy scan.
    bool lora_capture_replaces_energy_scan() const;
    // The capture rotation the next cycles will use with current settings.
    std::vector<lora_plan::Capture> lora_listen_plan() const;
    using LoraListenStats = lora_plan::ListenStats;
    LoraListenStats lora_listen_stats() const;

    // Keyless LoRaWAN rules (security/lora_rules.hpp), fed by CRC-valid
    // frames from every LoRa decode path.
    lora_rules::Snapshot lora_incidents() const;
    bool acknowledge_lora_incident(uint64_t id);
    void clear_closed_lora_incidents();

    // Operator review of persistent LoRa clusters (lora_master.hpp).
    bool set_lora_enrolment(int device_id, const std::string& policy, const std::string& alias,
                            const std::string& note);
    bool merge_lora_emitters(int into, int from);
    std::vector<lora_master::LoraMasterReading> lora_emitter_history(int device_id) const;
    std::string lora_master_storage_error() const { return lora_master_.storage_error(); }

    std::vector<DeviceRow> snapshot(const std::string& band) const;
    ScannerStatus status() const;

    // Coordinated mode (BAND_COORDINATED). The schedule applies at the next
    // cycle boundary and resets the rotating cursors; nullopt restores the
    // device default (coord::default_schedule at the Wi-Fi rate in use).
    void set_coordinated_schedule(std::optional<coord::Schedule> schedule);
    std::optional<coord::Schedule> coordinated_schedule() const;
    // The schedule a coordinated run would use with the current settings.
    coord::Schedule effective_coordinated_schedule() const;
    CoordinatedStatus coordinated_status() const;
    // Recent visits (newest last, bounded), for coverage-aware freshness.
    std::deque<coord::VisitRecord> coordinated_visits() const;
    static double now_steady_s();
    // Optional DroneID follow-up queue (coordinated mode only). Each cycle the scanner asks it
    // for extra drone-only visits and reports the captures they produced. Not owned; must
    // outlive the scanner or be cleared with nullptr. Safe to set while running.
    void set_droneid_followup(droneid_followup::Followup* followup) { droneid_followup_.store(followup); }

    // Opt-in evidence retention: after the Wi-Fi processing lane has finished
    // with a Wi-Fi/drone capture, keep its IQ (moved, not copied) for the most
    // recent `captures` captures so an entry's exact samples can be exported.
    // 0 (default) keeps nothing. Memory: captures x one capture's fc32 IQ.
    struct RetainedCapture {
        wifi_security::CaptureRecord record;
        std::shared_ptr<const std::vector<std::complex<float>>> iq;
    };
    void set_iq_retention(size_t captures);
    size_t iq_retention() const;
    std::optional<RetainedCapture> retained_capture(uint64_t capture_seq) const;
    std::vector<uint64_t> retained_capture_seqs() const;  // oldest first

    // Only populated while BAND_SUB_GHZ is active - see run()'s LoRa
    // PHY listen sub-loop, which runs alongside the usual energy scan.
    std::vector<LoraPacketRow> lora_packets() const;
    // LoRaWAN end devices named by CRC-valid frames this session (bounded,
    // least recently seen dropped first). Not persisted.
    std::vector<LoraDeviceRow> lora_devices() const;
    std::shared_ptr<const lora_security::Snapshot> lora_security_snapshot() const;
    bool set_lora_security_recording(const std::string& path); // before start only

    // Persistent, cross-run LoRa "master emitter" list (see
    // lora_master.hpp) - a separate identity system from registry_lora_
    // above; fed only by non-gated LoRa fingerprint readings, and never
    // forgets anything across restarts.
    std::vector<lora_master::LoraMasterRow> lora_master_snapshot() const;

    // Only populated while a Wi-Fi band is active - one row per
    // individually detected and classified burst.
    std::vector<WifiPacketRow> wifi_packets() const;
    // OFDM PPDUs seen this session by format ("802.11a/g legacy", "802.11n HT",
    // "802.11ac VHT", "802.11ax HE SU", ...); headers with a failed CRC are
    // counted under "<format> (header CRC failed)".
    std::map<std::string, uint64_t> wifi_phy_format_counts() const;

    // How many distinct transmitting sources were resolved on each
    // Wi-Fi channel, keyed by channel number (see
    // wifi::find_beacon_sources). This is an inferred LOWER bound from
    // beacon timing, never a decoded device count - co-phased BSSes
    // merge, and virtual/multi-BSSID networks on one radio are
    // physically indistinguishable here by construction.
    std::map<int, int> wifi_source_counts(const std::string& band) const;

    // Persistent, cross-run Wi-Fi "master emitter" list (see
    // wifi_master.hpp) - the Wi-Fi analog of lora_master_snapshot()
    // above, fed by FCS-valid identities and accepted RF readings independently. Keyed
    // by decoded MAC/BSSID when one is available (DSSS and legacy OFDM
    // beacons/probe responses), fingerprint-cluster otherwise.
    std::vector<wifi_master::WifiMasterRow> wifi_master_snapshot() const;
    bool wifi_enrolment_command(wifi_master::EnrolmentCommand c) { return wifi_master_.command(std::move(c)); }
    wifi_master::EnrolmentHealth wifi_enrolment_health() const { return wifi_master_.health(); }
    void flush_wifi_enrolment() { wifi_master_.flush(); }
    std::string wifi_storage_error() const { return wifi_master_.storage_error(); }

    // Passive Wi-Fi security monitor (docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md,
    // package A step 4): every Wi-Fi capture's coverage record and every
    // FCS-valid MPDU are submitted to it without blocking the scan thread.
    // Observations only - no detector rules exist yet.
    std::shared_ptr<const wifi_security::SecuritySnapshot> wifi_security_snapshot() const;
    wifi_security::QueueStats wifi_security_queue_stats() const;
    const std::string& wifi_security_run_id() const { return security_run_id_; }
    // Opt-in NDJSON recording of everything the monitor ingests, bounded to
    // about `max_bytes` on disk (current file + one rotated ".1"). Replay it
    // with tools/wifi_security_replay. Only takes effect before start();
    // returns false if the scanner is already running.
    bool set_wifi_security_recording(const std::string& path, uint64_t max_bytes = uint64_t(64) << 20);
    // Baseline review actions ("baseline_freeze" / "baseline_unfreeze" /
    // "baseline_reset"; key "" = all). Applied in order with ingested data.
    bool wifi_security_command(const std::string& action, const std::string& key = "");

private:
    // Beacons decoded this session, for attributing 802.11ac/ax headers.
    struct BssFacts { std::string ssid; double channel_hz = 0; int bss_color = -1; };
    mutable std::mutex bss_mutex_;
    std::map<std::string, BssFacts> bss_index_;            // by BSSID
    std::map<std::string, uint64_t> phy_format_counts_;
    void note_beacon(const wifi::BeaconInfo& b, double fallback_hz);
    std::string network_hint(const wifi::HighThroughputSig& s, double channel_hz) const;
    WifiIqObserver wifi_iq_observer_;
    WifiFrameObserver wifi_frame_observer_;
    friend struct ScannerTestAccess;  // radio-free tests (tests/test_scanner_wifi_lane.cpp)
    void run();
    // Appends any energy-detected segment(s) found in this same
    // capture to `detections` (see .cpp) - reuses the one capture
    // already taken here for the dedicated codec attempts, rather than
    // requiring a second wideband capture just to keep the Active
    // emitters table populated while locked to one frequency.
    void run_lora_listen_step(double freq_hz, const DeviceProfile& profile, double threshold_db,
                               std::vector<Detection>& detections);
    // One capture covering several planned channels, each derived at the
    // 500 kHz decode rate and decoded like a single-channel listen.
    void run_lora_wideband_step(const lora_plan::Capture& plan, const DeviceProfile& profile,
                                double threshold_db, std::vector<Detection>& detections);
    // A LoRa listen split into its two halves so the scan loop can take the
    // next capture while the previous one is decoded (run(): one serial
    // worker, so commits stay in capture order). capture_lora() touches only
    // the radio; process_lora() does everything the step did after capturing.
    struct LoraCaptured {
        lora_plan::Capture plan;  // wideband, or one channel at the decode rate
        CaptureResult received;
        double host_start = 0, capture_seconds = 0, steady_captured = 0;
        std::optional<double> gain;
        uint64_t radio_session = 0;
    };
    LoraCaptured capture_lora(const lora_plan::Capture& plan, const DeviceProfile& profile);
    void process_lora(LoraCaptured& captured, const DeviceProfile& profile, double threshold_db,
                      std::vector<Detection>& detections);
    void process_lora_wideband(LoraCaptured& captured, const DeviceProfile& profile, double threshold_db,
                               std::vector<Detection>& detections);
    void append_lora_energy_segments(const std::vector<std::complex<float>>& iq, double rate_hz,
                                     double center_hz, double threshold_db, std::vector<Detection>& detections);
    void note_lora_listen(const lora_plan::Capture& plan, double capture_s, double decode_s);
    double lora_wideband_max_rate(const DeviceProfile& profile) const;
    lora_plan::PlanOptions lora_plan_options(const DeviceProfile& profile) const;
    // LoRa decode of one capture (provenance carries rate/centre/timing):
    // analyze_lora_iq() (pure, parallel per bandwidth) then
    // commit_lora_analysis() (stateful, scan thread).
    void decode_lora_iq(const std::vector<std::complex<float>>& iq, const LoraCapture& provenance,
                        double threshold_db, std::vector<Detection>& detections, bool energy_segments);
    struct LoraAnalysis;
    static LoraAnalysis analyze_lora_iq(const std::vector<std::complex<float>>& iq, double rate_hz, double freq_hz,
                                        bool laboratory_mode);
    void commit_lora_analysis(const std::vector<std::complex<float>>& iq, const LoraCapture& provenance,
                              const LoraAnalysis& analysis, double threshold_db, std::vector<Detection>& detections,
                              bool energy_segments);
    DeviceRegistry& registry_for(const std::string& band);
    // A Wi-Fi step handed from acquisition to the processing lane, or (with
    // cycle_end set) the end-of-cycle status marker for a Wi-Fi cycle.
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
    struct CycloCaptureInput { cyclo::CaptureContext context; std::size_t prefix = 0; };
    using CycloCaptureInputs = std::array<CycloCaptureInput, SUB_CAPTURES_PER_STEP>;
#endif
    struct WifiStepJob {
        ScanStep step{};
        std::string band;
        double threshold = 0, actual_rate = 0;
        std::vector<std::vector<std::complex<float>>> captures;
        std::vector<wifi_security::CaptureRecord> capture_records;  // index-aligned with captures
        std::vector<wifi_security::CaptureRecord> empty_records;    // failed/empty captures, in capture order
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
        CycloCaptureInputs cyclo_inputs{}; // bounded metadata only; no IQ ownership
#endif
        bool cycle_end = false;
        int cycle_count = 0;
        bool last_overflow = false;
        // Coordinated drone-only visit: hand the capture to the IQ observers
        // (drones/ML) only; no Wi-Fi PHY, security or registry work.
        bool observer_only = false;
    };
    void run_wifi_lane_job(WifiStepJob& job);
    // Coordinated sub-GHz visit, processed off the acquisition thread:
    // energy segments plus LoRa decoding on every configured LoRa channel
    // inside the usable passband, derived from this one capture.
    struct SubGhzVisitJob {
        ScanStep step{};
        double threshold = 0, actual_rate = 0, host_start_unix_s = 0;
        std::optional<double> gain_db;
        std::vector<std::vector<std::complex<float>>> captures;  // exactly one
        CaptureTiming timing;
        bool overflow = false;
        uint64_t radio_session = 0;
        std::string device_args, antenna;
        bool cycle_end = false;  // last sub-GHz visit of the cycle: commit the registry
    };
    void run_subghz_visit_job(SubGhzVisitJob& job);
    std::vector<Detection> coordinated_subghz_detections_;  // sub-GHz lane only
    // Runs coordinated cycles until the mode changes or stop is requested.
    // Owns its own processing lanes and drains them before returning.
    void run_coordinated(const DeviceProfile& profile, std::optional<double> wifi_rate_override);
    // One step's spectrum/segments and (Wi-Fi) burst processing through the
    // per-hop registry flush - the former inline block of run(), unchanged.
    void process_step_captures(const ScanStep& step, const std::string& band, double threshold,
                               double actual_rate, const std::vector<std::vector<std::complex<float>>>& captures,
                               std::vector<wifi_security::CaptureRecord>& capture_records,
                               std::vector<Detection>& detections
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
                               , const CycloCaptureInputs* cyclo_inputs = nullptr
#endif
                               );
    // Coverage record for one Wi-Fi capture (security monitor input): what
    // was requested, what the radio reported, and how its time is known.
    wifi_security::CaptureRecord new_capture_record(const ScanStep& step, const DeviceProfile& profile,
                                                    double requested_rate_hz, double requested_duration_s,
                                                    const CaptureResult& capture);
    // Attempts to (re)connect sdr_, retrying a few times (X310
    // connections over Ethernet fail intermittently - see run()'s
    // comment). Updates status_.connected/error itself either way.
    // Used both for the initial connect and for the mid-run
    // auto-reconnect triggered by a sustained RX stall.
    bool connect_sdr(const DeviceProfile& profile, std::optional<double> gain);
    // Called after every scan step / LoRa listen attempt with whether it
    // returned any samples at all. Only touched from the background
    // thread (run() and its callees), so rx_fail_streak_ itself needs no
    // locking - only the status_ write it produces does.
    void note_capture_health(bool got_data);

    std::unique_ptr<CaptureDevice> sdr_;
#ifdef RFMON_ENABLE_WIFI_CYCLO_SHADOW
    cyclo::ShadowWorker cyclo_shadow_;
#endif
    // Creates the radio in connect_sdr(). Unset in production (a UsrpCapture
    // for the profile's device); tests install a fake radio here.
    std::function<std::unique_ptr<CaptureDevice>(const DeviceProfile&, std::optional<double>)> device_factory_;
    std::thread thread_;
    std::atomic<bool> stop_flag_{false};

    mutable std::mutex config_mutex_;
    std::string active_band_ = BAND_WIFI_2G4;
    double threshold_db_ = DEFAULT_DETECTION_THRESHOLD_DB;
    std::optional<double> gain_db_ = DEFAULT_GAIN_DB;
    bool gain_dirty_ = false;
    std::optional<double> lora_lock_freq_;
    std::optional<int> wifi_fixed_channel_;
    bool lora_laboratory_mode_ = false;
    double lora_capture_seconds_ = LORA_LISTEN_DURATION_S;
    std::vector<double> lora_channels_ = LORA_LISTEN_CHANNELS_HZ;
    bool lora_wideband_listen_ = true;
    std::optional<double> lora_wideband_rate_hz_ = 20e6;
    SdrDeviceType device_type_ = SdrDeviceType::B210;
    std::optional<double> wifi_sample_rate_hz_;

    mutable std::mutex status_mutex_;
    ScannerStatus status_;
    std::atomic<droneid_followup::Followup*> droneid_followup_{nullptr};
    CoordinatedStatus coordinated_status_;           // guarded by status_mutex_
    std::deque<coord::VisitRecord> coordinated_visits_; // guarded by status_mutex_
    mutable std::mutex retention_mutex_;
    size_t iq_retention_ = 0;                  // guarded by retention_mutex_
    std::deque<RetainedCapture> retained_;     // guarded by retention_mutex_
    // Wi-Fi lane, after processing; `records` index-aligned with job.captures.
    void retain_captures(WifiStepJob& job, const std::vector<wifi_security::CaptureRecord>& records);
    std::optional<coord::Schedule> coordinated_schedule_; // guarded by config_mutex_
    uint64_t coordinated_schedule_version_ = 0;            // guarded by config_mutex_

    DeviceRegistry registry_lora_;
    DeviceRegistry registry_wifi24_;
    DeviceRegistry registry_wifi5_;

    mutable std::mutex lora_log_mutex_;
    std::vector<LoraPacketRow> lora_packet_log_;
    std::map<std::string, LoraDeviceRow> lora_devices_;  // guarded by lora_log_mutex_; key kind + id
    lora_rules::Engine lora_rules_;                      // guarded by lora_log_mutex_
    LoraListenStats lora_listen_stats_;                  // guarded by lora_log_mutex_
    size_t lora_channel_idx_ = 0;
    mutable std::mutex capture_mutex_;
    std::string capture_save_directory_;
    std::string capture_message_;
    bool capture_pending_ = false;

    mutable std::mutex wifi_log_mutex_;
    std::vector<WifiPacketRow> wifi_packet_log_;
    std::map<std::string, std::map<int, int>> wifi_source_counts_;

    lora_master::LoraMasterList lora_master_;
    wifi_master::WifiEnrolment wifi_master_;

    // Security monitor input. The run id is fixed per Scanner instance;
    // radio_session_ increments on every successful radio (re)connect,
    // because USRP device time restarts with each new handle.
    std::string security_run_id_;
    wifi_security::MonitorConfig security_config_;
    std::unique_ptr<wifi_security::WifiSecurityMonitor> security_;
    uint64_t security_capture_seq_ = 0;
    uint64_t lora_security_capture_seq_ = 0;
    std::unique_ptr<lora_security::Monitor> lora_security_;
    uint64_t radio_session_ = 0;
    bool running_ = false;

    int rx_fail_streak_ = 0;
    static constexpr int kRxStallThreshold = 3;
};

}  // namespace rfmon
