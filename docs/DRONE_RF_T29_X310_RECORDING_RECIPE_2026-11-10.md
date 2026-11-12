# T29 / T30 — X310 controlled recording recipe

10 November 2026. Prepared by the coding agent; **Sudeep operates the radio, controls the scene and approves labels.** The pilot and one 30-minute session were recorded on 10 November; the timing and storage figures below are from those two runs.

## What is used

- Recorder: the existing standalone tool `~/Documents/air3s_iq_recording/air3s` (`check`, `record`). It wraps UHD `rx_samples_to_file`, is receive-only, and writes sc16 segments with a JSON sidecar (centre, rate, gain, host start time, overflow marks, SHA-256). It is used unchanged.
- Validator: `tools/drone_ml/x310_session.py` in this repository (`template`, `check`, `collection`), with tests in `tests/test_drone_ml_x310_session.py` (7 passing).

Known limits of the recorder, accepted for this purpose:

| Limit | Effect |
|---|---|
| No device timestamps, host start time only | Segment timing is approximate; segments are never joined. |
| Session file says `target: DJI Air 3S` for every session | Ignored. The scene manifest is the only source of scene truth. |
| Writes under `~/Documents/air3s_iq_recording/sessions` on the system disk (54 GB free) | See setup step 2. |
| One `rx_samples_to_file` start per segment | Gaps of a few seconds between segments; this is not the scanner's exact visit timing. |

## Receiver settings (same for every session, negative and positive)

| Setting | Value | Reason |
|---|---|---|
| Sample rate | 20 MSps | Matches the coordinated visits and the model's 20 MSps view; inside the measured 25 MSps link limit |
| Gain | 20 dB | The app's profile |
| Antenna port | RX2 | The app's profile |
| Antenna and position | Fixed and written in the manifest | A change of antenna or position is a new collection group |

Using identical settings for background and drone sessions is required. Otherwise the model can learn the receiver setting in place of the signal, and the validator's `collection` check fails.

## Proposed centres (confirm or change under decision D5)

`2401 2413.5 2438.5 2463.5 5181.5 5221.5 5746.5 5786.5 5826.5` MHz. These are the default schedule's DroneID visit and Wi-Fi channels 1, 6, 11, 36, 44, 149, 157, 165, each with the app's 1.5 MHz capture offset.

## Setup, once

1. Close or disconnect `rf_monitor_gui`; the X310 has one owner. Then:

```bash
cd ~/Documents/air3s_iq_recording && ./air3s check
```

2. Put recordings on the large disk. The recorder has no output option, so point its `sessions` folder at the Storage drive (this creates one folder and one link; it changes no code):

```bash
mkdir -p /media/sudeep/Storage/x310_sessions && ln -s /media/sudeep/Storage/x310_sessions ~/Documents/air3s_iq_recording/sessions
```

3. Pilot, about two minutes. It measures the real cycle time, checks that the Storage drive keeps up at 80 MB/s without overflow, and gives the first revisit-interval figure:

```bash
cd ~/Documents/air3s_iq_recording && ./air3s record --rate 20e6 --centres-mhz 2401 2413.5 2438.5 2463.5 5181.5 5221.5 5746.5 5786.5 5826.5 --seconds 1 --repeat 2 --label pilot --note "pilot, timing and disk check"
```

   If any segment reports overflow, stop and tell me; the fallback is recording to the system disk in short blocks and moving them.

## Two recording shapes

| Shape | Command pattern | Use | Size |
|---|---|---|---|
| Scan-style | nine centres, `--seconds 1`, `--repeat N` | Background hours and paired positives; approximates the deployed revisit pattern | about 690 MB per cycle; 39 GB per wall-clock hour (measured: 63 s per cycle) |
| Continuous | one or two centres, `--seconds 30`, `--repeat 2` | Temporal-context experiments; link structure | 2.4 GB per 30 s segment |

Scan-style, 29 minutes (28 cycles, measured):

```bash
cd ~/Documents/air3s_iq_recording && ./air3s record --rate 20e6 --centres-mhz 2401 2413.5 2438.5 2463.5 5181.5 5221.5 5746.5 5786.5 5826.5 --seconds 1 --repeat 28 --keep-going --label bg-quiet-A --note "scene notes here"
```

## Scenes

Each row is one session. Run the background scenes and the paired positive scenes in the same sitting, same antenna position.

