# Receive-only Wi-Fi enrolment validation — 16 October 2026

Hardware: UHD 4.1.0.5, X310 serial 326CF02 at 192.168.10.2, UBX-160 v2, RX2, 20 MSps. No RF transmissions. No production enrolment data modified. These are ambient observations, not labelled physical-device identities.

Completed 340 one-second captures and received 40,402 FCS-valid frames across five runs. No capture overflows, timeouts, exceptions, short captures, security queue rejections, or instrumented enrolment drops/storage errors occurred. All four instrumented reload comparisons passed. One capture hit the burst-analysis cap.

## Live results

| Run | Captures | Valid frames | Interfaces in isolated store | RF profiles | Reload comparison |
|---|---:|---:|---:|---:|---|
| ch1 | 44 | 1601 | not instrumented | not instrumented | not instrumented |
| ch6 | 56 | 1498 | 14 | 10 | True |
| sweep24 | 68 | 2581 | 44 | 31 | True |
| sweep5 | 56 | 29097 | 28 | 38 | True |
| soak1 | 116 | 5625 | 34 | 20 | True |

The initial channel-1 run lasted 45 seconds, channel 6 ran 60 seconds, the 2.4 GHz sweep ran 75 seconds, the 5 GHz sweep ran 60 seconds, and the final channel-1 stability run ran 120 seconds. Durations are requested wall time; initialization, capture scheduling and shutdown determine the actual capture count. Tests are sequential, not simultaneous multiband coverage.

The sweeps covered channels 1–13 and 36/40/44/48/149/153/157/161/165. Both DSSS and legacy OFDM decoded live. See `summary.json` for exact counts and health metrics. Interface counts must not be added together to claim a unique physical-device count.

All instrumented stores reopened with matching tested device counters, first/last timestamps, policy, role, context identifiers, profile capture counts, medians and spreads. All newly discovered interfaces initially had the NEW DEVICE label. The previous software GUI test verifies the corresponding display; this campaign did not run the live GUI.

A copy of channel 6's live store passed 16 policy checks: acknowledgement without approval, rejection of reference acceptance before approval, explicit acceptance of an eligible live profile, disallow and alias persistence, reference persistence, and protection against a second writer. Test approval means only a test command on copied data, not verified ownership of an ambient device. `policy-check.cpp` and `policy-check.txt` record the procedure and results.

A busy 5 GHz capture reached the burst-analysis cap. Inspection found zero RF-window samples sourced from that capped capture (`cap-check.json`), verifying the learning-quality exclusion without throwing away digital observations. This is a software analysis limit, not a NIC/sample-rate measurement.

## Limits and findings

- The initial channel-1 run used the scanner default requested gain of 40 dB, above the X310's 31.5 dB range. Metadata records requested gain, not actual hardware readback. Do not use that run as a calibrated gain comparison. Later tests explicitly requested 20 dB and the final stability test 30 dB, within range. Actual-gain readback remains an improvement to make.
- UHD reports hardware RX IQ-balance correction unavailable on this daughterboard. RF estimators still run, but this does not establish calibrated transmitter-only silicon measurements.
- A short ambient campaign cannot validate long-term oscillator drift, cross-session physical identity, spoof/replay attack recall, drone model/link identity, or false-positive rates against labelled ground truth. Automatic trusted-reference adaptation remains disabled.
- No controlled attacks, drone flights, unplug/replug recovery, long-duration soak, simultaneous interference generator, or higher-rate throughput benchmark was performed. Some require additional equipment or labelled scenarios; long-duration/recovery tests need a separate deliberately disruptive or extended campaign.
- Sweeping loses off-channel events. Successful receipt of ambient traffic is not exhaustive Wi-Fi PHY coverage.

## Changes and regression

Only the receive-only `wifi_scan_smoke` tool was extended in this campaign: isolated channel-0 band sweeps, enrolment health reporting, and restart comparison. The production GUI/scanner behavior was not changed. All 50 existing CTests passed (23.23 seconds). `git diff --check` passed.

Raw management-frame recordings and per-device stores remain locally available in ignored subdirectories. Aggregate reports omit MAC addresses and SSIDs. The radio was released after testing.
