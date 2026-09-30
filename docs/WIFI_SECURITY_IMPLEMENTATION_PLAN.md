# Wi-Fi replay and DoS detection: implementation plan

Date: 2026-09-23. Status: proposed work; this document does not implement or authorize hardware transmissions.

## 1. Objective and scope

Add a passive, evidence-based Wi-Fi security monitor to the existing DSSS/legacy OFDM receiver. Start with management-frame flood detection and conservative replay anomalies on one fixed channel. Preserve the existing network identities and RF observations. LoRa is outside scope.

The system should distinguish a measured observation, a suspected attack, and independently corroborated service disruption. FCS validates transmission integrity, not sender authenticity. A claimed MAC address is not an authenticated attacker identity. Packet collection is incomplete, including on a fixed channel when reception or processing loses frames.

Initial exclusions: automatic countermeasures, transmitter operation, cryptographic replay validation without security context/keys, HT/VHT/HE payload decoding, IP/application DoS classification without decrypted or AP-side visibility, and physical-device attribution from uncalibrated fingerprints.

## 2. Assessment of the supplied plan

Keep its separate monitor, bounded queues/state, general frame parser, capture accounting, evidence-backed alerts, GUI integration, and benign-look-alike tests. Correct the following before implementation:

| Proposed assumption | Revised interpretation |
|---|---|
| Both decoders already return FCS-checked MPDUs | OFDM exposes `mpdu` and `fcs_valid` separately: bytes alone are not acceptance. `DsssDecodeResult` exposes a beacon and MPDU length, but no MPDU byte vector or general FCS status. DSSS currently passes locally recovered bytes to `parse_beacon()`. A small decoder-interface change is needed. |
| No decoder or classifier changes are necessary | Preserve DSP algorithms initially, but expose general DSSS frames, separate FCS checking from beacon parsing, and remove the beacon-duration restriction from the security decode path. Any required classifier diagnostics are explicit instrumentation work. |
| DSSS is categorically blocked at 20 Msps | `CONTEXT.md` records an older failure and also describes OFDM decoding as future work, superseded by `HANDOVER.md` section 11. Earlier session observations include DSSS identities. Reproduce current DSSS yield before declaring a blocker. An internal 22 Msps chip grid does not by itself prove a universal minimum input sample rate. Do not change radio rates based only on old notes. |
| All PMF management frames have unreadable bodies | Protected unicast robust management bodies are encrypted; group-addressed robust management uses integrity protection and may remain readable. Authentication still cannot be established without the relevant keys/context. PMF capability advertised in a beacon is not the negotiated state of every client. [1] |
| A general header always has three addresses and sequence control | Header length and field presence depend on type/subtype. Control frames can be shorter; data frames may have address 4, QoS and additional controls. Parse conditionally and bounds-check every access. [2] |
| Adjacent-channel overlap creates duplicates across hops | A single receiver observing non-overlapping time intervals cannot hear the same transmission twice merely because passbands overlap. Frequency overlap also does not guarantee sufficient bandwidth/SNR to decode an adjacent channel. Deduplicate only demonstrated duplicate processing or overlapping observations of the same transmission. |
| TSF reversal, exact repetition, or RF mismatch is strong proof | These are anomalies needing timing, retry, restart, capture-health and configuration context. Start with qualitative confidence backed by validation; do not add correlated indicators into an invented probability. |
| Similar MACs or locally administered-bit pairs identify one radio | They do not establish physical-radio equivalence or a shared sequence counter. Keep state per observed transmitter/context; merge only with independently established topology. |
| Carrier rejects and high decode failure establish jamming | They establish interference or reception anomalies at most. Normal occupancy, off-channel traffic, unsupported PHYs, clipping, gain changes, LO leakage and transport loss can produce similar symptoms. |
| Fixed 30–40 dB attenuation makes an X310 TX test safe | A hardware test needs a separate reviewed link budget using actual TX power, receiver limits, attenuation ratings, isolation and port configuration. Do not assume an unused TX port or adequate attenuation. Start offline and receive-only. |

Code inspected for this review: `src/wifi_dsss_rx.hpp/.cpp`, `src/wifi_ofdm_rx.hpp`, `src/scanner.cpp`, `src/wifi_phy.cpp`, `CONTEXT.md`, and `HANDOVER.md`. Use function names as integration anchors, since source line numbers change.

