# T09 (part) — same-family pairwise diagnostic

10 November 2026. **Diagnostic only.** This is the separate, versioned pairwise experiment approved for Mini 3 Pro. It produces no deployable weights, read no source IQ and no locked test unit, and does not feed the main identity model, where Mini 3 Pro remains the unknown model. It is not a qualification result.

## Question

Can a spectral-only model trained on some physical aircraft tell two models apart on other physical aircraft, when both models use the same advertised link family? A size-matched cross-family pair is run under the identical protocol as the reference.

## Setup

- Data: existing reviewed 8 ms / 20 MSps / 256 × 625 log-power tensors from `data/drone_ml/2026-10-20/expanded_v2/corpus` (DRFF-R2, 5745 MHz). Only activity-eligible, non-augmented windows.
- Same-family pair: Air 2S versus Mini 3 Pro (both advertised O3; not verified against the waveform).
- Control pair: Air 2S versus Air 2 (O3 versus OcuSync 2.0), with Air 2 subsampled to the Mini 3 Pro window counts per receiver.
- Two folds, each with disjoint physical units in training and evaluation:

| Fold | Training units | Evaluation units | Training receivers |
|---|---|---|---|
| X | Air 2S 2, 3; second model one unit | Air 2S 1, 4; second model other unit | `u2` only |
| Y | Air 2S 1, 2; second model one unit | Air 2S 3, 4; second model other unit | `u1` and `u2` |

- Model: the compact `SpectrogramCNN` (179 k parameters), trained from scratch, 12 epochs, class × receiver balanced sampling, time-shift augmentation only, normalization fitted on training windows. Three seeds. No evaluation data was used for selection; the final epoch is reported.
- The fold design differs from the one first proposed: fold X was added so that one fold trains on a single receiver, which removes receiver as a possible label shortcut.

## Results

Balanced window accuracy on unseen physical units (mean of three seeds, range in brackets):

| Pair | Fold X | Fold Y |
|---|---|---|
| Same family: Air 2S vs Mini 3 Pro | 0.871 (0.839–0.898) | 0.693 (0.648–0.725) |
| Cross family: Air 2S vs Air 2 | 0.856 (0.853–0.858) | 0.719 (0.673–0.775) |

Recording-vote results, correct of total, by seed:

| Pair / fold | Air 2S | Second model |
|---|---|---|
| Same family, X | unit 1: 34–35 / 40; unit 4: 3 / 3 | Mini 3 Pro unit 2: 29–32 / 32 |
| Same family, Y | unit 3: 12 / 12; unit 4: **0 / 3** | Mini 3 Pro unit 1: 7–8 / 12 |
| Cross family, X | unit 1: 37–39 / 40; unit 4: 3 / 3 | Air 2 unit 1: 34–37 / 40–41 |
| Cross family, Y | unit 3: 12 / 12; unit 4: 3 / 3 | Air 2 unit 3: 5–8 / 12 |

Training-window accuracy was 0.91–0.94 in fold X and 0.99–1.00 in fold Y.

## What this shows

1. **The same-family pair was not harder than the cross-family control.** In both folds the two pairs land within each other's seed range. On this data there is no sign that sharing a link family removes the spectral differences between these two models.
2. **Which physical units are in training matters more than which pair is being separated.** The gap between folds (about 0.87 versus 0.70) is far larger than the gap between pairs (about 0.02). Fold Y fits its training units almost perfectly and then loses about 30 points on new units, which is the signature of learning the units, not the model.
3. **One training unit per model is not reliable.** In fold Y the second model, trained from a single unit, is recognised on its other unit only 57–69% (Mini 3 Pro) and 35–56% (Air 2) of the time. In fold X the single-unit class transfers well. The direction-dependence means the outcome depends on which unit happens to be available.
4. **Air 2S unit 4 behaves unlike Air 2S units 1–2.** It is 0 of 33 windows against Mini 3 Pro when units 1–2 are the Air 2S training set, and 33 of 33 when units 2–3 are. The 22 October model also scored 0 of 36 on this unit. It has only three files from one day, so this is a lead, not a conclusion; a different link mode, bandwidth or firmware is one possibility to check.

## What this does not show

- It is one pair, one dataset, one band, two receivers of the same publisher. Evaluation sets are small (12 Mini 3 Pro recordings in fold Y, 3 recordings for Air 2S unit 4), and windows from one recording are correlated.
- Both Mini 3 Pro units and all Air 2S units used here had been inspected in earlier work.
- The models may differ in ways other than the link waveform (for example bandwidth setting, traffic, power or the recording scene). Separability here is not proof of waveform-level model identity.
- Nothing here addresses background rejection, unknown models or X310 reception.

## Consequences for the plan

- T32 stays open: this is supporting evidence, not independent-unit qualification of a model name.
- The priority for model naming is more physical units per model in training, not a different architecture. That applies to any local recording campaign: two units of a model are the minimum to test, and three or more are needed to train and test.
- Next step for T09: audit Air 2S unit 4 against units 1–3 (occupied bandwidth, burst timing, frequency-view selection) on development recordings.

## Artifacts

- Code: `tools/drone_ml/same_family_diagnostic.py` (SHA-256 `2ed9074f42b5f9352345ecf22be378205550ba61995d98c054f9c1288e36297c`)
- Frozen split: `data/drone_ml/2026-11-10/same_family_diagnostic_01/split.json` (`8cb8592408d334d958a4b73794c699f2967e1f488823c01618439de85b4f403a`)
- Results: `data/drone_ml/2026-11-10/same_family_diagnostic_01/results.json` (`237249a60d0311f3d5ee77b7fab02cb6c5e189135e1fca979b045bddbbb3f9a5`)
- Source-file hashes for the six units: `data/drone_ml/2026-11-10/identity_subset_01/diagnostic_sha256.txt`
- Command, run outside the sandbox for GPU access:

```bash
LD_LIBRARY_PATH=$PWD/data/drone_ml/runtime-cuda-2026-10-21/driver-libs data/drone_ml/runtime-cuda-2026-10-21/bin/python -m tools.drone_ml.same_family_diagnostic --corpus data/drone_ml/2026-10-20/expanded_v2/corpus --output data/drone_ml/2026-11-10/same_family_diagnostic_01
```

Environment: PyTorch 2.14.1+cu130, RTX 3060 Ti. Runs are seeded but were not run in strict deterministic mode, so small numerical differences are possible on a repeat.

## Correction after the lag probe (same day)

Measured symbol timing shows Air 2S and Mini 3 Pro are not the same waveform: Air 2S shows 30 kHz timing in every probed file and Mini 3 Pro in one of five (`DRONE_RF_T07_CP_LAG_PROBE_2026-11-10.md`). Finding 1 above therefore does not show that same-waveform models are separable.

A pair that does share measured timing, Air 2 versus Mini 3 Pro (both 15 kHz only), was run under the same protocol (`same_family_diagnostic_02`): balanced accuracy 0.757 (0.649–0.829) in fold X and 0.875 (0.834–0.901) in fold Y. Findings 2 and 3 (dependence on training units, unreliability of one unit per model) stand, and receiver change appears as a further failure source.
