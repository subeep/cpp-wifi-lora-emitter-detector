# Wi-Fi and LoRa security hardening

Updated: 2026-10-02. Bluetooth is outside this work. Both Wi-Fi and LoRa are now
in scope, as requested. This document distinguishes implementation from validation
and from future detectors. No prevention or authenticated attacker attribution is
claimed by this passive receiver.

## Current capability and work order

| Milestone | Status | Acceptance / next action |
|---|---|---|
| Capture evidence and replay foundation | Implemented; receive-only and conducted RF gates passed | Fresh timing and live/offline checks recorded in the hardware reports; broader RF evaluation remains pending |
| Existing Wi-Fi input/history safeguards | Implemented: rejected-input boundaries and additional replay/flood regression checks | Historical beacon replay remains enabled; disconnect flood rule expanded as described below |
| Wi-Fi flood coverage expansion | Implemented; software and conducted 6 Mbps OFDM positives passed | All four flood paths and historical beacon replay flagged automatically; calibration and broader hardware negatives remain pending |
| LoRa security event/coverage pipeline | Implemented; evidence only | Separate capture hypotheses from receptions; no LoRa attack rules yet |
| LoRa anomalies / LoRaWAN context | Pending | Raw-LoRa rate/airtime anomalies; version/session-aware LoRaWAN counters and joins; optional owned-session MIC verification |
| Additional Wi-Fi rules / interference indicators | Pending | Add one independently validated rule at a time; RF degradation alone is not proof of intentional jamming |
| Baseline calibration and deployment evaluation | Pending | Held-out positives/negatives, receiver recall separately from detector recall, false alerts per observed hour, 24–72-hour benign soak |

The first software milestone is complete. This is not completion of the full
security-classification programme. Wi-Fi disconnect flood coverage now includes
guard-separated exact repeats and distributed targets, with rolling windows.
Conducted OFDM positives passed as recorded below; threshold calibration and
broader RF coverage remain pending. LoRaWAN structural candidates remain unauthenticated and do not
establish a device/session identity. Repeated raw-LoRa payloads can be ordinary
telemetry, not necessarily attacks.

## Implemented capture provenance

`src/capture_timing.hpp` contains the existing capture timing contract without a
UHD dependency. Receiver streaming/tuning policy and RF fingerprint gates are
unchanged. Metadata records the first-sample device timestamp, host bracket,
requested sample count, timeouts/exceptions, tune result, requested gain, retune
flag and overflow positions/resume anchors.

- **Wi-Fi IQ schema 2:** `wifi_capture_cli` uses detailed receive results and
  saves one run/session identity with increasing capture numbers across files.
  `wifi_security_replay` preserves these anchors and identities, allowing timing
  comparisons across captures in the same session. It checks IQ length, checksum,
  finite samples, bounded sizes, local filenames, overflow consistency and clock
  ranges before decoding. `wifi_ofdm_replay` reads both schemas through the same
  loader.
- **LoRa IQ manifest version 2:** scanner and receive-only capture CLI preserve
  device timing and run/session/capture identities. Both versions 1 and 2 load.
  Saving a legacy/synthetic capture without known timing does not invent it.
- **Legacy recordings:** Wi-Fi schema 1's timestamp predates tuning and is not
  promoted to a first-sample clock. LoRa version 1 lacks device time. Such files
  remain useful decoder fixtures, but cannot demonstrate device-timed replay.
- **Cropping:** contiguous LoRa crops shift known sample anchors and receive a
  distinct ingestion identity. The old host capture-call timestamp is never
  treated as sample timing. Crops of discontinuous recordings conservatively
  remain timing-excluded.
- **Packet-start uncertainty:** the LoRa decoder's symbol-window start/end
  indices map back through integer decimation. The sample clock is exact when
  anchored, but the packet-start estimate remains coarse; its symbol-window
  uncertainty is retained explicitly. It is not a precision arrival measurement.

Hardware timing added to archived IQ in integration tests is **synthetic test
metadata**, not recovered or measured timing from those historic transmissions.

## Wi-Fi rejected-input safeguard