## 3. Architecture and data contracts

```text
USRP capture + timing/health metadata
    ├─ burst classification → DSSS / OFDM decode → validated MPDU events
    └─ capture coverage + spectrum/rejection statistics
                         ↓
              bounded security input queue
                         ↓
           parser → state windows → detector rules
                         ↓
             incident/evidence store → GUI
```

The scanner enqueues observations without blocking reception. One consumer owns detector state and publishes immutable alert snapshots. The offline runner calls the same parser/state/rules without a radio, live identity writes, or GUI dependency. Queue loss becomes a visible coverage event rather than silent degradation. All caches, queues, evidence buffers, files and GUI histories have byte/count/time limits; disk errors cannot stall acquisition.

Suggested modules are `src/security/wifi_frame_event.*`, `wifi_security_monitor.*`, `wifi_security_rules.*`, and `wifi_security_store.*`. Explicitly add them to appropriate build targets; do not assume the current source glob includes nested directories. Names are proposals, not files created by this plan.

### Packet event

- Unique capture ID, sample index/range, ingestion identity, PHY/rate, FCS status and bounded MPDU bytes.
- Receive timestamp with clock domain and uncertainty. Prefer UHD receive metadata plus sample index/rate within a contiguous capture. If unavailable, record an uncertain host-time anchor explicitly; GUI `HH:MM:SS` and capture-request time are insufficient for tight TSF comparisons.
- Tuned center, monitored channel, actual sample rate, bandwidth, gain, antenna and receiver profile. Advertised AP channel is a separate field, not proof of RF origin.
- Frame control/type/subtype, conditional address roles, sequence/fragment, retry/protection and QoS fields where defined.
- Management reason/status, authentication transaction, beacon TSF and security advertisements when readable. Unsupported, truncated, encrypted and malformed bodies have explicit states.
- Cipher packet number/key ID or management integrity packet number only when correctly identified. A key ID is not a unique key epoch; unknown security associations limit replay conclusions.
- Optional RF measurements with their acceptance gates and receiver conditions. Do not discard an otherwise valid packet because its fingerprint failed a gate.

### Capture coverage and health

Record valid sampled intervals, tuning/settling gaps, empty captures, overflow/discontinuities, decoder attempts and outcomes, burst-limit saturation, queue drops and relevant processing time. An overflow with unknown gap positions invalidates precise timing claims across that interval.

Rates mean **observed decoded frames per usable observed second**, not true transmitted packet rates. Keep denominators and coverage alongside results. Never interpolate attack activity through unobserved intervals. Fixed-channel mode should reduce gaps, but finite captures still need accounting; assess continuous RX only if measured dead time requires it.

Collect spectrum statistics before packet-classification rejection. Audit what existing helpers actually expose; a local percentile inside `detect_bursts()` is not automatically usable telemetry. Under continuous interference, a within-capture percentile can rise with the signal and hide occupancy. Record reference method and compare like receiver/gain/bandwidth settings. Digital power is not calibrated dBm.

### Identity and incident storage

Keep `wifi_master` as observed network history. Security state keeps its own baseline and previous advertisements so a suspicious new frame does not silently redefine the trusted reference. An operator-approved profile can be recorded separately; learned observations are not authenticated truth.

Version incident records and include rule/config version, severity, qualitative confidence, first/last observation, claimed source/target, channel, measured numerator/denominator, coverage limitations, benign alternatives, and evidence references. Coalesce repeated observations into one incident. “Not observed again” during a scan gap does not mean “attack ended.” Persist configuration and incidents; reset or mark timing/session state uncertain across process restarts.

## 4. Detector policy

