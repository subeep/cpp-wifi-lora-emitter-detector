# Historical beacon replay detection

Implemented 2026-09-28. This is the first replay milestone, alongside the
existing management disconnect flood rule. LoRa and the receive/PHY settings
are unchanged. No radio access is needed for the offline checks.

## Rule and evidence

`historical_beacon_replay` v1 reports a **suspected historical beacon replay**
when a complete FCS-valid beacon matches retained bytes (excluding FCS and the
Retry flag), a beacon with a strictly larger TSF was observed in between, and
the matching beacon returns at least 0.5 seconds after the original observation.
All three observations must have ordered USRP device timestamps in the same
run, radio session, receiver profile, claimed BSSID/transmitter, PHY and rate.
Full retained bytes are compared, not just a hash. Retry alone cannot suppress
an alert. A repeated packet without an intervening newer beacon is insufficient.

Incidents carry original/intervening/repeated frame references, bounded MPDU
prefixes, exact integer TSF strings, device times, replay age, rule configuration,
coverage limits and benign alternatives. The GUI displays these in the incident
evidence popup. Incidents persist through the existing state store; matching
history deliberately does not persist across restarts. Confidence and severity
are medium, not proof of malicious intent, authenticated origin or client impact.
AP firmware sending stale beacons and an unseen restart remain alternatives.

This is a historical match rule, not a clock-drift estimator: it does not compare
TSF deltas against packet-start deltas. Unknown backward TSF movement clears that
identity's history and reacquires, rather than generating a replay verdict.

## Bounds and coverage

Provisional configuration in `BeaconReplayConfig`:

| Setting | Default |
|---|---:|
| Minimum original-to-repeat separation | 0.5 seconds |
| Maximum history age | 60 seconds |
| Maximum gap between captures before reacquisition | 2 seconds |
| Receiver profiles / identities per profile | 8 / 64 |
| Distinct historical beacons per identity | 16 |
| Pending captures / beacon frames per capture | 64 / 512 |
| Maximum full beacon size retained | 2048 bytes |

The history count often limits the practical look-back to much less than 60
seconds. Capacity evictions and excluded captures are exposed in snapshots and
the GUI. Profile/identity eviction is deterministic, not an identity assertion.

Processing waits for finalized capture coverage. Missing events, queue loss,
overflow, burst caps, timeout, local input overload, host-only timing and
out-of-order capture time cannot produce a verdict or establish replay quiet.
Loss clears history. Oversized or untimed otherwise-eligible beacons invalidate
that capture for this rule. Fragmented beacons and malformed bodies are not used.
Capture timestamps and frame order are validated before any incident is emitted.
The state currently conservatively requires both enabled rules' usable coverage
before advancing incident quiet clocks. A triggering replay capture is excluded
from baseline training; no persistent replay learning hold is installed because
this rule does not learn a rate baseline. Existing operator/flood holds remain.

An incident cannot close in its own triggering capture. Subsequent usable quiet
exposure uses the shared incident lifecycle (60 analysed seconds by default).
No observation gap is interpreted as clean air.

## Validation and remaining work

`test_wifi_security_beacon_replay` covers positive historical matches, retry-bit
variation, normal progression and sequence wrap, duplicate ingestion, reboot-like
TSF reset, history expiry/eviction, receiver context changes, degraded captures,
local overload, minimum delay, bad FCS, incident recovery, persistence and
serialized/direct/threaded decision equivalence. Existing flood, parser, event,
baseline and capture-lane regression suites must also pass. The offscreen GUI
harness opens both flood and replay evidence views.

Build/run:

```sh
cmake -S . -B build
cmake --build build -j4 --target test_wifi_security_beacon_replay rf_monitor_gui wifi_security_replay test_wifi_gui
ctest --test-dir build --output-on-failure -R '^(wifi_security_.*|serial_lane|scanner_wifi_lane)$'
./build/test_wifi_gui /tmp/wifi-security-replay-gui
```

Hardware acceptance is still pending. Connect the USRP for a receive-only
fixed-channel baseline and reception/timing assessment first. For isolated
positive replay cases use owned equipment, an independent monitor receiver and
an ESP32 only after verifying its supported frame types and whether firmware
rewrites sequence/TSF fields. Keep original, newer and repeated transmissions
within measured observation coverage and retained history. Record reference
frames, USRP IQ, event/capture logs, effective config and GUI incident evidence.
USRP remains receive-only; disruptive cases require RF isolation.

Report reference-to-decoder yield, conditional and end-to-end detection recall,
false incidents per observed hour, classification confusion, time to detection,
loss/exclusion rates and resource usage. Separate calibration and held-out runs;
include multiple APs, PHYs, signal levels, reboot/retry controls and a multi-day
benign soak. Packet fixtures do not validate the RF decoder chain. New recorded
IQ replay fixtures, timing tolerance calibration and independent client-impact
logs remain hardware work.

Not implemented: generic TSF drift alerts, disconnect-frame replay, encrypted
PN/IPN replay classification, handshake replay, spoof attribution, other flood
classes or jamming classification. Existing `wifi_security_replay` is an offline
log runner; its name does not imply those attack detectors exist.

### Offline validation result — 2026-09-28

- Seven targeted CTest suites passed: MAC parser, events, baselines, serial lane,
  scanner Wi-Fi lane, disconnect flood and historical beacon replay.
- The new replay executable passed 35 assertions, including direct/serialized/
  threaded incident and evidence equivalence.
- Production GUI and offline replay runner built successfully.
- Offscreen GUI interaction passed for both incident views; the replay evidence
  screenshot was visually inspected. No radio was accessed.
- Hardware/IQ acceptance and false-positive calibration remain unverified.

AddressSanitizer and UndefinedBehaviorSanitizer: all 35 replay assertions passed; no sanitizer diagnostics.

### Initial hardware progress — 2026-09-28

Receive-only USRP validation now covers live DSSS and OFDM beacon ingestion,
replay eligibility and live/offline equivalence. Ten offline cases using real
packet bytes passed. Controlled over-the-air replay, device-timed IQ replay and
long-duration false-positive/recall validation remain pending. See the
[hardware report](WIFI_SECURITY_HARDWARE_VALIDATION_2026-09-28.md).