A malformed/unknown input line now interrupts pending flood windows, beacon
history and pending baseline learning. It cannot silently bridge an unknown gap.
The offline runner preserves loss and rejected-input markers when re-exporting
recordings. The initial safeguard milestone did not change thresholds; the
subsequent disconnect-flood expansion below adds paths under rule version 2.

Added regression cases cover malformed-input boundaries for both rules, changed
SSID/sequence/source/PHY under an old TSF, and out-of-order frames before incident
emission. Existing retry, reboot, loss, baseline, resource and persistence checks
remain part of validation.

## Wi-Fi disconnect flood expansion — 2026-10-02

`management_disconnect_flood` version 2 now evaluates four paths:

- `distinct_management_frames`: distinct retry-invariant contents for one target.
- `repeated_management_frames`: distinct contents plus separated non-Retry
  copies for one target, when the distinct-only criterion is not met.
- `ap_wide_distinct_management_frames`: distinct contents across at least two
  affected targets under the same claimed BSSID.
- `ap_wide_repeated_management_frames`: aggregated separated copies across
  those targets, when the AP-wide distinct criterion is not met.

For identical contents, extra counted copies require device timestamps agreeing
with the capture sample anchor, Retry clear, and at least a provisional 10 ms
guard from the previous counted copy. Short trains collapse even without Retry;
Retry-marked duplicate contents never increase the repeated-copy numerator.
Host-only/unknown timing cannot establish repeated-copy separation. Different
contents still contribute to the existing distinct path. These are observation
units, not authenticated independent messages. Tight identical bursts and
Retry-marked repeated floods are deliberate coverage limits of this guard.

The shortest whole-capture suffix covering the configured analysed-time window
and minimum capture count is retained. Evaluation runs after every usable
capture, rather than clearing fixed windows. Whole captures can make the
denominator exceed two seconds; all retained exposure is counted. A group must
have activity in the latest capture and multiple active captures; old window
contents alone cannot reset quiet or emit another incident.

AP-wide counts combine only one claimed BSSID. Client-originated disconnects
use the client as the affected target. If a target-specific path qualifies,
the overlapping AP-wide emission is suppressed for that BSSID in that
evaluation. Incidents are separated by receiver baseline profile. GUI targets
show **AP-wide**, and evidence details expose scope, content counts and path.

Provisional baseline maturity, rate floor/multiplier and minimum-unit thresholds
are unchanged. Candidate captures hold learning before baseline ingestion,
including repeated and aggregate candidates. Clean observed recovery releases
the existing persisted holder. Loss, malformed input, overflow, caps, receiver
context changes and clock regressions break comparisons. Submitted event
counts must match received accepted input even when the count is zero;
out-of-capture disconnect evidence is excluded. Window raw observations,
capture count and group count are bounded; overflow of these limits is explicit
unusable coverage, not evidence of quiet.

Validation includes independent frame/CRC fixtures, exact/retry/short-train
controls, multi-target/client direction, BSSID separation, baseline threshold
and hold checks, gap/session/profile negatives, clock-anchor mismatch, resource
caps, variable capture lengths, four window phases, and identical full threaded
versus offline snapshots. The actual CLI tests use production defaults and
check repeated/AP-wide positives, a shifted boundary, retry/loss negatives and
NDJSON re-export equality. Synthetic positives are not RF attack captures.
The baseline tests now supply accurate submitted-frame counts in their fixtures.

Today's 2,550-frame benign X310 recording also replays with zero incidents and
the same one burst-limited exclusion. At this software milestone, positive RF
attack testing, measured thresholds and long false-positive soak remained
pending; medium confidence
means suspected activity, with reconnect/reconfiguration and spoofing/PMF
acceptance left unresolved. No RF or firmware operation is needed for this
software milestone. LoRa attack rules remain pending.

Final validation for this expansion: production GUI/tools built; all 13 CTest
suites passed. Flood, beacon-replay and event suites also passed with
AddressSanitizer/UndefinedBehaviorSanitizer and no reported errors. Leak detection
was disabled because this environment's ptrace wrapper does not support it;
these runs do not establish leak freedom. The GUI labels/details were compiled;
no new visual render check was performed for this small text change.

## Conducted Wi-Fi hardware classification gate completed — 2026-10-02