| Detector | Initial decision and required safeguards |
|---|---|
| Deauth/disassociation flood | Sustained excessive observed rates per target/BSSID and channel, compared with a baseline and minimum usable exposure. Count unicast/broadcast and readable reasons, but do not treat particular reason codes, broadcast destination, or an uncalibrated fingerprint as attack signatures. Alert on suspected flood; PMF does not prove client impact. |
| Authentication/association flood | Request rates, repeat attempts, transaction outcomes and target concentration. Account for startup, roaming, reconnect storms and missing responses. Include association/reassociation responses if reasoning about outcomes. MAC randomization alone is not malicious. |
| Repeated-frame / stale-beacon anomaly | Retain raw observations; compare both exact bytes and explicitly defined semantic fields. Legitimate retry/header changes mean a full-byte hash alone is insufficient. Separate known duplicate ingestion from repeated over-the-air transmissions. Retry flags and time windows are evidence, not universal exemptions or proof. |
| TSF inconsistency | Compare AP timestamps with receive times whose uncertainty is known. Require multiple observations; preserve capture gaps, reordering and clock changes. Report restart-like resets as an explanation, not automatic dismissal. A replay can also look like a restart. No tight comparison against GUI wall-clock time. |
| Sequence or encryption-counter anomaly | Use the correct transmitter/link/traffic-class/security context. Handle wrap, fragments, retries, reordering, missing observations and rekey/reassociation. A backwards sequence alone has low evidential value. Defer stronger counter rules when key epochs cannot be established. |
| Advertised security/identity change | Report an observed change against a retained baseline. SSID/channel/cipher/PMF changes can reflect administration, migration or advertised-profile differences. Missing/malformed IEs are not automatically “open.” Security downgrade is not synonymous with rogue AP. |
| Beacon/probe population anomaly | Elevated management traffic or new advertised BSSIDs may warrant a warning after normal multi-BSSID operation, scanning clients and MAC randomization are accounted for. Similar fingerprints or junk-looking SSIDs are not a physical-source giveaway. |
| RF degradation/interference | Sustained changes in spectral occupancy/power and decoding outcomes with healthy reception. Initially label as RF anomaly, not confirmed jamming. Reactive-jamming attribution needs substantially better time-correlated evidence or an independent reference. |
| NAV anomaly, later | Parse control subtypes and Duration/ID semantics correctly. A large field can represent an association ID or another special meaning rather than a NAV reservation. Long durations alone are insufficient evidence of abuse. |

RF fingerprint mismatch and power shifts remain supporting observations until controlled same-device/different-device measurements establish stability across gain, channel, temperature and propagation. A confidence label must disclose its evidence and is not a statistical probability. Severity and confidence are independent.

## 5. Work packages and acceptance gates

### A — Input foundation and offline event replay: start here

1. Reconcile old documentation with current results; create a coverage matrix for band, PHY/rate, subtype and protection visibility. Reproduce the 1 Mbps DSSS case at the current X310 settings; no sample-rate changes by assumption.
2. Add the conditional MAC/management parser, including TSF, required request/response fields and independent general-MPDU FCS validation.
3. Expose validated DSSS MPDUs and ensure short supported-rate management frames reach decoding. Preserve beacon parsing, identity behavior and existing RF gates; isolate security eligibility from beacon-cadence eligibility.
4. Add timestamp/coverage contracts, bounded event capture and a deterministic offline runner. Deduplicate repeated ingestion by capture/sample identity; retain real repeated transmissions for analysis.
5. Add malformed/truncated/variable-header and protocol fixture tests, plus decoder-to-event tests for short management frames. Cross-check field interpretation with an independent dissector.

Acceptance: every supported FCS-valid management fixture produces one correctly attributed event; invalid/truncated frames cannot become valid events; retry transmissions remain distinct; duplicate processing is idempotent; missing timing/coverage is explicit; existing Wi-Fi regressions pass and LoRa behavior is unchanged.

USRP requirement: a bounded receive-only coverage audit is useful, but parser/event tests can begin offline. This package does not require a transmitter, a new radio, or an attack generator.

### B — Fixed-channel measurement and minimal incident plumbing

Add selectable fixed-channel monitoring, measure capture dead time and processing capacity, and implement bounded incident storage and a small diagnostic GUI. Start clean baseline collection here, before detector thresholds. Condition baselines on channel and receiver settings; reset/requalify after changes. Keep baseline versions, freeze learning during candidate incidents, and allow review so persistent attacks are not automatically learned as normal. Time-of-day models are optional until sufficient clean data exists.

Acceptance: all losses/gaps are reported; memory/disk limits hold under load; observed-rate denominators are reproducible; storage failure is visible; restarting does not pretend continuous observation. Read-only AP/client logs, when available, provide impact context.

USRP requirement: yes for receive-only baseline acquisition and comparison with a monitor-mode adapter. An adapter is a useful independent reference, not lossless ground truth; align coverage, timestamps and PHY capabilities.

