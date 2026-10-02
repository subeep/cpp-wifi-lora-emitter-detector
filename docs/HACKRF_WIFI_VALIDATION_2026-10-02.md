# Conducted HackRF/X310 Wi-Fi validation — 2026-10-02

## Setup and scope

The user confirmed a conducted cable path: HackRF SMA → 30 dB, 5 W attenuator →
X310 RF A RX2, with no antenna on that path. HackRF One serial
`0000000000000000f77c60dc261977c3`, firmware `git-6f26b74e` API 1.07;
X310 serial `326CF02` at `192.168.10.2`.

All waveforms use legacy OFDM at 6 Mbps, 20 Msps on channel 11 (2462 MHz).
HackRF RF amplifier and antenna power remain disabled. Its verified test gain
is 20 dB; X310 requested RX gain is 20 dB, within the UBX gain range. The X310
receives with the production 1.5 MHz tuning offset. No LoRa operation, device
firmware update, real SSID/BSSID imitation or connected-client disruption is
part of this campaign.

The fixed synthetic flood/baseline BSSID is `02:00:00:fa:ce:01`, SSID
`RFMON-HACKRF-LAB`, with four fabricated client addresses
`02:00:00:fa:ce:10` through `02:00:00:fa:ce:13`. The replay sequence uses a fresh
lab BSSID `02:00:00:fa:ce:02` so the long baseline cannot exhaust its history.

Evidence root: `data/hackrf_wifi_validation/2026-10-02/conducted-104606/`.
All scanner stores/logs are isolated under this root. The main application's
identity and security stores are not used.

## Preflight and initial trials

The existing Python OFDM fixture encoder now exposes an importable MPDU encoder
and writes files only from its script entry point. Its existing fixture campaign
still passed 88 decode/classification checks. Quantised signed-8-bit IQ vectors
were re-decoded with exact MPDU equality, valid FCS and 6 Mbps rate through the
production OFDM decoder; the final generator produced 63 passing packet vectors.
This is software self-consistency, not an independent RF oracle.

- Minimum-gain sparse beacon probe: 12 clean half-second IQ captures, zero
  decoded frames. This is an unsuccessful reception setting, not a detector test.
- Gain 12 dense-beacon probe: 18 exact FCS-valid lab beacons, no other accepted
  frames and no incidents.
- Gain 20 dense-beacon probe: 19 exact FCS-valid lab beacons, no other accepted
  frames and no incidents. Peak recorded component was 0.02164 of full scale.
  Both successful probes had no receive overflows, failures or burst caps.
- First production campaign: clean baseline matured and a six-second retry-only
  control created no incident. Its sparse replay trial lacked the full decoded
  original/newer/repeated sequence at the checkpoint. The runner stopped before
  any positive flood phase. It had checked before completed later captures were
  available; this is not a validated negative replay-detector result.

The runner now waits for a completed capture spanning the transmission end,
rather than sleeping a fixed interval. The revised replay waveform redundantly
sends original A, newer B, then old A. The retry control runs last: its benign
raw management traffic otherwise raises the learned p95 reference (9.1/s in the
first trial), legitimately making the subsequent comparison more conservative.
No detection threshold was lowered or detector rule modified for these tests.

## Finite test sequence

1. Forty nominal RF seconds of identical beacons without an intervening newer
   TSF. This must mature a clean reference without a replay incident.
2. Four nominal RF seconds of A → higher-TSF B → delayed identical A, using
   redundant packets and a fresh identity. Expect historical-beacon replay.
3. Six seconds each of target distinct, target repeated, AP-wide distinct and
   AP-wide repeated disconnect frames. Flood files contain 33 frames per second;
   cyclic files repeat sequences/content, so distinct and separated-copy counts
   differ. Use measured receiver evidence for incident numerators.
4. Three seconds of quiet to expire prior flood windows, then six seconds of
   Retry-marked identical frames. Expect no new incident or renewed observation.
5. Drain/stop the production scanner and compare all live semantic fields and
   its recent-event tail against offline replay of the recording.

Every transmit has a finite sample count, disabled amplification and a watchdog.
Execution JSON and TX logs retain commands, phase times, tool exits, completed
capture checkpoints and outcomes. Command UTC times are not exact hardware RF
start timestamps. Receiver event times come from the X310 sample anchors.

## Completed results

