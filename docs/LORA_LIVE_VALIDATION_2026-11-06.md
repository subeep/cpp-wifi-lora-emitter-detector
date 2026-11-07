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
2. **Link drops**: resolved by the cable fix (zero drops and flat NIC CRC
   errors at 20 Msps afterwards; see the addendum). The 4 Msps drops earlier in
   the day were most likely the same failing cable.
3. **Overload**: superseded; the CRC failures were mainly the decoder's
   sub-chip timing sensitivity (see follow-up 8).
4. **RF fingerprint**: the clustering is not usable as identity for this device
   over the air; re-examine the IQ-imbalance fit (EVM 45%+) before relying on
   cluster labels or the RF-mismatch rule.
5. BW250/500 were not transmitted over the air (not confirmed as allowed here);
   they remain covered only by offline fixtures.
6. **IQ-imbalance images**: fixed (planner mirror clearance + image tag; no
   ghosts in the live rerun).
7. **Adjacent-channel aliasing**: fixed (boxcar + low-pass,
   `lora_decimator.hpp`); live: no rows on unused channels.
8. **Sub-chip timing sensitivity**: fixed (phase retry); live recall
   0.41 -> 0.72, CRC failures 29 -> 2. A fractional timing estimate in the
   receiver itself would also cover BW500 (no spare phases at 500 kHz).

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
| t7wide20 (degraded link) | 20 MHz, energy scan replaced | 4 | 8.7 s | 0.69 | 0.36 (38/106) | 136/288 (47%) | 840 | 2.2 GB |
| t7broad20 (degraded link) | 20 MHz, energy scan replaced | 13 | 11.2 s | 0.54 | 0.28 (15/54) | 153/153 (100%) | 941 | 2.8 GB |
| **t7wide20 (cable fixed)** | 20 MHz, energy scan replaced | 4 | 8.2 s | 0.73 | **0.50 (54/107)** | 0/304 | 0 | 2.1 GB |
| **t7broad20 (cable fixed, queue fix)** | 20 MHz, energy scan replaced | 13 | 10.5 s | 0.57 | **0.43 (23/54)** | 0/403 | 0 | 2.7 GB |

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

### Rerun after the cable fix

The user reseated/replaced the cable. Before the rerun, `rx_samples_to_file`
streamed 3 x 6 s (20, 20, 25 Msps) with no drop markers, a full-size file
(960 MB for 20 Msps x 6 s) and NIC `rx_crc_errors` flat at 2949; it stayed at
2949 through both cases. Degraded-link data moved to
`data/lora_validation/2026-11-06/degraded_link_20mhz/`.

- **t7wide20**: recall 0.50 (per channel 0.67 / 0.52 / 0.52 / 0.31), listen
  share 0.73, zero UHD drops, zero evidence exclusions, zero false positives,
  no incidents. Detection is now close to listen share: 54 CRC-valid plus 21
  genuine CRC-failed receptions (about 75 of 107 sent, 0.70). The other 7 of
  the 28 CRC-failed rows are mirror images (see below): the lab default plan's
  centre (865.505 MHz) puts 865.0625 and 865.985 on each other's mirror. The
  genuine failures (865.985: 6, 866.9: 9, 865.4025: 6) follow the earlier
  overload pattern (follow-up 3).
- **Bug found and fixed: evidence queue too small for wideband.** The first
  13-channel rerun lost 233 of 403 per-channel evidence batches
  (`input_loss` 233, 36 gaps): the LoRa evidence monitor queue held 4 batches,
  enough for rotation (one batch per capture) but not for one batch per derived
  channel submitted at once. The queue is now `2 x lora_plan::MAX_CHANNELS`
  (`src/scanner.cpp`); `tests/test_scanner_lora_security.cpp` now runs the
  20 MHz case with 13 channels and requires `input_loss == 0` (it fails 3/3
  with the old size). Data from the lossy run is in `broad20_queue4/`.
- **t7broad20 after the fix**: recall 0.43, all 403 channel captures accepted
  (`input_loss` 0, no gaps), zero false positives, no channel suggestions.
