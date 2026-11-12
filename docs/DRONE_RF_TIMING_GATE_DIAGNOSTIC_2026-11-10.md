# Symbol-timing gate in front of the identity model

10 November 2026. **Diagnostic only.** It tests the two-stage idea on data already in hand: the lag probe decides whether a known drone-link timing is present in a capture and which timing group it is; the existing model may then name a model only from classes measured in that group.

## Setup

- Gate: `timing_group()` in `tools/drone_ml/cp_lag_probe.py`. A capture is assigned a group when at least 30% of its blocks are flagged: 32.8 kHz; 15 kHz and 30 kHz together; 30 kHz; 15 kHz; otherwise none.
- **The 30% threshold and the 32.8 kHz hypothesis were set after seeing data** (the X310 sessions and the Tampere blind peaks). They are not calibrated.
- Class-to-group map: from DRFF-R2 training units and Tampere only. Air 2, Mavic Pro, Mini 5 Pro: 15 kHz. Air 2S: 15 + 30 kHz. Mavic 3: both of those. Mini 4 Pro: 30 kHz. Inspire 2, Matrice 100/210, Phantom 4, Phantom 4 Pro Plus: 32.8 kHz. The other ten classes were not measured and stay eligible in every group.
- Models: the app default and the 22 October candidate, as before.

## Part A — X310: is a drone link present?

| Set | Captures | Assigned a drone-link group |
|---|---:|---|
| No-drone, 10 November | 237 | 0 |
| No-drone, 23 October | 13 | 0 |
| Controller-only, 23 October | 13 | 0 |
| Air 3S linked, 23 October (13 centres + 2 follow-up) | 15 | 3, all 30 kHz: the 2432 and 2452 MHz captures |
| `air3s_whole`, 20 and 50 MSps | 4 | 4, all 30 kHz |

All seven captures that contain the Air 3S link are assigned the 30 kHz group, and none of the 263 captures without it. For comparison, the models alone gave a drone name to 48% of the no-drone intervals.

Within the 30 kHz group the only trained class is Mini 4 Pro, so the honest output for these captures is "30 kHz four-symbol group; trained candidate Mini 4 Pro; Air 3S known from local recordings". The gate does not name the Air 3S.

## Part B — unseen DRFF aircraft: does the gate help naming?

36 recordings from calibration units (aircraft never used in training), 100 MSps, recording-level vote over each recording's eligible windows.

| | App default | 22 October candidate |
|---|---:|---:|
| Model alone, correct name | 30 of 36 | 27 of 36 |
| Gate finds a link | 23 of 36 | 23 of 36 |
| Timing group matches the true model's group | 22 of 23 | 22 of 23 |
| Model alone, correct on those 23 | 17 | 16 |
| Gated model, correct on those 23 | 17 | 18 |
| Gated model, wrong on those 23 | 6 | 5 |
| Abstained (no link found) | 13 | 13 |

By class:

| Class | Recordings | Link found | Group | Model alone correct (default / candidate) |
|---|---:|---:|---|---|
| Air 2 | 15 | 5 | 15 kHz in all 5 | 13 / 12 |
| Air 2S | 15 | 12 | 15 + 30 kHz in 11, 15 kHz in 1 | 11 / 9 |
| Mini 4 Pro | 6 | 6 | 30 kHz in all 6 | 6 / 6 |

## What this shows

1. **As a presence gate on the X310 it worked on everything available:** 0 of 263 captures without the link, 7 of 7 with it.
2. **On unseen aircraft, timing alone identifies the group better than the model identifies the name:** 22 of 23 against 16–17 of 23, on the recordings where a link is found. These three models happen to fall in three different groups, so here the group is the model.
3. **Gating barely changes the model's naming** (17 → 17, and 16 → 18). It removes physically impossible answers but the model then picks another wrong class inside the allowed set. Example: Air 2S unit 4 measures as Air 2S timing in all three recordings, yet both models vote Mini 4 Pro; with Mini 4 Pro excluded they mostly choose Mavic 3 or DJI FPV. So the unit-4 outlier is a model error, not a different link mode.
4. **The model's "Air 2" is not the Air 2 video link.** In the ten Air 2 recordings with no 15 kHz timing, both models vote Air 2 every time. In the five where the 15 kHz link is present, the default model votes Mavic 3 in two and the candidate votes Air 2S in three. The same pattern holds for the three Air 2S recordings with no link, where the default still votes Air 2S. What the model is using in those captures is not known: it could be a controller signal the probe cannot see, or the recording context. This needs review before such recordings count as identity evidence.
5. **The gate misses a third of recordings.** Timing is close to all-or-nothing per recording (flagged fractions near 0.98 or near 0), so "no link" mostly means the video link was not in that capture, not that it was weak. Weak-signal sensitivity is still untested.

**Correction (same day):** item 4 is too strong. A model trained only on link-present windows still names the link-absent Air 2 windows correctly, so they carry Air 2 content, most likely control-uplink bursts and links the probe missed. See `DRONE_RF_LINK_FILTERED_IDENTITY_2026-11-10.md`.

## Consequences

- Presence and family should come from measured timing first; this is now supported on both the X310 and unseen public aircraft.
- The identity model should be retrained with the timing result as a label-quality filter: train a model's name only on captures where its link is actually present. Today's classes mix link-present and link-absent captures under one name.
- A separate question to settle is what the link-absent captures contain (item 4), since the controller-only state must still be detected somehow.

## Limits

- Threshold and one hypothesis were set after seeing data; a sealed test is still required.
- One room and 29 minutes of no-drone data; seven link captures from one aircraft and one sitting.
- 36 DRFF recordings from six aircraft, all previously inspected development data, one publisher, one band.
- My recording-vote counts use eligible windows only; the 22 October report counted 28 of 36 for the candidate where this run gives 27.

## Artifacts

- `data/drone_ml/2026-11-10/timing_gate_01/run.py` and `results.json` (per-recording groups, flagged fractions and votes).
