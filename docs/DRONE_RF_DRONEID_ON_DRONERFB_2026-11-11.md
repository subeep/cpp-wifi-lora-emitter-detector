# DroneID decoding on DroneRFb-DIR

11 November 2026. **Diagnostic.** The repository's native DJI DroneID decoder (`build/drone_native_worker`, unchanged) was run over all 4,690 DroneRFb-DIR segments to see whether decoded identity can supply the model name that RF naming could not.

## Method

- Decoder check first: its 5 tests pass, including a recorded Mavic Air 2 packet with serial.
- Two passes over every 50 ms segment:
  - **Channelised:** mixed to 2414.5, 2429.5, 2444.5, 2459.5 and 2474.5 MHz and decimated to 20 MSps, assuming the capture centre is 2440 MHz (inferred from the stated 2.40–2.48 GHz range).
  - **Wide:** the full 80 MSps segment, with the decoder choosing the strongest 10 MHz.
- Only packets passing the outer CRC are counted. From each payload only the version byte, the product-type byte and the serial were kept; positions were not stored.

## Results

| | Channelised | Wide |
|---|---:|---:|
| Segments with a CRC-valid packet | 67 of 4,690 | 21 of 4,690 |
| Decoder errors | 0 | 0 |

Packets were found at 2414.5, 2429.5, 2444.5 and 2459.5 MHz, which supports the assumed centre.

By type (channelised pass):

| Type | Packets | Format | Product-type code | Serial prefix | Aircraft with packets |
|---|---:|---|---|---|---|
| G | 7 | version 2, parsed | 66, listed as Air 2S in the repository's reference table | `3YTB` | G1, G2, G3: three different serials |
| C | 12 parsed + 4 auxiliary | version 2, parsed | 88, not in the table | `6C5C` | C1, C2, C3: three different serials |
| E | 7 parsed + 3 auxiliary | version 2, parsed | 87, not in the table | `687X` | E1, E2, E3: three different serials |
| A | 22 + 2 auxiliary | version 3, not parsed | — | — | A1, A2, A3 |
| D | 5 | one version 3, three unrecognised, one auxiliary | — | — | D2, D3 |
| F | 5 | one version 3, four unrecognised | — | — | F1, F2 |

## What this shows

1. **Where DroneID parses, it gives the model consistently.** For types C, E and G, every parsed packet from all three aircraft carries the same product code, and each aircraft has its own serial. That is 26 of 26 consistent, against about one in two for RF naming on the same data. Type G reports the code the reference table lists as Air 2S.
2. **The decoder works on independent data it was not built on**, across 14 of the 18 aircraft.
3. **Three of six types cannot be named this way yet.** Type A sends version 3 payloads, which pass the CRC but are not parsed; D and F mostly produce packets in an unrecognised envelope. These are probably newer link generations.
4. **The product-code table is nearly empty in the C++ decoder:** it maps only code 63 (Mini 2). The Python reference table stops at 70, so codes 87 and 88 are unnamed. I could not confirm what they are from DJI's public documentation.
5. **Tuning to the DroneID channel matters:** 67 packets channelised against 21 wide.
6. **Yield is low here, as expected:** a burst comes roughly every 0.6 s and each segment is 50 ms, so only a small share of segments can contain one. This says little about live yield with a one-second dwell.

## Consequences

- For the 15 kHz group, decoded DroneID should be the primary source of the model name; RF naming is a fallback candidate list.
- Work needed: a full product-code table in the C++ decoder, version 3 handling, and the unrecognised envelope seen for D and F.
- The serial separates aircraft of one type, which gives a ground-truth check for any future multi-aircraft recording.

## Limits

Codes 87 and 88 are unidentified. The mapping of 66 to Air 2S comes from the repository's reference table, not from the dataset publisher. DroneID is self-reported and not authenticated. Types A, D and F remain unnamed.

## Artifacts

