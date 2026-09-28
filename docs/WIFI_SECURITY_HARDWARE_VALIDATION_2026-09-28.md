# Wi-Fi security hardware validation — 2026-09-28

## Result and scope

Receive-only X310 reception, device timing, production security ingestion and
live/offline equivalence were exercised successfully. Both DSSS and OFDM beacons
reached the historical-beacon rule on channel 11. No radio transmissions or ESP32
flashing were performed. This is **not** a completed over-the-air attack test or a
measured detection-recall/false-positive campaign.

Hardware: X310 serial 326CF02, UHD 4.1.0.5, address 192.168.10.2, RF channel 0,
RX2. Production scanner campaigns used 20 Msps, gain 40 dB, +1.5 MHz capture
offset, fixed channels and the existing processing lane. Records were written to
new validation directories rather than the normal identity/baseline database.
The existing GUI was idle and left unchanged. LoRa source/settings were untouched.

## Live observations

| Metric | 5 GHz channel 36 | 2.4 GHz channel 11 |
|---|---:|---:|
| Requested wall-clock run | 75 s | 60 s |
| Sampled and analysed time | 72 s | 56 s |
| Measured receive duty | 95.69% | 95.60% |
| Largest measured inter-capture gap | 55.12 ms | 58.77 ms |
| FCS-valid accepted frames | 68,224 | 5,032 |
| DSSS frames | 0 | 2,440 |
| OFDM frames | 68,224 | 2,592 |
| Beacons evaluated by replay rule | 0 | 2,429 |
| DSSS / OFDM beacons | 0 / 0 | 1,702 / 727 |
| Overflow / failed / burst-capped captures | 0 / 0 / 0 | 0 / 0 / 0 |
| Security events rejected by queue | 0 | 0 |
| Replay-excluded captures | 0 | 0 |
| Learned clean baseline time | 70 s | 50 s |
| Incidents observed | 0 | 0 |

Channel 36 decoded mainly control traffic and no beacons; it did not exercise
beacon replay matching. Absence of decoded beacons does not prove absence of
beacons on air. Channel 11 exercised the replay rule on both supported paths.
History eviction occurred 2,300 times, consistent with bounded retained history;
this is separate from capture loss. No hardware thresholds were changed.

UHD warned that the OS UDP send buffer was smaller than requested. No kernel
settings were changed. There were no recorded receiver overflows or queue drops
in these runs. Neither that observation nor the short zero-incident window
establishes sustained-load robustness or a deployment false-positive rate.

Replaying the complete recordings produced identical snapshot fields for all
shared measurement and detection fields. The comparison excludes `recorded_bytes`
(live recorder bookkeeping) and `recent` (different output selection/retention).
This includes incidents, baselines, rule diagnostics and coverage accounting.
Channel 36's rotated `.1` recording must precede its current recording.

## Offline cases using real decoded packets

Ten derived event-stream cases passed: for each of DSSS and OFDM, ordinary
progression, historical A-B-A replay, repeats without an intervening newer beacon,
a degraded final capture and an under-threshold repeat delay.

Only the historical replay cases generated one `historical_beacon_replay`
incident each. The other eight generated none. MPDU bytes came from the live
channel 11 recording; timing, capture metadata and selected packet order were
synthetically constructed. These are **offline detector tests**, not attacks
transmitted by an ESP32 and not end-to-end radio attack detection evidence.
The generator and output retain original frame references for provenance.

## Saved IQ

A separate 0.25-second / 5,000,000-sample channel 11 recording was saved without
overflow and decoded through the production offline burst pipeline. Two OFDM
beacons passed FCS. The existing IQ tool uses gain 20 dB, unlike the scanner's
40 dB campaign, so its yield must not be directly compared with the scanner.

Its manifest has no USRP device-time anchor. The replay rule correctly excluded
that capture (zero evaluations, one excluded capture). The file validates IQ to
beacon decoding only. Device-time-aware IQ manifests are required before saved
IQ can reproduce the timing-sensitive rule across captures.

## Artifacts and reproduction

All paths below are relative to the repository root; the data is local test
output, not a newly committed fixture corpus.

- `data/wifi_security_validation/2026-09-28-summary.json`: measured summary.
- `data/wifi_security_validation/2026-09-28-ch36/`: live logs, snapshots,
  offline result, receiver log and isolated state.
- `data/wifi_security_validation/2026-09-28-ch11/`: corresponding 2.4 GHz run.
- `data/wifi_security_validation/2026-09-28-derived/`: offline-only generator,
  provenance, ten input streams, snapshots and assertions.
- `data/wifi_security_validation/2026-09-28-iq/`: IQ, manifest, decoded frames,
  summary and receiver log.

`wifi_scan_smoke` now supports a bounded fixed-channel recording mode:

```sh
# Output directories must be new. These commands receive only.
./build/wifi_scan_smoke 5g 36 75 NEW_CH36_DIRECTORY
./build/wifi_scan_smoke 2g4 11 60 NEW_CH11_DIRECTORY
./build/wifi_security_replay data/wifi_security_validation/2026-09-28-ch36/events.ndjson.1 data/wifi_security_validation/2026-09-28-ch36/events.ndjson
./build/wifi_security_replay data/wifi_security_validation/2026-09-28-ch11/events.ndjson
python3 data/wifi_security_validation/2026-09-28-derived/check.py
```

Campaign exit success requires accepted frames and eligible beacon evaluations;
thus the channel 36 run returned failure for beacon validation despite successful
radio reception. The ordinary two-argument smoke mode remains available. Invalid
channel/duration requests are rejected before radio creation. Recording remains
size-bounded; longer campaigns can rotate away initial context, so retain all
segments externally or increase recording capacity before a long soak.

## Remaining acceptance gates

The user identified an ESP32-S3 Heltec-style V3 with display and no RF shield box.
No ESP32 serial device was visible during this session; only an ST-Link serial
interface was listed. No independent reference adapter was established.

1. Preserve sample-exact device timing in IQ recordings and replay it offline.
2. Verify ESP32 transmit support and actual on-air field preservation, with an
   independent monitor-mode receiver. Do not assume raw API support for deauth.
3. Establish RF isolation before disruptive or impersonating replay experiments.
   Until then, use receive-only observations and offline derived cases.
4. Run labelled isolated positive/negative RF cases with reference captures and
   endpoint logs, including normal retries, AP restart and receiver loss.
5. Measure RF acquisition yield separately from rule recall, then do held-out
   multi-device sessions and a multi-day benign soak before threshold calibration.

No confirmed attack impact, measured detection sensitivity, authenticated sender
attribution or completed DoS hardware validation is claimed.
