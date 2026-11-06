# LoRa tab: live over-the-air validation, 2026-11-06

Ground-truth test of the LoRa tab changes (wideband listening, channel plans,
LoRaWAN devices, RF emitter review, keyless LoRaWAN rules, channel
suggestions) with a real transmitter and the production `Scanner`.

## Setup

| | |
|---|---|
| Transmitter | Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262), MAC 70:af:09:d6:82:c4, own antenna |
| Receiver | USRP X310 (UBX, RX2), 1 GbE, gain 5 dB, sub-GHz antenna |
| Path | Over the air, about 2-3 m, fixed placement |
| Power / PHY | 0 dBm (one run at -9 dBm), BW125, CR 4/5, sync 0x12, explicit header, CRC on |
| Channels | IN865: 865.0625, 865.4025, 865.985, 866.9 MHz; 866.5 MHz for the suggestion test |
| LoRaWAN identity | Test only: DevAddr 26FFBEEF (ABP, test session keys), DevEUI 70AF09FFFED682C4 |

The Heltec's original firmware was backed up first (full 8 MB, verified against
the chip): `data/lora_validation/2026-11-06/heltec_backup/` with restore steps
in its README. Restore it with `esptool.py write_flash 0 <file>`.

## Tools (new)

- `tools/heltec_lora_telemetry/` - PlatformIO firmware. Boots silent; the host
  sets a run tag, channels, SF/BW/CR/power and a mode: tagged ASCII telemetry,
  LoRaWAN 1.0.x uplinks with real AES encryption and MIC (verified on the host),
  raw bytes, chosen-FCnt uplinks, join requests. Logs every transmission as JSON.
- `tools/lora_validation/heltec_link.py` - drives the board on a timed script,
  logs ground truth with host time, has replay/counter-reuse macros, and always
  leaves the board in MODE OFF (also on SIGTERM/SIGHUP).
- `tools/lora_live_check.cpp` (`build/lora_live_check`) - production Scanner in
  sub-GHz with a chosen plan (wideband, `--rotate`, `--lock`), isolated data
  root, JSON of every distinct row, devices, clusters with history, incidents,
  listening statistics and capture evidence.
- `tools/lora_validation/lora_match.py` - matches transmissions to CRC-valid
  rows by exact bytes; classifies rows as matched, false positive (our tag but
  no transmission) or foreign.
- `tools/lora_validation/run_case.sh`, `run_2026-11-06.sh` - the cases below.

Raw data: `data/lora_validation/2026-11-06/<case>_{truth.ndjson,rx.json,rx.log,report.json}`.

## Results

"Listen share" = seconds of IQ captured on the channel / wall time. "In-window
efficiency" = recall / listen share, an estimate of how many packets that fell
inside a capture were decoded.

| Case | Mode | Sent | Received | Recall | Listen share | In-window eff. | Cycle | False pos. |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| t1rot | rotate, 4 ch hopping | 108 | 8 | 0.07 | 0.12 | 0.61 | 11.9 s | 0 |
| t1wide | wideband 4 MHz, 4 ch hopping | 108 | 26 | 0.24 | 0.42 | 0.58 | 13.8 s | 0 |
| t1lock | lock 865.0625 | 107 | 62 | 0.58 | 0.79 | 0.73 | 7.6 s | 0 |
| t1crc | lock (repeat) | 54 | 35 | 0.65 | 0.79 | 0.82 | 7.6 s | 0 |
| t3rules | lock, LoRaWAN + attacks | 220 | 117 | 0.53 | 0.80 | 0.66 | 7.5 s | 0 |
| t4sf | lock, SF7-SF12 | 83 | 37 | 0.45 | 0.81 | 0.55 | 7.4 s | 0 (1 logging artifact) |
| t5off | wideband, tx on 866.5 (not planned) | 45 | 0 | 0 | - | - | 13.5 s | 0 |
| t5add | wideband, 866.5 added | 45 | 12 | 0.27 | 0.38 | 0.71 | 15.9 s | 0 |
| t6p0 | lock, 0 dBm | 33 | 18 | 0.55 | 0.82 | 0.66 | 7.3 s | 0 |
| t6m9 | lock, -9 dBm | 33 | 22 | 0.67 | 0.82 | 0.81 | 7.3 s | 0 |

Across all runs: **zero decoder false positives** (every CRC-valid row carrying
our tag matched a logged transmission byte for byte), zero wrong SF/BW/frequency
reports, and no foreign LoRa traffic was heard.

### Wideband listening