- `data/drone_ml/2026-11-11/droneid_decode_01/run.py`, `results_chan.json`, `results_wide.json`, `results_packets_detail.json`.

## Decoder extension (later the same day)

The vendored decoder under `third_party/punjdrone_linux` is pinned by a hash manifest and was not edited. The extension lives in `src/drones/droneid_packet.{hpp,cpp}`, `tools/drone_native_worker.cpp` and `src/drones/detector.{hpp,cpp}`.

What changed:

- **Readability is decided from the record itself:** message family 0x10, version 2 or 3, and a passing record checksum, at the start of the payload or after up to 8 bytes of padding. The serial (offset 7, 16 bytes) and product-type byte (offset 67) are then read. Version 3 records are 112 bytes and plain text at the same offsets as version 2; the "unrecognised" packets from types D and F were the same records behind a few bytes of padding.
- **Product table:** 49 codes from the repository's Python reference list. The recorded fixtures confirm 58 (Mavic Air 2) and 63 (Mini 2). Unlisted codes are reported as "unknown code" with the number.
- **Channel tuning in the worker:** each documented 2.4 GHz DroneID channel inside the capture is tried, then the decoder's automatic choice as a fallback (needed for recordings whose centre metadata is inexact, and for 5 GHz, where the channel list is not confirmed here).
- **Input queue:** the native queue held one chunk, so only the first 160 ms of a visit was decoded. It now holds up to 8 chunks or 24 M samples, refuses the rest, and counts refusals by cause. A test that delivers a four-chunk visit times out with the old queue and passes with the new one.
- **Packet contract** (`droneid-packet-v1`): `payload_version`, `identity_readable`, `product_code`, `model`, `model_source`, `payload_shift`, `channel_hz`, with a typed reader `parse_native_packet()`.

Measurements with the extended worker:

| Measurement | Result |
|---|---|
| No-drone X310 session, 237 one-second segments | 0 checksum-valid packets, 0 errors |
| DroneRFb-DIR segments known to hold a packet (67) | 66 found, all on a documented channel |
| DroneRFb-DIR random sample (600) | 5 with a packet |
| Packets readable, of 71 found | 59; the rest are 10 auxiliary messages and 2 unrecognised |
| Worker time per second of IQ at 20 MSps, four in parallel | about 4.5 s with one pass, 7.7 s with two |

Product codes per type, each consistent across every aircraft that produced a readable packet:

| Type | Version | Product code | Serial prefix | Aircraft | Name in table |
|---|---|---|---|---|---|
| A | 3 | 84 | `F67Q` | A1, A2, A3 | none |
| C | 2 | 88 | `6C5C` | C1, C2, C3 | none |
| D | 3 | 93 | `F6Z9` | D2, D3 | none |
| E | 2 | 87 | `687X` | E1, E2, E3 | none |
| F | 3 | 90 | `F6N8` | F1, F2 | none |
| G | 2 | 66 | `3YTB` | G1, G2, G3 | Air 2S |

Open items: names for codes 84, 87, 88, 90 and 93; the 5 GHz DroneID channel list; decode speed, which is several times slower than real time and matters for how long decoded evidence must stay current in the app.

## Known-model check on DRFF-R2 (12 November)

Purpose: attach model names to product codes using recordings whose model is known. Script and results: `data/drone_ml/2026-11-11/droneid_decode_03/` (`drff.py`, `drff_codes.json`, `pilot.json`).

Scope: 84 hover files (1.4 s each, 5745 MHz, 100 MSps) from admitted aircraft only: Air 2 units 1–4, Air 2S 1–4, Mini 4 Pro 1–4, Mini 3 Pro 1–2, Mavic 3 unit 1, Mini 5 Pro unit 1. Locked test aircraft (Air 2 5–8, Air 2S 5–7, Mini 4 Pro 5, mavic3C, mavic3S) were not opened. Each file was tuned to 5756.5 and 5776.5 MHz. Kept per packet: format, version, product code, four-character serial prefix, channel; no positions.