- **New finding: IQ-imbalance images.** In the 13-channel run, 7 CRC-failed
  rows appeared on 867.1 and 867.5 MHz, where nothing of ours transmits. They
  are images of our own packets mirrored about the capture centre
  (866.263 MHz): 2 x 866.263 - 865.0625 = 867.464 MHz, matching the rows'
  -35 kHz CFO on 867.5; 867.1 + 25 kHz mirrors 865.4025. They carry our sync
  word and length (36-37) and decode only as inverted IQ, as a conjugate image
  would. UHD reports that RX IQ balance cannot be set on this X310
  daughterboard. All 7 failed CRC, so no false identity, but the 20 MHz plan
  could place one planned channel on another's mirror. The 4-channel lab
  default had the same collision: 7 of its 28 CRC-failed rows were images
  (865.0625 <-> 865.985 about 865.505 MHz), so its genuine detection was
  about 75 of 107, not 81.

### IQ mirror fix

- **Planner** (`src/lora_channel_plan.hpp`): `place_center` now also requires
  `mirror_clearance` (smallest |a + b - 2*centre| over all channel pairs, a
  channel with itself included) of at least 250 kHz, the largest offset the
  decoder accepts (BW500: half the bandwidth). Wider rates are tried when the
  narrowest cannot satisfy it; failing that, the DC-clear centre with the most
  mirror clearance is used. Every preset is now image-clear at 20 MHz and in
  Auto width. Lab default: 865.505 -> 865.820 MHz (DC 165 kHz, mirror
  252.5 kHz); 13-channel preset: 866.263 -> 865.855 MHz. The Receiver plan
  table and `lora_live_check` show the mirror clearance.
- **Decoder tag** (`tag_lora_iq_images`, `src/lora_observation.cpp`): on a
  wideband capture, a decoded, not-CRC-valid row is tagged as a likely image
  (`iq_image_of_mhz`, detail text, "IQ image of ..." in the packet table,
  exported) when a packet of the same SF/BW and opposite IQ polarity, CRC-valid
  or with the stronger preamble, starts within 4 symbols at its mirror
  frequency. This covers plans that cannot be image-clear and strong signals
  outside the plan.
- Tests: planner mirror clearance for every preset (20 MHz and Auto), the two
  live collisions reproduced, tagging with the live numbers plus controls
  (other time, same polarity, CRC-valid, other SF, weaker-of-pair).

Live rerun with the fix (link clean, NIC CRC flat at 2949, zero UHD drops,
zero evidence exclusions or input loss):

| Case | Centre | Mirror clearance | Recall | CRC-valid + genuine CRC-failed | Ghost rows |
|---|---:|---:|---:|---:|---:|
| t7wide20 before | 865.505 | 37.5 kHz | 0.50 (54/107) | 55 + 21 = 76 | 7 |
| t7wide20 after | 865.820 | 252.5 kHz | 0.41 (44/107) | 44 + 29 = 73 | **0** |
| t7broad20 before | 866.263 | ~36 kHz | 0.43 (23/54) | | 7 |
| t7broad20 after | 865.855 | 252.5 kHz | 0.41 (22/54) | | **0** |

No mirror ghosts remain (no inverted-IQ rows at a mirror offset; the tagger
had nothing to tag). Detection is unchanged (73 vs 76 of 107); the recall
difference is the CRC-valid/failed split, which moves run to run with the
near-field overload (follow-up 3). That the new centre causes it is not
excluded but not supported either: all four channels now fail at similar rates
(6-8 each).

**Separate finding: adjacent-channel aliasing in the decoder.** Both
13-channel runs (at either centre) logged CRC-failed rows on 867.1 MHz with
CFO about +50 kHz and normal IQ. Their bytes partly match packets sent on
866.9 MHz (18-21 of 37 bytes). 866.9 lies inside 867.1's 500 kHz decode
window at -200 kHz, and folds to -200 + 250 = +50 kHz when the BW125
hypothesis resamples to 125 kHz, so the resampling filter is not removing it.
Not a planner issue (it also applies to rotation mode, wherever two planned
channels are 200 kHz apart, as in EU868). All failed CRC.