HackRF One → confirmed 30 dB attenuator → X310 RF A RX2, without antennas:
the production monitor flagged historical beacon replay plus distinct/repeated
target and AP-wide disconnect paths, with unchanged production thresholds.
Baseline and retry controls produced no new/renewed incidents. The successful
recording contains 1,114 exact-reference FCS-valid OFDM frames across 104 clean
device-timed captures, with no receive/queue loss or cap. Three incidents retain
all five paths through coalesced timelines; live/offline state agrees.

The production GUI rendered the actual recorded incidents and opened the
AP-wide evidence dialog in an automated interaction check; its screenshot was
visually inspected. All 13 CTest suites and 88 regenerated OFDM fixture checks
passed, alongside 63 exact-byte quantized packet preflight checks.

Scope is one conducted channel-11, 6 Mbps synthetic-management campaign; it
does not establish PMF/client impact, DSSS or other-rate recall, general false-
positive rate, RF-jamming detection or authenticated replay. No LoRa change or
firmware flash was needed. Initial failed/incomplete trials and test harness
repairs are retained. Full results and remaining limits:
[HACKRF_WIFI_VALIDATION_2026-10-02.md](HACKRF_WIFI_VALIDATION_2026-10-02.md).

## LoRa evidence monitor and GUI

`src/security/lora_security.*` provides separate LoRa capture batches and state.
The production scanner submits observations before moving them into GUI rows.
Each batch contains the packet evidence **and** its exposure denominator, so a
queue cannot deliver an orphan packet without its capture coverage.

- Complete/partial payloads and valid/failed/absent CRC are separate states.
  Only complete production-PHY, CRC-valid bytes from usable captures count as
  eligible protocol inputs. Physical CRC still does not authenticate a MIC.
- Identical decoder hypotheses at the same sample span, SF/BW, bytes and
  integrity state collapse. Equal bytes at different sample indices remain
  separate receptions. These are not unique-device counts.
- Coverage excludes failed/empty, discontinuous, timed-out, laboratory,
  unsupported and output-limited captures. Decoder cap detection is conservative:
  a combined hypothesis result of at least 32 packets is marked limited.
- Analysed exposure uses the common fully decimated sample span; discarded
  remainder samples do not inflate it. This is supported-PHY exposure, not a
  measured packet-delivery rate. SF5/6 detection-only, implicit PHY and other
  unsupported formats are not covered as decoded traffic.
- Exposure is kept separately by frequency, rate, gain, antenna and device.
  Device-clock regressions exclude the affected observation. Session changes
  permit a clock reset; unsampled time and unknown gaps are exposed separately.
- Bounded defaults: four queued capture batches, 2,048 events per batch, 256
  retained events, 32 receiver/run/clock contexts. Two NDJSON generations together
  are bounded to 64 MiB (32 MiB each). An oversized record or disk error stops
  recording with a visible error; the consumer remains available.
- Default live evidence path: `data/lora_security/events.ndjson` (and `.1`).
  Full unrotated recordings reproduce consumer state. A retained rolling suffix
  cannot reproduce totals/history already evicted from disk; pass `.1` before
  the current file when replaying retained generations.
- The LoRa tab has a collapsible **LoRa security monitoring** panel showing
  latest-capture health, sampled/analysed/usable exposure, integrity inputs,
  queue losses, profile coverage and recording errors. It explicitly states
  that **LoRa attack rules are not implemented**. Existing decoding and RF
  identity tables retain their meaning.

`lora_security_replay` accepts saved IQ directories or event recordings and uses
the same consumer state without hardware or identity-store writes. Malformed
record lines remain explicit gaps, including after re-export; rejected input
makes this runner return a nonzero status while printing its diagnostic snapshot.

## Reproducible software validation

```sh
cmake -S . -B build
cmake --build build --target rf_monitor_gui wifi_capture_cli wifi_ofdm_replay \
  wifi_security_replay lora_capture_cli lora_capture_crop lora_security_replay \
  test_capture_provenance test_lora_security test_scanner_lora_security \
  test_scanner_wifi_lane test_lora_receiver test_lora_observation \
  test_wifi_security_frame test_wifi_security_events test_wifi_security_baseline \
  test_wifi_security_flood test_wifi_security_beacon_replay test_serial_lane \
  test_lora_gui -j4
ctest --test-dir build --output-on-failure
build/test_lora_gui /tmp/lora-security-gui.ppm \
  tests/fixtures/lora_sx1262/capture-1790061256825496286-0
build/lora_security_replay --record /tmp/new-lora-events.ndjson \
  tests/fixtures/lora_sx1262/capture-1790061256825496286-0
build/lora_security_replay /tmp/new-lora-events.ndjson
```

