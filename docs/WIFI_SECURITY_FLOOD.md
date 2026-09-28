# Passive management-frame flood detection

Implemented 2026-09-25, building on the existing security parser, event pipeline,
baselines, incident store, and capture/processing lane. LoRa, receiver tuning,
PHY algorithms and RF fingerprint gates were not changed.

## What is enabled

`ManagementFloodRule` runs inside the security consumer when a capture's coverage
record arrives. Accepted frames are buffered with bounded capacity until that
record establishes their usable exposure. The same rule runs in
`wifi_security_replay`. It detects sustained **suspected deauthentication /
disassociation floods** per claimed BSSID and receiver under one receiver profile.
It does not authenticate the sender or establish that clients disconnected.

Both subtypes share the window. Protected management headers can contribute;
unreadable reason codes are not invented and PMF acceptance is unknown. Malformed
or truncated management bodies do not contribute. No RF-fingerprint attribution
or reason-code signature is used.

Within each window, retry-invariant content hashes count distinct units; repeated
identical content is counted separately as raw frames. This limits ordinary
retransmission amplification. It also means an attack consisting solely of exact
repeats may be missed: disconnect-frame replay detection remains a future milestone. Historical beacon
replay is now covered separately by [WIFI_SECURITY_BEACON_REPLAY.md](WIFI_SECURITY_BEACON_REPLAY.md).
Hashes are grouping aids, not cryptographic proof of identity or equivalence.
Traffic distributed across many receivers below each receiver threshold can also
be missed; this first rule does not aggregate them into an AP-wide flood.

## Provisional defaults

Configuration lives in `FloodConfig` (`src/security/wifi_security_flood.hpp`).
It is configurable by the state/monitor's C++ configuration, not a new GUI slider.
The effective rule configuration is included in snapshots and incident records.
These defaults have software coverage, **not measured deployment calibration**.

| Setting | Default |
|---|---:|
| Minimum clean baseline | 30 analysed seconds and 3 included windows |
| Baseline window | 10 analysed seconds |
| Detection window | 2 analysed seconds, non-overlapping |
| Minimum distinct units | 20 |
| Minimum captures with new units | 2 |
| Threshold | max(10 units/s, 4 × baseline combined p95) |
| Gap that resets a partial detector window | More than 1 second |
| Release rule learning hold | 5 usable quiet seconds within the current receive context |
| Close an open incident | 60 usable analysed quiet seconds on its channel/profile |

The baseline p95 is calculated by summing deauth and disassociation counts within
each included baseline window, then taking the percentile. It is **channel-wide
raw traffic**, compared conservatively against a target's distinct-unit rate;
it is not yet a learned per-client distribution. A busy channel can therefore
reduce sensitivity for an individual target.

A capture with candidate activity at the provisional floor holds learning before
its samples are admitted to the baseline, including during initial warm-up. This
prevents obvious sustained activity from automatically becoming the initial
normal reference. It can also delay warm-up indefinitely in legitimately noisy
environments. Review the hold and coverage rather than interpreting an empty
incident list as proof of no attack. Automated confidence never exceeds medium.

## Coverage and lifecycle

Learning/detection/quiet decisions exclude unprocessed, empty, non-contiguous,
overflowed, timed-out, burst-capped, queue-loss, event-count-mismatched and locally
overloaded inputs. Out-of-order receive times do not establish quiet. Capture
statistics still report the observations even when the detector excludes them.
Global loss notices clear partial rule input and invalidate pending learning.
An affected partial baseline window is excluded, rather than mixed into clean data.

A long gap, changed radio session, gain/profile change, reset baseline version,
or restart discards partial detection context. No rates are extrapolated across
missing samples. Release of a rule hold requires observed quiet, not elapsed
wall time; gaps can prolong it. Operator freeze and rule holders are independent.
Holds survive reset/eviction/restart, and clean observation releases this rule's
hold without releasing other holders. Capacity exhaustion fails conservatively
by excluding learning; it is surfaced in excluded-capture counts.

Incidents coalesce by rule/channel/claimed BSSID/target. Candidate activity resets
the applicable quiet timer; a capture that reports activity cannot also close
that incident as quiet. A different receiver profile cannot close it. A restored
incident is marked interrupted and its old quiet timer is discarded.

The rate windows retain at most 16 receiver profiles, 128 target groups per
profile and 4096 distinct units per profile. Pending input is capped at 64 captures
and 1024 candidate frame records per capture. Baseline hold keys and holders are
also capped. Incident counts, first/recent evidence, timelines, text and MPDU
prefixes have enforced live and restored limits. Saved state is limited to 64 MiB;
an oversized or failed save reports an error and does not replace the previous file.

## GUI and persistence

Use the existing fixed-channel Wi-Fi monitoring control to obtain useful exposure.
After rebuilding/restarting the application, the security panel reports whether
the flood rule is enabled, evaluated windows and excluded captures.

- **Baselines:** learned exposure and a rule-hold label/tooltip. Operator freeze
  remains independent.
- **Incidents:** click the incident number to inspect claimed BSSID/target,
  confidence, triggering numerator/denominator, threshold, baseline version/hold,
  benign alternatives, rate timeline, and first/latest MPDU evidence prefixes.
- Incident evidence remains a suspected observation, not a confirmed attacker or
  confirmed service outage. Closed incidents retain their evidence within limits.

The existing `data/wifi_security/state.json` stores baselines and incidents.
Schema additions are optional for older records. Existing NDJSON frame/capture
recordings remain readable. Replay JSON now includes full retained incidents.
A replay starts with a fresh state; a partial recording without earlier baseline
history cannot reproduce the context of a live monitor that restored saved state.
Use a complete fresh-session event stream and matching configuration when comparing
live and offline decisions. No claim is made that arbitrary partial logs are equivalent.

## Validation and commands

```sh
cmake -S . -B build
cmake --build build -j4 --target rf_monitor_gui wifi_security_replay \
  test_wifi_security_flood test_wifi_security_frame test_wifi_security_events \
  test_wifi_security_baseline test_scanner_wifi_lane test_serial_lane test_wifi_gui
ctest --test-dir build --output-on-failure -R '^(wifi_security_frame|wifi_security_events|wifi_security_baseline|serial_lane|scanner_wifi_lane|wifi_security_flood)$'
./build/test_wifi_gui /tmp/wifi-flood-gui
./build/wifi_security_replay --recent PATH_TO_RECORDING.ndjson
```

Regression scenarios include sustained mixed-subtype traffic, ordinary retries,
a single reconnect burst, cold-start baseline contamination, queue loss, missing
events, overflow, scan gaps, out-of-order time, changed gain, overload, target
separation, readable/encrypted headers, hold independence, restored limits and
restart/recovery. Tests compare decisions and evidence after NDJSON round-trip
and through the actual monitor consumer thread. Default production thresholds
are exercised separately from shortened test windows. The GUI test clicks the
production incident table and opens the evidence popup; rendered output is
visually inspected.

No radio transmissions are part of this implementation or its tests. Live USRP
false-positive rates, detection sensitivity, capture duty cycle and impact
correlation still need a receive-only baseline/reference campaign. The next task
is to measure these defaults on a fixed channel and labeled held-out recordings;
then validate the new historical-beacon rule and extend replay/TSF coverage
using the same evidence lifecycle.