| Scene type | What to set up | Label it can become |
|---|---|---|
| `quiet` | All own drone systems off, batteries out. Wi-Fi as it normally is, nothing deliberately loaded. | Scoped negative |
| `router_idle` | A known access point on, no deliberate traffic | Scoped negative |
| `router_load` | Sustained download or upload through that access point | Scoped negative |
| `phone_hotspot_upload` | Phone hotspot on, a device uploading through it | Scoped negative |
| `camera_stream` | Wi-Fi camera or video stream running | Scoped negative |
| `bluetooth` | Bluetooth audio or file transfer nearby | Scoped negative |
| `mixed_non_drone` | Two or more of the above together | Scoped negative |
| `controller_only` | Controller on, every aircraft off | Controller-only activity (positive, role kept) |
| `linked_ground` | Aircraft and controller linked, props off, on the ground | Drone-system activity |
| `linked_flight` | Authorised flight only | Drone-system activity |
| `mixed_with_drone` | Linked drone plus loaded Wi-Fi | Drone-system activity; evaluation only |

A scene is a negative only within the controlled scene, the recorded band and the recorded time. "Aircraft off" or "no DroneID decoded" is not enough on its own: the manifest must say how the scene was controlled (for example indoors, batteries removed, no other operators nearby). Anything ambiguous is left unapproved and stays unlabelled stress data.

## After each session

1. Create the scene manifest, then fill in every blank (scene type, environment, day, group, partition, own drone systems and their states, nearby equipment, at least one timestamped note, and your approval):

```bash
cd ~/Documents/newrocktest-cpp && data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.x310_session template /media/sudeep/Storage/x310_sessions/SESSION_DIR
```

2. Validate. It checks sizes, sidecars, overflow marks, clipping and empty data, and the manifest. Faulty segments are excluded, not repaired, and a session fails if more than 10% are faulty. Add `--verify-hashes` to re-read and re-hash every segment:

```bash
cd ~/Documents/newrocktest-cpp && data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.x310_session check /media/sudeep/Storage/x310_sessions/SESSION_DIR
```

3. After several sessions, check the whole collection (split isolation, exposure hours per partition, paired receiver settings):

```bash
cd ~/Documents/newrocktest-cpp && data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.x310_session collection /media/sudeep/Storage/x310_sessions
```

The validator never assigns a label. Its strongest output is "candidate", and only when your approval is in the manifest.

## Partitions and independence

- Decide the partition **before** recording, and write it in the manifest: `train`, `calibration`, `development` or `final_sealed`.
- Sessions that share a day, an environment or a collection group are not independent. The collection check rejects a day, environment or group that appears in more than one partition.
- Minimum to start: two independent groups per label in each of train, calibration and development (the existing trainer's rule). That means at least three recording days or places for those, plus separate ones for the sealed final set.
- `final_sealed` sessions (T30): record, write the manifest, validate, and do not open the IQ. The validator checks them from sidecar metadata only. Do not look at their spectra or run any model on them until the frozen evaluation.
- How many hours: this follows from decision D1 in the T05 sheet. A short zero-alarm recording cannot support a low hourly false-event rate.

## What I need back from you

| Item | When |
|---|---|
| Pilot result (cycle time, any overflow) | Before the first real session |
| Confirmation or change of the nine centres | Before the first real session |
| Antenna description and position | In each manifest |
| Completed, approved manifests | After each session |

If the X310 or an operator is unavailable, T29 is blocked and recorded as such; the classifier and offline evidence work continue.

## Measured on 10 November 2026

| Run | Segments clean | Cycle time | Size | Notes |
|---|---|---|---|---|
| Pilot, 2 cycles | 17 of 18 | 63 s | 1.4 GB | One short segment |
| `bg-ambient-A`, 28 cycles | 237 of 252 | 62 s | 19 GB | 15 short segments (6%), spread over the session and all centres |

- A short segment means samples were dropped while writing to the Storage drive at 80 MB/s. The recorder reports zero overflow marks for these (its mark detection misses the case) but flags them `short`; the validator excludes them.
- Median level at 20 dB gain: about −14 to −34 dBFS on the 2.4 GHz centres and 5181.5 MHz, and about −72 dBFS (noise floor) on 5221.5 and the 5.8 GHz centres. No meaningful clipping.
- Revisit interval of this recording method is about 62 s per centre. It is set by the recorder restarting the radio for each segment, not by the app's scanner, so it is not the deployed revisit interval.
