# Universal drone classifier: C5 scorecard

> **Update 14 Nov (later):** the 13 Nov "no-drone" hour had a Phantom (Lightbridge) at 2.4 GHz
> from minute 18. Those captures are now excluded and the model retrained (C2d, package v2):
> never-seen-model type 0.89 (was 0.64–0.70), site background views falsely typed 0.5–0.6%
> (was 1.2–1.9%), 0 false rows, 0% unknown models named. The held-out background no longer has
> 2.4 GHz captures. The app cannot yet see the user's Phantom: its link needs a 10.9375 kHz timing
> check (changelog, same date). Numbers below are for C2c.

Date: 2026-11-14. Model: C2c (open-set two-level network, Air 3S trained, splits v2) with the
C4 v3 decision rule (per-type naming limits). Three seeds. Development only; nothing committed.
Every number is on data never used for training, epoch choice or threshold setting, except
where marked. Sources: `data/drone_ml/2026-11-14/universal_c2c/` (`c2_seed*.json`,
`c4_report.json`, `naming_tradeoff.json`), ablations in `universal_c3/`.

## 1. Section 8 gates

| Measure | Data | Proposed gate | Result (seeds 1–3) | Status |
|---|---|---|---|---|
| Type accuracy, unseen aircraft | held-out aircraft of multi-aircraft classes (634 captures) | ≥ 0.95 | 1.00 per view and per capture | **Pass** |
| Type accuracy, other receiver | own X310 recordings of our drones | ≥ 0.90 | Air 3S held-out session 7/7 views, 3/3 captures; Phantom 4 Pro 0/2 views (called not-a-drone) | **Not established** (9 views; the one non-Air 3S recording fails) |
| False named rows per hour | held-out X310 background, minutes 40–60 of 13 Nov | ≤ 1 / h | 0 rows in 0.33 h (3 of 200 captures typed, none named) | **Not established**: 0 in 20 min only bounds it below ~9 / h; ~3 h with 0 rows is needed for ≤ 1 / h |
| False typed rows per hour (classifier-only) | same | ≤ 1 / h | 0 in 0.33 h | **Not established**, as above |
| Unknown model accepted as a known model | left-out models (open_test) | ≤ 5% | 0 / 0 / 0 (Mini 3 Pro 0 of 58 captures; Taranis 0 of 70, see note) | **Pass** at the strict naming point |
| Model top-1 / top-3 in the family, unseen aircraft | multi-aircraft classes | reported; expected 0.5–0.7 top-1 | per view 0.49–0.51 / 0.97–0.98; captures named 0.5–1.6%, top-3 right when named 1.00 | reported |
| Model accuracy, same unit | single-unit classes, held-out recordings | reported as unit match | captures named 0.71–0.76; top-1 right when named 0.99 | reported |
| Cross-dataset pairs | Mini 4 Pro (DRFF-R2 → RFUAV), Mini 3 (DroneRFb → other), Air 3S | reported | type 0.995–1.00. Model per view: Mini 4 Pro top-1 0.39–0.51, top-3 1.00; Mini 3 top-1 0, top-3 0–0.31. C4 names 2–17% of cross captures, never top-1 right, always top-3 right | reported: **model names do not carry across datasets** |

Note on Taranis: the per-type limits were designed after seeing Taranis named by v2, so its 0
is not a clean test. Mini 3 Pro (DJI) is clean. A second never-seen controller is needed.

## 2. The naming trade-off (decision needed)

The naming rule cannot both name other units of a known model and refuse unknown models. The
DJI score threshold sets the balance (distance limit fixed; seeds 1 / 2 / 3):

| DJI score threshold | Unseen aircraft of known models named | right model in top-3 when named | Mini 3 Pro (never seen) named | Known units named / top-1 right |
|---|---|---|---|---|
| 0 (always name) | 100% | 0.96–0.98 | 55–58 of 58 | 0.94 / 0.93–0.95 |
| 0.3 | 72–76% | 1.00 | 25–32 of 58 | 0.84–0.87 / 0.98 |
| 0.4 | 27–46% | 1.00 | 4–14 of 58 | 0.79–0.84 / 0.99 |
| 0.5 | 3–8% | 1.00 | 0 of 58 | 0.76–0.80 / 0.99 |
| C4 v3 (0.57–0.64, strict) | 0.5–1.6% | 1.00 | 0 of 58 | 0.71–0.76 / 0.99 |

What this means in the field: a DJI drone whose model is in training but whose unit is not
(the usual case) gets "DJI OFDM, model unknown" at the strict point, and a Likely top-3 list
that is almost always right at 0.4, at the cost of naming roughly 1 in 4–14 never-seen DJI
models wrongly. "Unseen aircraft" here are other units recorded on the same dataset's receiver,
so the field rate will be lower still until more receivers are in training (E4, DroneRFa).

## 3. What C3 showed (kept parts)

Measured facts as network input and presence filtering earn their place (type across datasets,
naming, refusal); background mixing helps weak signals and naming at about 0.5 points more
falsely typed background views. All kept. Unseen-model *type* stays 0.64–0.76 in every variant.

## 4. Open items before the gate can be fully passed

1. About 3 hours of X310 no-drone background (rotating band slots, same antenna) to establish
   false rows per hour.
2. More own-X310 recordings of drones other than Air 3S (other-receiver type accuracy).
3. A second never-seen hobby controller, for the unknown-model check on hopping controllers.
4. DroneRFa (E4) for more models and receivers; about 26% downloaded.

## 5. Decisions for the user

1. The gate values (section 1, proposed column).
2. The naming point (section 2): strict, or Likely top-3 at a lower threshold.
3. Go, conditional go, or no-go for Stage D integration.