- **Works, and triples per-channel coverage**: listen share 0.42 vs 0.12 per
  channel when rotating; recall 0.24 vs 0.07 for traffic hopping across the
  four channels. The 4 MHz capture is tuned to 866.332 MHz, 347 kHz from the
  nearest channel; no packet was reported on the wrong channel.
- **Not near-continuous yet.** A wideband cycle is 13.8 s: about 4.5 s of the
  pre-existing 5 Msps sub-GHz energy scan (4 x 1 s plus processing), the 6 s
  LoRa capture, and about 3 s of derivation and decoding (5.3 s with 5
  channels, which spills into a second batch of 4 parallel derivations). Only
  the 6 s are listening time.
- **Link drops at 4 Msps.** UHD reported 377 dropped-packet markers in the
  wideband run against 2-3 in the 500 ksps runs. 24 of 180 derived-channel
  captures (6 of 45 wideband captures) were excluded from evidence for
  discontinuity. NIC CRC errors moved only 861 -> 863 across the later runs,
  so this is not the cable. Peak memory is 1.4-1.7 GB vs ~0.27 GB locked.
  Cause not isolated.
- Fixed during the run: channels derived from one capture share its device
  start time, which the evidence monitor counted as clock regressions (32 of 36
  captures excluded). They are now recognised as siblings
  (`src/security/lora_security.cpp`, test in `tests/test_lora_security.cpp`
  and `tests/test_scanner_lora_security.cpp`).

### Decoding

- In lock mode, 66-82% of packets inside a capture decode. Most of the rest are
  **"Payload / CRC failed" rows with 9-41 FEC mismatches at ~45 dB fingerprint
  SNR**, which points at overload, not weak signal. At -9 dBm, CRC failures
  halved (8 -> 4) and recall rose 0.55 -> 0.67 (33 packets each; indicative).
- SF sweep (lock): SF7 0.47, SF8 0.47, SF9 0.33, SF10 0.20, SF11 0.64, SF12
  0.67 (11-15 packets each, too few to rank SFs). SF11/SF12 packets (about
  1-1.8 s on air) are not lost to the 6 s window disproportionately.

### LoRaWAN devices and rules (t3rules, lock mode)

| Rule | Attack sent | Both copies received | Fired | Control | Control quiet |
|---|---|---|---|---|---|
| Frame replayed | 5 replays after ~120 s | 2 | yes, count 2 | 3 resends within 5 s | yes (counted as retransmission) |
| Counter reused, new bytes | 5 `@REUSE` | 2 | yes | - | - |
| Counter went backwards | FCnt 141 -> 10 | yes | yes, count 10 | normal traffic with ~47% missed frames | yes |
| Join DevNonce reused | 3 nonces resent ~100 s later | 2 | yes, count 2 | 3 joins 20 s apart | yes |
| Join-request burst | 30 joins in 45 s | 15 seen | yes, once | spread joins | yes |
| Disallowed emitter | clusters marked disallowed at 850 s | - | yes | - | - |
| Address from different RF cluster | (needs a 2nd transmitter) | - | no | - | did not fire falsely |

Every rule fired whenever both halves of its evidence were received, and no
control raised an incident. The devices view showed DevAddr 26FFBEEF (92
uplinks, FCnt tracked to the last received value) and the DevEUI's 20 join
requests. The FCnt reset also raised "counter reused" for the repeated
counters, as designed: a reset and a forgery look the same without keys.

### RF clusters

**One Heltec produced 10-20 clusters per run.** Its IQ-imbalance fingerprint
spreads wider than the matcher's tolerances: IQ epsilon stdev 0.03 vs a +-0.02
tolerance, IRR p10-p90 over ~15 dB vs +-8 dB, fit EVM median 45-52%. CFO swings
about +-60 ppm. Consequences:

- Review labels and "Disallowed emitter" apply to fragments, not to the device
  (8 different clusters raised the disallowed rule in t3rules).
- The "address from a different RF cluster" rule did not fire falsely, because
  no cluster reached its 80% baseline. With fingerprints this unstable it is
  effectively blind; it was not exercised against a real second transmitter.

### Channel suggestions

866.5 MHz energy outside the plan was suggested (12 hits; its 264 kHz width is
the energy-scan estimate, not the LoRa BW). After adding it, its packets
decoded (12/45 at a 0.38 listen share), and the suggestion disappeared.

## Pass criteria vs. results