### Adjacent-channel aliasing fix (2026-11-07)

**Cause.** Each bandwidth hypothesis decimated the 500 kHz channel IQ with a
plain boxcar (average of 4 samples for BW125). Its response at 200 kHz is
only about -12 dB, and 200 kHz folds to +50 kHz at the 125 kHz output rate,
which matches the live rows exactly. Offline, the real SX1262 fixture moved
200 kHz off-channel decodes **CRC-valid** on the wrong channel with the
boxcar (12 of 12 neighbour cases at 150/200 kHz produced a decode).

**Fix.** `src/lora_decimator.hpp` (header-only), used by both the live scanner
(`analyze_lora_iq`) and offline replay (`analyze_lora_capture`), so scanner
and replay evidence stay identical: the same boxcar, preceded by a Kaiser
low-pass at the output rate, as one combined FIR (52 taps for BW125). In band
it matches the boxcar to within 0.001 dB (so existing decodes do not change);
200 kHz is now -86 dB, 160 kHz -84 dB. BW250 (factor 2) is exactly the old
boxcar. Sample timing is unchanged (output k centred on input
4k + 1.5, as before).

A sharper flat-passband filter was tried first: it rejected the neighbour
just as well but broke one of the three recorded SX1262 fixtures, which led to
the next finding.

Tests: `tests/test_lora_decimator.cpp` (in-band match to the boxcar,
rejection, timing, taps); `tests/test_scanner_lora_security.cpp` decodes the
fixture on its own channel and requires no BW125 decode with it 200 kHz away
(fails with the old boxcar). Full suite 84/84.

**New finding: the decoder is sensitive to sub-chip timing.** Shifting the
500 kHz input by 0-3 samples (a quarter chip each) before decimation, over
the 3 fixtures and CFOs of 0, +/-10 and +/-20 kHz, only 44 of 60 cases decode
CRC-valid with the old boxcar (43/60 with a sharp filter, 44/60 with the new
one). The failures are FEC/CRC errors on strong, clean packets (e.g. fixtures
1 and 2 fail at a 3-sample shift with 16-19 FEC disagreements). Live packets
arrive at random sub-chip timing, so this alone predicts roughly a quarter of
strong packets failing CRC, which fits the live CRC-failure rate better than
near-field overload. Fix candidates (decoder change, not done): decode at
the other sub-chip phases (the 500 kHz input already has 4) when a header
decodes but the CRC fails, or estimate and correct fractional timing before
demodulation.

**Live check.** The first rerun on 2026-11-06 could not start the X310 (UHD
"x300 fw communication failure"; ping and discovery worked); it was stopped
with the Heltec switched to MODE OFF. On 2026-11-07 the next attempt had a
working X310 but the Heltec was unplugged (`/dev/ttyUSB0` missing), so
nothing was transmitted. That run is kept as a quiet-air control
(`quiet_control_no_heltec/`): 325 s, 32 captures of all 13 channels,
**zero decodes of any kind**, no evidence loss.

With the Heltec reconnected (X310 needed one init retry, then captured):

| 13-channel run | Heltec packets on 866.9 MHz | Rows on 867.1 MHz (alias) | Rows on any channel the Heltec never used | Recall |
|---|---:|---:|---:|---:|
| Before mirror fix | 13 | 1 alias (+1 mirror) | 7 | 0.43 |
| Mirror fix, old decimator | 13 | 3 | 3 | 0.41 |
| **Mirror + anti-alias fix** | 13 | **0** | **0** | 0.38 (21/55) |

Zero false positives, no incidents, no tagged images. The link was slightly
worse than the day before (15 overflow polls, 13 of 403 channel captures
excluded from evidence, one NIC CRC error), which costs some recall; after the
two init failures, the X310 or its link may need attention again. The energy
map flagged **870.5375 MHz** again (15 hits): now seen in three separate runs,
so an external LoRa-like emitter near 870.54 MHz is real (not ours).