| Model (publisher label) | Aircraft with packets | Format | Product code | Serial prefix | Readable |
|---|---|---|---|---|---|
| Air 2 | 1, 2, 3 | version 2 | 58 | `3N3B`, `1WNB` | yes (6 packets) |
| Air 2S | 1, 2 | version 2 | 66 | `3YTB` | yes (7) |
| Mini 3 Pro | 1, 2 | version 3 | 73 | `F4XF` | yes (11) |
| Mavic 3 | 1 | INFP wrapper | none | none | **no, encrypted** (4) |
| Mini 4 Pro | 2, 3, 4 | INFP / CRYP wrapper | none | none | **no, encrypted** (7) |
| Mini 5 Pro | 1 | INFP wrapper | none | none | **no, encrypted** (1) |

Findings:

1. **58 = Air 2 and 66 = Air 2S are confirmed on several aircraft; 73 = Mini 3 Pro is new** and now in the product table, with this run as its source.
2. **Mavic 3, Mini 4 Pro and Mini 5 Pro send an encrypted DroneID in these recordings.** The packet passes its outer and inner checksums and starts `INFP` (one `CRYP`), but everything after the header differs completely between two packets from the same aircraft. No serial or product code can be read. The four bytes after `INFP` differ between aircraft of one model and changed on the one Mavic 3, so they are not a model tag either. Such packets are now reported with `identity_encrypted: true`.
3. **Encryption depends on more than the model.** DroneRFb-DIR type D sends plain version 3 records with code 93 and serial prefix `F6Z9`, a prefix that two web pages give for the Mini 4 Pro, while the DRFF-R2 Mini 4 Pro aircraft send encrypted packets. Firmware or sales region is the likely difference; not verified.
4. **5.8 GHz DroneID channels:** packets were decoded at 5756.5 MHz (32) and 5776.5 MHz (14). Both are now in `channels_58`, and the worker tunes to them itself (checked on an untuned 100 MSps file). The recordings stop at 5795 MHz, so higher channels are unknown.
5. **Yield per file is low:** 25 of 84 files (1.4 s each) gave any packet, so a single short visit will often see none.
6. **Codes 84, 87, 88, 90 and 93 are still unnamed.** No published table covering them was found. Web leads from serial prefixes only (not in the table): `F6Z9` Mini 4 Pro (93), `F6N8` Air 3 or Air 3S (90), `F67Q` Mavic 3 Pro or Classic (84). If 93 is the Mini 4 Pro, the type D link shows 15 kHz timing in DroneRFb-DIR but 30 kHz in DRFF-R2, so timing group would depend on link mode, not only on model.
7. **Air 3S:** the existing recordings (13 s at 2444 MHz, plus 28 quarter-second captures) gave no DroneID packet at all, readable or encrypted. Whether the Air 3S DroneID is encrypted is therefore unknown.

Consequence for naming: decoded DroneID names older aircraft and some newer ones; for aircraft that encrypt it, the app can say "DJI DroneID present, identity encrypted" plus the link group, and nothing more specific.

## Decode time on live captures (12 November)

The worker accepts `--listed-channels-only`: when a listed DroneID channel lies inside the capture it decodes only those channels and skips the automatic-channel pass. The app passes it for the live native detector (`native.start(true, true)` in `src/drones/addon.hpp`); offline replay keeps the automatic pass because recorded centre metadata can be inexact. When no listed channel is in view the automatic pass still runs.

Measured at 20 MSps (`data/drone_ml/2026-11-11/droneid_decode_03/speed.py`, `listed_only.json`):

| Input | Default | Listed channels only |
|---|---|---|
| 71 DroneRFb-DIR segments holding packets, tuned 2 MHz off the channel | 71 packets | 71 packets (none lost, none gained) |
| 12 s of no-drone X310 captures near a listed channel | 83.0 s, 0 packets | 43.4 s, 0 packets |