| Criterion (from the plan) | Result |
|---|---|
| >= 90% of SF7-SF10 in-window packets decoded | **Not met**: 55-82%, mainly CRC failures (overload suspected) |
| >= 85% recall in wideband | **Not met**: 0.24; listening share 0.42 is the limit |
| Zero false positives | **Met** (one logging artifact, fixed in the logger) |
| Every attack -> exactly one incident, controls quiet | **Met** where both halves were received |
| No overflows at 4 Msps | **Not met**: about 13% of wideband captures had link drops |

## Follow-ups

1. **Listening share**: done for the energy scan (see the 20 MHz addendum:
   0.42 -> 0.69). Overlapping the next capture with decoding is still open.
2. **4 Msps drops**: reproduce with `rx_samples_to_file --rate 4e6` for 6 s
   captures; check UHD `recv_buff_size` and the NIC ring (currently 256 of 4096).
3. **Overload**: repeat t6 with more distance or X310 gain 0 dB to confirm the
   CRC-failure cause before touching the decoder.
4. **RF fingerprint**: the clustering is not usable as identity for this device
   over the air; re-examine the IQ-imbalance fit (EVM 45%+) before relying on
   cluster labels or the RF-mismatch rule.
5. BW250/500 were not transmitted over the air (not confirmed as allowed here);
   they remain covered only by offline fixtures.

## Addendum: 20 MHz LoRa listening (same day)

LoRa wideband listening now defaults to a 20 MHz capture, the rate the Wi-Fi,
drone and coordinated paths use (Receiver sub-tab: "Capture width"; Auto, 4,
8, 10 MHz remain). Changes:

- `src/lora_channelizer.cpp`: overlap-save FFT channelizer. All channels come
  out of one parallel pass: 6 s of 20 Msps (120 M samples), 4 channels in
  0.25 s, against 4.6 s and ~4.9 GB with the previous per-channel mix + FIR.
  Matches the old derivation on tone channels (correlation 0.999997), keeps
  phase continuous across blocks, and rejects out-of-channel energy by 108 dB
  (`tests/test_lora_channelizer.cpp`).
- When the plan is one wideband capture that also keeps 863-868 MHz, it
  replaces the separate 5 Msps sub-GHz energy scan; its own energy map (same
  2.44 kHz bins, computed alongside decoding) feeds Active emitters and the
  channel suggestions. RF observations show the capture's full span.
- Per-capture bound raised to 125 M samples (20 Msps x 6.25 s); longer windows
  fall back to a narrower width automatically.
- New preset "IN865 + EU868 (863-870 MHz)" (13 channels, one 20 MHz capture).

| Case | Width | Channels | Cycle | Listen share | Recall (4 IN865 ch, hopping) | Evidence excluded | UHD drop markers | Peak memory |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| t1wide | 4 MHz + 5 Msps energy scan | 4 | 13.8 s | 0.42 | 0.24 (26/108) | 24/180 (13%) | 377 | 1.7 GB |
| t7wide20 | 20 MHz, energy scan replaced | 4 | 8.7 s | 0.69 | 0.36 (38/106) | 136/288 (47%) | 840 | 2.2 GB |
| t7broad20 | 20 MHz, energy scan replaced | 13 | 11.2 s | 0.54 | 0.28 (15/54) | 153/153 (100%) | 941 | 2.8 GB |

Zero false positives and no incidents in both 20 MHz runs. The Heltec stayed on
IN865 frequencies; t7broad20 only widened what the receiver decoded. With the
broader view, the energy map suggested **870.5375 MHz** (LoRa-like, 4 hits),
which is outside both regional plans; nothing of ours was there.

**The 1 GbE link is now the limit, not the LoRa code.** NIC Ethernet CRC errors
rose 863 -> 2585 during the 20 MHz runs. UHD's own `rx_samples_to_file`, with
no scanner involved, also drops at 20, 10 and 4 Msps (54-57 drop markers per
3 x 6 s at 4-10 Msps; NIC CRC errors +15 to +91 per test). On 2026-11-04 the
same link streamed 25 Msps cleanly, so something physical has degraded: cable,
connector or NIC port. Corrupted captures are excluded from evidence and lose
packets, which is why recall rose less than listen share (in-window efficiency
0.52 at 20 MHz vs 0.58 at 4 MHz).

Next: reseat or replace the X310 Ethernet cable (or try another NIC port),
confirm with `rx_samples_to_file --rate 20e6 --duration 6` and
`ethtool -S enp0s31f6 | grep rx_crc_errors` (no increase), then rerun
`RUN_20MHZ_ONLY=1 tools/lora_validation/run_2026-11-06.sh`.