### Sub-chip timing fix (2026-11-07)

**Cause.** The explicit-PHY receiver (`src/lora_receiver.cpp`) aligns timing
to whole samples at one sample per chip, so the leftover sub-chip offset
depends on where the 500 kHz samples fall within a chip. Over the three
recorded SX1262 packets x 4 quarter-chip phases x CFO 0/+/-10/+/-20 kHz, one
phase decodes 44 of 60 cases CRC-valid; every failure decoded its header and
failed only the payload CRC. Decoding the same packet at any of the four
phases recovers **60 of 60**.

**Fix.** `analyze_lora_packets_phased()` (`src/lora_observation.cpp`), used by
both the live scanner and offline replay: decode at phase 0 as before; for
each explicit-PHY row with a valid header and a failed CRC, re-decode just
that packet's span at the other sub-chip phases of the 500 kHz channel IQ
(3 for BW125, 1 for BW250) and replace the row with the first CRC-valid
result of the same polarity and data start (within 2 samples). Timing is
mapped back to the phase-0 grid and the row's detail says it was recovered.
The CRC gates every replacement, so this cannot invent a packet. Because a
clean mirror image could now also decode CRC-valid, `tag_lora_iq_images` also
tags a CRC-valid row that has the same bytes as a stronger, mirrored,
opposite-polarity packet, and tagged images are kept out of the LoRaWAN
device list and the keyless rules.

Tests: `tests/test_scanner_lora_security.cpp` decodes all three recorded
packets at every quarter-chip phase with CFO 0/+/-20 kHz (36/36 CRC-valid, 8
by the retry); `tests/test_lora_observation.cpp` covers CRC-valid images.
Full suite 84/84.

**Live** (4 channels, 20 MHz, same setup as before):

| Run | Recall | CRC-valid | CRC-failed | Recovered by retry | Listen share | Cycle | Decode / capture |
|---|---:|---:|---:|---:|---:|---:|---:|
| Mirror fix (before) | 0.41 (44/107) | 44 | 29 | - | 0.72 | 8.3 s | 1.83 s |
| **Phase retry** | **0.72 (76/106)** | 76 | **2** | 27 | 0.73 | 8.2 s | 1.73 s |

Recall now equals the listen share: nearly every packet that arrives while
its channel is being captured decodes. Per channel 0.69-0.74. Zero false
positives, no incidents, no link drops, no tagged images.

**Correction to earlier findings.** The high-SNR CRC failures earlier in this
report (t1crc, t6, and the 20 MHz runs) were attributed to near-field
overload. They were mostly this sub-chip timing sensitivity; the -9 dBm
improvement was likely chance in a small sample. Follow-up 3 (overload) is
no longer needed to explain them. The remaining recall limit is the listen
share (capture plus decode time), i.e. follow-up 1.

### RF cluster duplication fix (2026-11-07)

**Cause.** RF clusters (`lora_master`) were fed by `extract_lora_fingerprint`
placed by `lora::detect_burst`, whose start is only symbol-granular: its
`cfo_bins` (27-32 on the recorded SX1262 packets, whose decoder CFO is
-122 Hz) was really the arrival time within a symbol, and the fit window took
in pre-burst samples and sync-word symbols. Fit EVM 43-70%; features moved
with sampling phase as much as between devices; readings were also recorded
from wrong SF/BW hypotheses (271 of 542 stored). Within the Heltec the
`iq_eps` spread (sd 0.030) exceeded its whole match tolerance (0.02), so most
readings missed their cluster: 39 clusters for two boards, many carrying
DevAddr 26FFBEEF.