New checks include manifest compatibility/corruption/range rejection, overflow
anchor handling, decimation mapping, hypothesis deduplication versus actual
repetition, integrity eligibility, coverage exclusions, clock reset/regression,
profile separation, bounded memory/record rotation and threaded/offline equality.
An end-to-end scanner test uses a fake radio with archived SX1262 IQ; all writes
stay in a temporary root. CLI tests use real archived Wi-Fi and SX1262 IQ and
synthetic timestamp anchors, not attack transmissions. Wi-Fi packet crops receive
deterministic low-level noise padding so the production burst detector has a
quiet reference; the embedded real packet samples are unmodified.

Verified on 2026-10-02:

- Production GUI and affected capture/replay tools built successfully.
- All 13 registered CTest suites passed, including the new scanner/CLI checks.
- Four focused suites (capture provenance, LoRa security, Wi-Fi flood and beacon
  replay) passed with AddressSanitizer and UndefinedBehaviorSanitizer, with no
  reported address/undefined-behaviour errors. Leak detection could not run under
  this environment's ptrace wrapper; it was disabled for those runs, so no leak-
  freedom claim is made.
- Actual LoRa GUI widgets rendered offscreen with an archived SX1262 row and
  explicitly synthetic coverage values; the resulting image was visually
  inspected. Existing Wi-Fi scanner-lane equivalence checks passed.
- No hardware connection, RF transmission or fresh hardware timing validation
  was performed during this milestone.

## Receive-only hardware gate completed — 2026-10-02

The connected X310 (serial `326CF02`) passed fresh Wi-Fi/LoRa sample reception
and device-clock checks. Four Wi-Fi IQ captures decoded 23 FCS-valid frames;
IQ/event snapshots matched exactly. A production channel-11 run decoded 2,550
valid frames with matching live/offline semantics. One burst-limited capture
was correctly excluded from clean rule continuity. Four production LoRa
captures supplied eight usable seconds with matching live/event state and
first-saved-IQ/live-batch state. No known LoRa transmitter was heard.

The finite receive-only `lora_scan_smoke` helper provides an isolated, repeatable
production-lane check. Results, artifacts, requested-gain limits and remaining
hardware gates: [SECURITY_RECEIVE_CHECK_2026-10-02.md](SECURITY_RECEIVE_CHECK_2026-10-02.md).
This completes the timing/evidence gate; positive attacks and known-source LoRa
reception remain unverified.

## Hardware gate procedure and next connection

**X310 first, receive-only.** No transmitter is required for this gate:

1. Collect fresh schema-2 Wi-Fi captures and version-2 LoRa/no-signal captures.
2. Verify device anchors advance correctly across captures, actual rates/tuning
   agree with the metadata, and requested/received/analysed counts are consistent.
3. Compare live event recordings with saved-IQ replay for the same captures;
   account explicitly for provenance labels and measured processing times.
4. Confirm a capture with samples but no decoded packets remains observed coverage, while a failed receive,
   overflow or queue loss remains excluded. Do not create RF interference to
   test software loss handling; those paths already have injected-input tests.

For the subsequent **known LoRa reception** check, connect the Heltec V3/SX1262.
Preserve its current firmware before selecting compatible owned-test LoRa
firmware; it was previously repurposed for Wi-Fi. A node plus gateway/server
reference and documented version/session are needed for true LoRaWAN tests.

A conducted HackRF disconnect-flood/replay campaign now passed for legacy OFDM
at 6 Mbps; its received frame bytes were verified against lab references. The
ESP32-S3's rejected raw deauth attempt remains a transmitter limitation. For
all later calibration, retain independently labelled positives and benign
look-alikes, split calibration from held-out evaluation, and measure latency,
receiver recall, detector recall and false alerts per usable observed hour.