### C — First security milestone

Implement deauth/disassociation flood and conservative repeated-frame/TSF anomaly rules using measured baselines. Run in observation mode first. Add alert detail/timeline, coverage and evidence inspection, and a badge linking an identity to an incident without labeling that identity an attacker. Keep deterministic rule/config versions for replay.

Acceptance: labeled offline scenarios are detected within a declared usable-exposure window; normal retries, reboots, reconfiguration, gaps and reconnect activity do not become high-confidence attack claims. Measure missed detections as well as false alerts. GUI confidence matches actual evidence availability.

USRP requirement: receive-only live confirmation; initial detector development is offline.

### D — Expand only after C is measured

Add authentication/association and beacon/probe anomalies, then advertisement-change and security-context-aware counter rules. Introduce RF-interference warnings after spectrum telemetry is calibrated. Add stronger spoofing/fingerprint or NAV rules only when supporting measurements justify them. Resolve remaining DSSS/PHY coverage deficiencies as separately measured receiver work; do not conceal them behind “no attack detected.”

## 6. Validation plan

- **Protocol fixtures:** independent byte-level examples for header variants, control/data address layouts, sequence wrap, fragmentation, retransmission flags, readable versus encrypted/integrity-protected management bodies, malformed IEs, and boundary lengths. Include a delayed legitimate retry and a replay carrying a retry flag.
- **Temporal scenarios:** host clock jumps, uncertain receive timestamps, AP reset, reordered ingestion, process restart, scan gaps, overflow and queue loss. Do not assume paired BSSIDs share a radio or counter.
- **Pipeline fixtures:** recorded or synthetic IQ for supported-rate short management frames, clean traffic, poor SNR, collisions, unsupported formats, and RF interferers. Generated attacks never require hardware transmission. Record fixture provenance and distinguish local-generator loopback from independent validation.
- **Reference comparison:** align a receive-only monitor adapter with USRP intervals. Investigate disagreements instead of assuming either receiver is complete. Use AP/client logs to corroborate service impact when available.
- **Performance:** inject sustained event load offline; verify bounded memory, disk rotation, no acquisition-blocking writes, deterministic expiry and visible dropped-event counts.
- **Passive soak:** begin with 24–72 hours across representative normal conditions. Report false incidents per monitored hour (and per BSSID-hour where relevant), usable exposure, detection delay and labeled-scenario sensitivity. Set deployment targets before evaluation; a short run with no alerts is not proof of accuracy. Keep development and held-out recordings separate.
- **Optional later hardware stimulus:** requires a separate test design and authorization for equipment under control, with RF isolation and a calculated link budget. Neither this document nor writing a detector is authorization to transmit deauth frames, replay traffic or interference. Prefer offline fixtures for the first milestone.

## 7. Definition of the first deliverable

Packages A–C deliver a passive fixed-channel monitor that explains suspected management-frame floods and replay anomalies, preserves inspectable evidence, reports coverage limits, and survives sustained input without disrupting reception. It does not claim exhaustive coverage, authenticated attacker identity, proven jamming, or confirmed DoS impact from packets alone.

Begin with **Package A** as the first reviewable implementation task. Complete its acceptance checks before expanding the rule set. The plan includes future tests only; no application code, configuration, tests, LoRa files or existing documentation were changed when this document was written.

## References