**Fix.** `fingerprint::extract_lora_fingerprint_at` (explicit fit/noise
positions) and `fingerprint_decoded_row` (`src/scanner.cpp`): every CRC-valid
decoded uplink is fingerprinted from the decoder's alignment, fitting the
clean preamble up-chirps before the sync word. Candidates: each sub-chip
phase x +/-1 sample x neighbouring CFO bin (an up-chirp cannot tell a
one-sample shift from a one-bin CFO shift); the SFD down-chirps break the tie
(residual bins after removing the fit's CFO), then lowest EVM. Only these
fingerprints feed RF clusters, the session registry and the disallowed-emitter
check; each decoded row gets its cluster directly. The burst-search fit
remains a diagnostic on detected-only rows. Test: the three recorded packets
at all four sub-chip phases (12 inputs) through the production listen path
form exactly one cluster (`tests/test_scanner_lora_security.cpp`). Full suite
84/84.

The old 41-cluster store was moved to
`data/lora_master_legacy_burst_fingerprint_2026-11-07/` (not deleted).

**Live** (GUI, 20 MHz, Heltec LoRaWAN uplinks hopping 4 IN865 channels):
after 4 minutes, **1 cluster, 29 readings across all 4 channels**.

| Feature | Heltec live (29) | Old fingerprint, Heltec | Same Heltec, 2026-09-29 recordings (new method) |
|---|---|---|---|
| CFO (ppm) | -0.055 +/- 0.06 | -6.0 +/- 38.8 | +0.09 to +0.12 |
| IRR (dB) | -34.7 +/- 0.36 | -36.7 +/- 6.6 | -34.1 to -35.0 |
| DC (dBc) | -47.9 +/- 1.7 | -31.8 +/- 7.0 | -46.5 to -50.3 |
| IQ eps | 0.025 +/- 0.002 | 0.003 +/- 0.030 | 0.025 to 0.030 |
| IQ phase (deg) | 1.52 +/- 0.19 | -0.2 +/- 1.9 | 1.38 to 1.49 |
| Fit EVM | 21 +/- 2.5% | 43 +/- 15% | 19-25% |

**Correction (same day): the "other SX1262 board" was this Heltec.** The
888 MHz recordings (`tests/fixtures/lora_sx1262/`) were captured on
2026-09-29 from the Heltec MAC 70:af:09:d6:82:c4 while it still ran its
earlier auto-transmit firmware (docs/ESP32_WIFI_RECEPTION_CHECK.md,
lora_handover.md). So the near-identical IRR/DC/IQ values above are one
device measured weeks apart, at 888 vs 865 MHz, in 500 kHz vs 20 MHz capture
mode: evidence that the fingerprint is **stable for one device**, not that it
fails to separate devices. Separation between different devices is still
untested; the column header "Other SX1262 board" should read "Same Heltec,
2026-09-29 recordings". Next: record a different transmitter (a second
TarangMini ST22LR01, EUI 3cc1f6050005d8eb, TarangNet v0_0_6, 865.1 MHz,
SF7/BW125, power 22 dBm default) in the same session as the Heltec. The fit's
remaining ~20% EVM is still model error worth removing (receive-filter
reference) before judging separation.

**TarangMini check (2026-11-07, no configuration changes).** The second
TarangMini (EUI 3cc1f6050005d8eb, router mode, 865.1 MHz, 22 dBm) answered
data-send commands with "Uplink ACK" (`2b038100`) but put nothing on air:
2 Msps and 20 MHz captures during sends showed no burst near 865.1 MHz (at
22 dBm and 2-3 m it would sit near -8 dBFS; the Heltec at 0 dBm read
-30.6 dBFS at X310 gain 0). A 10-minute passive watch with the board next to
the X310 (5 decoded channels incl. 865.1, energy map over 858-873 MHz,
listen-only serial logger) recorded no packets, no LoRa-like energy and no
serial events. Consistent with a router waiting for a root. The two-device
fingerprint comparison is on hold until it has a root (another TarangMini as
root) or is switched to root mode. Sender for later use:
`tools/lora_validation/tarangmini_send.py` (send commands only, logs settings
and every send).

## Efficiency, capture/decode overlap and fingerprint robustness (2026-11-07)

Goal: run well on a modest PC (baseline 4 cores, 8 GB), not this 20-core
machine. New radio-free benchmark `build/lora_bench` (tools/lora_bench.cpp):
a 6 s, 20 Msps capture with receiver noise and the recorded SX1262 packet
FFT-upsampled onto the planned channels, through the production
capture/process path; restrict cores with `taskset`.