Decode time falls from about 6.9 s to 3.6 s per second of IQ, with one worker and nothing else running. That is still slower than real time: a 0.5 s visit takes about 1.8 s, so the evidence store must not expire a capture's DroneID result before roughly 2–3 s after acquisition plus queueing.

## RFUAV recordings through the link detector and decoder (2026-11-12)

RFUAV was already on disk but had not been decoded. Three 1 s files per video-bandwidth
setting were extracted for six archives (41 files, complex float32, 100 MSps, USRP X310) and
run through the timing probe and the DroneID worker. Script and results:
`data/drone_ml/2026-11-12/rfuav_decode_01/`.

| Recording | Centre | Timing group (files) | DroneID |
|---|---|---|---|
| DJI Avata 2 | 5760/5765 MHz | 30 kHz in 8 of 11, same 142.7/33.3 µs peaks as Mini 4 Pro and Air 3S | 3 packets, all encrypted (INFP), one on 5776.5 MHz |
| DJI FPV Combo | 5760 MHz | 15 kHz in all 9; 30 kHz as well in 5 of 6 files at 20/40 MHz video | none in 9 s |
| DJI Mavic 3 Pro | 5800 MHz | 15 kHz in 5 of 6, weak (0.33–0.65 of blocks) | none in 6 s |
| DJI Mini 3 | 2470 MHz | 15 kHz in all 6 | 6 packets, version 2, readable, product code 87, one serial |
| DJI Mini 4 Pro | 2450 MHz | 30 kHz in all 6 | 2 encrypted (INFP) and 1 auxiliary packet |
| Autel Evo Nano | 5770 MHz | none | none (not DJI) |

Consequences applied in code: product code 87 is named "Mini 3" (it was one of the unnamed
codes in DroneRFb-DIR, type E); the model table gains measured rows for Mini 3, Avata 2,
DJI FPV and Mavic 3 Pro. Limits: one aircraft per model, 3 s to 11 s each, high-SNR
recordings; "no DroneID in N seconds" is not evidence that a model sends none.

## KU Leuven recordings through the link detector and decoder (2026-11-12)

41 members (100 MSps, centre 2.44 GHz, semi-anechoic chamber) sampled across the video and
remote-controller archives. Script and results: `data/drone_ml/2026-11-12/kuleuven_decode_01/`.

| Recording | Timing group (files) | DroneID |
|---|---|---|
| Mini 2, video | 15 kHz in 6 of 6 | none in 1.2 s |
| Mini 2, controller | none in 6 of 6 (15 kHz flagged in 0.11–0.16 of blocks) | 1 packet, code 63 "Mini 2", 2459.5 MHz |
| "Matrice" (Matrice 300), video | 15 kHz in 6 of 6 | none in 0.6 s |
| "Matrice", controller | 15 kHz in 6 of 6, weak (0.32–0.42) | 1 packet, code 60 "M300 RTK" |
| Inspire 2, video | 32.8 kHz in 6 of 6 | none |
| Inspire 2, controller | 32.8 kHz in 6 of 6 (0.47–0.63) | none |
| "Mavic", video | 15 kHz in 3 of 3 | 1 packet, code 69 "Mavic 2 Enterprise Advanced" |
| "Mavic", controller | none in 2 of 2 | 2 packets, code 69; 1 auxiliary packet |

What this adds: the dataset's "Mavic" is a Mavic 2 Enterprise Advanced by its own DroneID; the
model table rows for Mini 2, M300 RTK and Mavic 2 Enterprise Advanced are now measured (15 kHz),
and Inspire 2 at 32.8 kHz is confirmed on a second, independent dataset. Product codes 60, 63
and 69 agree with the table. The "controller" recordings contain DroneID packets, so the
aircraft was on in at least some of them; they are not controller-only evidence. Limits: one
aircraft per model, files of 0.1 s to 1 s.
