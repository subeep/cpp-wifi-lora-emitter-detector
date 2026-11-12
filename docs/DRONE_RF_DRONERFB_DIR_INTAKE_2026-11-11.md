# DroneRFb-DIR: intake and first symbol-timing check

11 November 2026. Downloaded with Sudeep's approval on 10 November. Development data only; nothing here is a qualification result.

## Source and integrity

- Dataset: DroneRFb-DIR, Zhejiang University, ScienceDB, DOI `10.57760/sciencedb.20454`, licence CC BY-NC-SA 4.0 (non-commercial; the publisher asks that the JEIT 2025 paper be cited).
- Download: ScienceDB's public endpoint, no login, no account details sent. 32 parts, 68,247,293,761 bytes; every part matches the publisher's MD5 (`data/drone_ml/2026-11-10/dronerfb_dir_download/scidb_file_list.json`, log in the dataset folder).
- Location: parts in `/media/sudeep/Storage/datasets/DroneRFb-DIR`, extracted to `extracted/twin_droneRF` (4,692 files, 68.85 GB). The archive listing had no absolute or parent paths.

## Contents, as verified

- 4,690 segments. Each is an HDF5 `.mat` with `I` and `Q` arrays of 4,000,000 float32 samples: 50 ms at 80 MSps, 2.4–2.48 GHz. About 235 s in total. In 25 random files there were 17–21 leading zero samples and no other padding; peaks were 0.03–0.18, so no clipping.
- Six drone types labelled A, C, D, E, F, G, three aircraft each. **The files do not give model names.**
- Scenes: indoor S0, S1, S2 and outdoor. Background: 62 segments.
- Publisher split: aircraft 1 and 2 of each type appear in train (2,140) and test (908); **aircraft 3 of each type appears only in test** (1,580). Background: 37 train, 25 test. `test_labels.txt` maps each numbered test file to its original name, so test labels are known.

## Symbol-timing probe, settings unchanged from 10 November

All 4,690 segments, seven blocks of 6.6 ms each, full 80 MHz width. Flag factor 4 and capture threshold 0.3 were fixed before this dataset was seen.

| | Segments | Assigned a drone-link group | Group |
|---|---:|---:|---|
| Background | 62 | 0 | — (0 of 434 blocks flagged at 15, 30, 7.5 or 32.8 kHz) |
| Type A | 916 | 160 (17%) | 15 kHz, all |
| Type C | 806 | 292 (36%) | 15 kHz, all |
| Type D | 809 | 519 (64%) | 15 kHz, all |
| Type E | 681 | 357 (52%) | 15 kHz, all |
| Type F | 675 | 286 (42%) | 15 kHz, all |
| Type G | 741 | 554 (75%) | 15 kHz, all |

Outdoor segments are found far less often than indoor ones: type D 11% outdoors against 81–88% indoors; type E 5% against 59–75%; type G 53% against 81–85%. Outdoor median level is about 6–8 dB lower.

## What this shows

1. **No false link on the new background, with frozen settings:** 0 of 62 segments. It is only about 3 s of signal, so it supports the earlier result and does not prove a rate.
2. **No wrong group:** all 2,168 detections across 18 aircraft are 15 kHz. No segment was assigned 30 kHz or 32.8 kHz.
3. **About half of drone segments are missed** (2,168 of 4,628 found), and most outdoor ones. Two likely causes, not yet separated: segments that hold only the control signal, and sensitivity. The probe was run across the full 80 MHz, where a 10–20 MHz link is diluted by the rest of the band; searching sub-bands should recover part of this. This is the first direct evidence of the weak-signal limit.
4. **All six types fall in one timing group**, the same group as Air 2, Mavic Pro, Mini 3 Pro and Mini 5 Pro. Timing alone cannot name them. This dataset is therefore a direct test of naming within a group, with a third aircraft per type that training never sees.
5. Secondary peaks differ between types and sometimes between aircraft of one type (for example 44.7 µs for D, 40.5 or 43.1 µs for E, 43.3 µs for G2 but 18.0 µs for G1). They may reflect bandwidth or link mode, so they are a lead for descriptors, not yet a model feature.

## Artifacts

- `data/drone_ml/2026-11-11/dronerfb_dir_probe_01/run.py`, `results.json` (per-segment group, flagged fractions, blind peaks).
- The probe now reads this layout (`I`/`Q` datasets) through its existing `drff_mat` reader.

## Sub-band search (added later the same day)

The probe gained an optional search over 20 MHz sub-bands (10 MHz steps) inside a wide capture; the flag factor of 4 and capture threshold of 0.3 are unchanged. Re-run on all 4,690 segments (`data/drone_ml/2026-11-11/dronerfb_dir_probe_02`):

| | Segments | Found, whole band only | Found, with sub-bands | Assigned another group |
|---|---:|---:|---:|---:|
| Background | 62 | 0 | 0 | — |
| Type A | 916 | 160 (17%) | 762 (83%) | 2 |
| Type C | 806 | 292 (36%) | 674 (84%) | 0 |
| Type D | 809 | 519 (64%) | 734 (91%) | 1 |
| Type E | 681 | 357 (52%) | 580 (85%) | 0 |
| Type F | 675 | 286 (42%) | 529 (78%) | 0 |
| Type G | 741 | 554 (75%) | 729 (98%) | 0 |
| All drone segments | 4,628 | 2,168 (47%) | 4,008 (87%) | 3 |

- Indoor segments are now found 83–100% of the time. Outdoor ones improved most but remain the weak spot: 83% (A), 48% (C), 67% (D), 57% (E), 42% (F), 98% (G).
- Background: still 0 of 62 segments; 1 of 434 blocks flagged at 15 kHz, below the capture threshold.
- Three of 4,008 detections were assigned "15 kHz and 30 kHz" instead of "15 kHz".
- So most of the earlier misses were dilution by the rest of the 80 MHz band, not control-only segments.
- This does not affect captures of 20–25 MSps, such as the app's, which are already one sub-band wide.

## Control-uplink bursts (exploratory, not a detector yet)

`tools/drone_ml/uplink_bursts.py` finds short narrow bursts and counts how many have a partner at fixed spacings.

- KU Leuven chamber recordings of a DJI Mini 2 controller alone (100 MSps): bursts about 0.5 ms long and about 1.4 MHz wide, hopping over about ±40 MHz, with start times alternating 4 ms and 6 ms apart. 19 of 20 bursts per 0.1 s recording had a partner at 4, 6 or 10 ms, against 1.4 expected by chance. The same pattern appears in the recording with the video link on.
- X310 Air 3S controller-only captures (23 October, 13 × 0.25 s): no excess at 4/6/10 ms (25 matched against 27 expected). Burst spacings instead cluster near 7.5 ms and 15 ms, which the same-day background does not show. This is 3.25 s of data and is a lead only.
- Implication: the control uplink has a frame-timing signature that appears to differ by link generation, so a detector should search for regular burst spacing in general and report the period. Not yet run on DroneRFb-DIR or on the no-drone session; no false-alarm figure exists.