| Processing one 6 s, 20 MHz capture | Before | After |
|---|---:|---:|
| 4 cores, 4 channels | 5.2 s wall, 17.5 CPU-s | **2.1 s, 7.7 CPU-s** (2.8x real time) |
| 4 cores, 13 channels | 12.8 s, 48 CPU-s (falls behind) | **5.1 s, 19.8 CPU-s** (keeps up) |
| 2 cores, 4 channels | - | 4.1 s (1.5x real time) |
| 2 cores, 13 channels | - | 9.9 s (falls behind: use Auto width or fewer channels) |
| Per decoded packet (fingerprint, phase retry) | ~21 ms CPU | ~18 ms CPU |

LoRa processing needs ~1-1.2 GB at 20 MHz (one capture plus channel IQ).

Changes:
- Receiver window scan (`lora_receiver.cpp`): squared-norm peak search and
  sqrt instead of `std::abs`/`std::hypot` (runs per window, SF, BW, polarity).
- Legacy burst search only when the receiver found a preamble candidate (it
  repeats the receiver's scan with a stricter rule); always for SF5/6, which
  the receiver does not scan.
- Energy map: every 4th 8192-point frame (one per 1.6 ms) for wideband
  captures; LoRa packets (>= ~25 ms) still span dozens of frames.
- Channelizer: 1/16 overlap instead of 1/8 (filter response ~50 outputs vs 64
  overlap), and a per-block phase recurrence instead of cos/sin per sample.
- Wide capture freed once channels and the energy map are extracted.
- Fingerprint down-chirp check: phase recurrence instead of sin/cos per sample.
- Receiver panel: per-stage times and processing load vs real time (green,
  amber near the limit, red with advice when falling behind).

**Capture/decode overlap.** Each LoRa listen is split into `capture_lora`
(radio) and `process_lora` (serial worker); the scan loop takes the next
capture while the previous one is processed, commits stay in capture order,
detections reach the registry one cycle later, the radio session is recorded
at capture time, and the worker is drained on band change, coordinated mode
and stop. At most one capture waits, so a slow PC loses listening time, not
data. `RFMON_LORA_PIPELINE=0` disables it. Live (t8pipe, 4 channels, 20 MHz):
listen share **0.73 -> 0.93**, recall **0.72 -> 0.88** (95/108), zero false
positives, cycle 6.4 s.

**One-device robustness sweep** (t9robust, then t10sf; Heltec on
865.4025 MHz). Transmit power -9/0/6/14 dBm (SNR 50-72 dB): every feature
constant, one cluster. Spreading factor: the chip-rate fit split SF7/9/11
into three clusters, with IRR/eps/phase shrinking as SF rose, so the fit was
measuring its own error. Fixes:
1. `extract_lora_fingerprint_oversampled`: the fit on the 500 kHz channel IQ
   (before decimation) against an analytic continuous-phase chirp at a
   fractional symbol boundary found by a quarter-sample search (after the
   existing phase/pairing selection). EVM 19-25% -> ~10%.
2. Even then IRR, DC, eps and phase fell exactly as 1/N with SF (IRR
   -46/-57/-68 dB, DC -42/-54/-65 dBc at SF7/9/11): a once-per-symbol
   boundary transient, not hardware IQ imbalance or LO leakage (those are
   below ~-65 dB on the SX1262). The four terms are now reported per symbol
   boundary, scaled to the SF7 symbol length.

Result (t10sf rerun): **1 cluster, 92 readings across SF7/9/11 and 0/14 dBm**;
IRR -46.4/-44.7/-43.3 dB, DC -42.6/-41.8/-41.7 dBc, eps 0.008/0.010/0.010,
phase -0.28/-0.35/-0.51 deg, CFO within +/-0.07 ppm. SF11 fits are noisier
(EVM ~32%, likely phase drift over 16 ms symbols). The features are now
stable for one device across power, SF, channel and weeks; whether they
separate two devices still needs a second transmitter (TarangMini on hold,
see above).
