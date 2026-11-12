# Existing identity models on X310 no-drone data

10 November 2026. **Diagnostic only.** The existing models have no background class and no acceptance policy; in the app they report "identification unavailable" with an uncalibrated candidate. This measures how those candidates behave on real no-drone captures from the deployment receiver. The no-drone session is training-partition data whose manifest still awaits antenna position and approval.

## Setup

- Models: the app default (`data/drone_ml/2026-10-14/expanded_training_01/residual_model/model.pt`) and the 22 October candidate (`data/drone_ml/2026-10-20/expanded_v2/residual_model/model.pt`). Both have 21 known classes.
- Path: the live worker's own entry point (`predict_with_triage`), three 8 ms intervals per capture at the start, middle and end, as the app selects them. 20 MSps.
- Data: the 237 usable one-second captures of `bg-ambient-A` (10 November); the 23 October Air 3S sessions; the two 20 MSps `air3s_whole` files.

## No-drone session, 711 intervals

| | App default | 22 October candidate |
|---|---:|---:|
| Intervals given a drone model name | 343 (48%) | 343 (48%) |
| of which score ≥ 0.95 | 26 | 38 |
| of which score ≥ 0.99 | 0 | 0 |
| Captures with at least one score ≥ 0.95 | 24 of 237 | 33 of 237 |
| Rejected by the activity gate | 368 | 368 |

By centre:

| Centre (MHz) | Intervals | Named | ≥ 0.95 (default / candidate) | Most frequent names |
|---:|---:|---:|---|---|
| 2401.0 | 75 | 75 | 12 / 18 | Air 2, Air 2S |
| 2413.5 | 81 | 81 | 3 / 6 | Mini 5 Pro, Air 2S |
| 2438.5 | 78 | 73 | 5 / 2 | Mini 5 Pro, Air 2S / DJI FPV |
| 2463.5 | 84 | 80 | 4 / 3 | Mini 5 Pro, Mini 4 Pro / DJI FPV |
| 5181.5 | 84 | 20 | 1 / 2 | Mini 5 Pro / Yuneec Typhoon H |
| 5221.5 | 72 | 7 | 1 / 0 | Mini 4 Pro / Parrot Disco |
| 5746.5 | 81 | 0 | 0 / 0 | — |
| 5786.5 | 75 | 6 | 0 / 6 | Air 2 / Air 2S |
| 5826.5 | 81 | 1 | 0 / 1 | Air 2S |

## Air 3S link present, 15 intervals

Captures where the lag probe found the 30 kHz link (23 October at 2432 and 2452 MHz, and `air3s_whole` at 2444 MHz). Air 3S is not one of the 21 classes.

| | App default | 22 October candidate |
|---|---|---|
| Named Mini 4 Pro | 8 of 15, scores 0.91–0.98 | 8 of 15, scores 0.97–0.98 |
| Other names | Mini 5 Pro 4, Air 2S 3 | Mini 5 Pro 4, Inspire 2 2, DJI FPV 1 |

## What this shows

1. **On busy 2.4 GHz, ordinary Wi-Fi and Bluetooth get a drone name almost every time** (309 of 318 intervals). The activity gate removes quiet spectrum, not non-drone signals. This is the expected behaviour of a closed-set classifier and confirms it cannot serve as a presence detector.
2. **The score does not separate drone from no-drone.** Link-present intervals top out at 0.91–0.98, and 26–38 no-drone intervals reach 0.95 or more. If 0.95 were used as acceptance, this 29-minute session would have produced 24–33 false named captures. No threshold between 0.95 and 0.99 keeps the link and drops the background.
3. **When the Air 3S link is present, the most frequent name is Mini 4 Pro**, the model that shares its measured symbol timing. So the classifier is picking up the link family across receivers, but only in about half the intervals.
4. **The symbol-timing probe is far more selective on the same no-drone data:** 30 kHz was flagged in 0.11% of blocks, against a drone name in 48% of intervals here.
5. The 22 October candidate named six noise-floor intervals at 5786.5 MHz as Air 2S with scores of 0.95 or more, which the app default did not.

## Limits

- One no-drone session, one room, 29 minutes; the three 5.8 GHz centres were at the noise floor throughout.
- Fifteen link-present intervals from one aircraft and one sitting.
- Scores are uncalibrated softmax values. The diagnostic rejection policy from 22 October (temperature, score and prototype distance) was not applied, because it is not stored in either checkpoint.

## Artifacts

- `data/drone_ml/2026-11-10/model_on_x310_01/run.py` and `results.json` (one row per interval and model).