1. [Cisco: 802.11w Protected Management Frames](https://www.cisco.com/c/en/us/td/docs/wireless/controller/technotes/5700/software/release/ios_xe_33/11rkw_DeploymentGuide/b_802point11rkw_deployment_guide_cisco_ios_xe_release33_chapter_0100.pdf) — distinguishes unicast protection and broadcast/multicast integrity/replay protection.
2. [Wireshark: IEEE 802.11 field reference](https://www.wireshark.org/docs/dfref/w/wlan.html) — independent field names/layout cross-checks; implementation should also use the applicable IEEE 802.11 specification for normative semantics.
3. [Suricata: thresholding](https://docs.suricata.io/en/latest/rules/thresholding.html) — useful precedent for keeping detection thresholds and alert-output limits distinct, not a Wi-Fi detector supplied by this project.

## Implementation update — 2026-09-25

The parser/event/baseline/GUI foundation and capture-processing lane are now in
place. The first management-frame flood rule has been added with coverage-aware
learning, incident lifecycle hardening and evidence inspection. The executable
behavior and provisional defaults are documented in
[WIFI_SECURITY_FLOOD.md](WIFI_SECURITY_FLOOD.md).
Replay/TSF detection, live threshold calibration and independent impact
corroboration remain future work; the original roadmap above is historical.

## Implementation update — 2026-09-28

The first replay milestone is implemented: bounded historical-beacon matching
with an intervening newer TSF, coverage/device-time gating, inspectable three-frame
evidence, live/offline integration, GUI diagnostics and persisted incidents.
See [WIFI_SECURITY_BEACON_REPLAY.md](WIFI_SECURITY_BEACON_REPLAY.md) for executable
semantics, limits, test commands and the hardware acceptance protocol.

Progress and next gates:

1. Implemented: parser/event/baseline foundation and management disconnect flood.
2. Implemented: conservative historical-beacon replay rule and offline regression
   tests. A backward TSF alone is reacquisition, not an attack verdict.
3. Next: USRP receive-only reception/coverage campaign, then isolated labelled
   historical-beacon tests with independent RF reference and captured IQ.
4. Before deployment calibration: held-out positive/negative sessions, per-PHY
   reception and detection recall, false alerts per observed hour, classification
   confusion, resource limits and multi-day benign soak.
5. Later: disconnect replay correlation, calibrated TSF anomaly rules and
   encrypted-counter context. These are not covered by the new beacon rule.

The 2026-09-25 statement that all replay detection is future work is superseded
by this scoped milestone. No claim of exhaustive security classification or
hardware validation is made. LoRa remains outside this work.

## Hardware progress — 2026-09-28

Completed the first receive-only X310 campaign: 73,256 FCS-valid frames across
channels 36 and 11; 2,429 channel 11 beacons evaluated (1,702 DSSS, 727 OFDM).
No recorded overflow, queue rejection or incidents; live/offline snapshots agree.
Ten offline derived real-packet cases passed. These results do not constitute an
over-the-air attack test or a calibrated false-positive/recall measurement.

A short IQ fixture decoded successfully, but its manifest lacks device-time
anchors and is ineligible for replay timing. Next tooling gate is device-time-
aware IQ capture, followed by isolated ESP32/reference-receiver validation and a
longer held-out campaign. User has an ESP32-S3 Heltec-style V3 but no shield box.
Details, limitations, artifacts and commands:
[hardware validation report](WIFI_SECURITY_HARDWARE_VALIDATION_2026-09-28.md).

## ESP32 preparation — 2026-09-29

Identified the user's Heltec V3, saved and device-verified its current 8 MB flash,
and preserved the distinct factory image. Built/flashed an owned-AP-only fixture
for one normal client disconnect, with idle boot and no raw injection or LoRa API.
AP startup confirmed. Client association and USRP disconnect-frame correlation
are the next acceptance gate, not a sustained-flood detection claim.
See [ESP32_WIFI_RECEPTION_CHECK.md](ESP32_WIFI_RECEPTION_CHECK.md) for recovery,
fixture scope and artifacts.

## Flood transmitter feasibility — 2026-09-29

The Heltec ESP32-S3's supported SoftAP API provides own-station disconnects,
but a source of sustained distinct deauth/disassoc frames is not established.
The vendor raw transmit API does not list those subtypes, and the supplied
simulator counts attempted sends even after TX errors. No live flood was sent.
See [ESP32_FLOOD_FEASIBILITY_2026-09-29.md](ESP32_FLOOD_FEASIBILITY_2026-09-29.md).


## Live ordinary-disconnect acceptance — 2026-09-29

The Heltec SoftAP's single supported own-client disconnect was observed over RF
by the X310 on channel 11: 17 FCS-valid disconnect transmissions, reduced to
three distinct retry-normalized contents in one capture. The production security
monitor reported zero incidents, and offline replay matched the live result.
This completes the receive/parse/normal-classification acceptance gate. The
positive sustained-flood hardware gate remains open: the measured operation did
not supply the required 20 distinct units across two captures, and there is no
RF shield box for a controlled flood campaign. Details and artifacts are in
[ESP32_WIFI_RECEPTION_CHECK.md](ESP32_WIFI_RECEPTION_CHECK.md).