Successful run: `campaign-complete/`, 11:14:16–11:16:09 UTC
(16:44:16–16:46:09 IST). It returned 104 full one-second device-timed captures,
with no overflow, timeout, exception, burst cap, queue loss, rejected input,
out-of-order timing or unknown gap. Sampled/analysed exposure was 104 seconds;
measured gaps totalled 4.868899 seconds (95.53% sampled duty).

The monitor accepted 1,114 FCS-valid OFDM frames. Every accepted MPDU exactly
matched one of the fixed lab references and had a device timestamp: 303 beacons,
647 non-Retry deauth frames and 164 Retry-marked deauth frames. Of 1,309 OFDM
decode attempts, 1,114 were FCS-valid; that ratio is not absolute packet recall.

| Phase | Newly accepted frames at checkpoint | Result |
|---|---:|---|
| Clean baseline | 289 | Mature baseline; zero incidents |
| Historical beacon replay | 14 | Replay incident, with original/newer/repeated evidence |
| Target distinct flood | 162 | Distinct target path observed |
| Target repeated flood | 165 | Repeated-content target path observed |
| AP-wide distinct flood | 159 | AP-wide distinct path observed |
| AP-wide repeated flood | 161 | AP-wide repeated path observed |
| Retry-only control | 164 | No new incident or renewed observation |

The live monitor produced **three coalesced incidents**, rather than five
duplicate alerts: one historical-beacon incident (five observations), one target
disconnect incident (11 observations covering both target paths), and one AP-wide
disconnect incident (10 observations covering both AP paths). All five paths
remain visible in their evidence timelines. Complete live/offline semantic
snapshots and the live recent-event tail matched; only recording-byte accounting
and recent-list capacity are intentionally different.

`campaign-complete/execution.json`, `validation-summary.json`,
`receiver/snapshot.json`, `receiver/events.ndjson`, phase-prefix recordings and
phase snapshots retain the proof and commands. Both radio tools exited normally
and no transmitter/scanner process remains running.

A second earlier run (`campaign-final/`) stopped on a timeout/overflow in its
first pre-transmission capture. The monitor excluded that capture and still
learned 30 clean seconds. The revised executor records startup-only exclusions,
requires a clean ready capture, and treats later degradation as a failed test.
The successful run had **zero startup exclusions**. A stop-file mechanism now
drains the native scanner gracefully on success or failure; hard termination is
only a timeout fallback. These are test harness changes, not detector changes.

## Software and GUI verification

All 63 unique quantized reference MPDUs decoded with exact bytes, valid FCS and
the expected 6 Mbps rate in the production decoder before transmission. The
existing regenerated OFDM fixture suite passed 88 checks with zero failures.
All 13 registered CTest suites passed after the final tool and GUI changes.

The production Wi-Fi Security panel was also rendered offscreen from the actual
successful receiver recording. Its three incident rows, coverage counters and
AP-wide evidence dialog were checked; the automated click opened the actual
recorded incident and the evidence screenshot was visually inspected. This is
a GUI replay of the completed hardware recording, not a desktop live-session
observation. Images:
[incident table](../data/hackrf_wifi_validation/2026-10-02/conducted-104606/campaign-complete/gui-hardware-incidents.png)
and [evidence dialog](../data/hackrf_wifi_validation/2026-10-02/conducted-104606/campaign-complete/gui-hardware-evidence.png).

## Tools and remaining limits

`tools/prepare_wifi_hackrf_validation.py NEW_DIRECTORY` only generates fixed
lab files/reference checksums; it never accesses radio hardware.
`tools/run_wifi_hackrf_validation.py VECTORS NEW_DIRECTORY --hackrf-serial SERIAL
--attenuation-db 30` is explicitly a conducted-only executor; physical setup
must be confirmed and benign RF reception verified first. It stops on missing
coverage, loss, degraded captures, unexpected negative-control incidents or a
missing expected positive. `wifi_scan_smoke` accepts an optional `--gain DB`
for a known gain within the X310 range; default behaviour is retained otherwise.
Its optional `--stop-file PATH` permits graceful finite-capture shutdown and
queue drain for the executor.

This campaign tests classification of decoded synthetic management frames.
It does not establish client disruption, PMF acceptance, authenticated attacker
identity, DSSS/other-rate performance or RF-interference/jamming detection.
Some emitted packets are missed or fail decoding; no independent third-party
on-air capture is available to measure absolute transmitter/receiver recall.
Measured threshold calibration, reconnect/reboot negatives and longer benign
soak/held-out evaluation remain open even if all positive paths pass here.
