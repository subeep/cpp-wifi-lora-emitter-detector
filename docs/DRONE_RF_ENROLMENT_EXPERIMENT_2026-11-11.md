# Naming a drone model from one enrolled aircraft: first experiment

11 November 2026. **Diagnostic on development data; a negative result for the setup tested.** No deployable weights.

## Question

Can a drone model be named on an aircraft never seen before, when only one aircraft of that model was available, including when the network was never trained on that model at all?

## Setup

- Data: DroneRFb-DIR, six types (A, C, D, E, F, G), three aircraft each, all in the 15 kHz symbol-timing group, so timing cannot separate them. Three 8 ms windows per 50 ms segment in the standard 20 MSps automatic view; only windows whose view shows the 15 kHz link (5,853 windows; 120–499 per aircraft). Stored in `/media/sudeep/Storage/datasets/_derived/dronerfb_dir_views_01`.
- Test set: aircraft 3 of every type (2,176 windows), used for nothing else.
- Network: the compact `SpectrogramCNN`, 12 epochs, type × aircraft balanced sampling, at most 400 windows per aircraft, time-shift augmentation only, two seeds. No tuning on the test aircraft.
- Chance level for six types is 0.167.

## Results on the unseen third aircraft

Balanced accuracy over the six types (two seeds):

| Condition | Windows | Segment vote |
|---|---|---|
| Classifier trained on aircraft 1 and 2 of all six types | 0.532, 0.587 | 0.534, 0.590 |
| Classifier trained on aircraft 1 only | 0.401, 0.374 | 0.395, 0.365 |
| Same two-aircraft network, naming by nearest stored reference from aircraft 1 | 0.392, 0.461 | — |
| Enrolment: network never saw the type; all six enrolled from aircraft 1 | mean 0.424 over twelve runs | — |

Recall of the type the network had never seen, enrolled from one aircraft:

| Held-out type | Windows correct (two seeds) | Mostly mistaken for |
|---|---|---|
| A | 104, 100 of 327 | F, G |
| C | 198, 192 of 255 | E |
| D | 84, 82 of 449 | G |
| E | 75, 78 of 448 | C (365–373 windows) |
| F | 15, 8 of 236 | A, G, D |
| G | 144, 196 of 461 | F, A |

Mean held-out recall is 0.31 (range 0.03 to 0.78). Enrolling the held-out type from two aircraft gives 0.33.

Per type, the two-aircraft classifier ranges from about 0.31 (G) to 0.86 (F); the one-aircraft classifier from about 0.15 (F) to 0.83 (D).

## What this shows

1. **One aircraft does teach something, but not much with this input:** 0.37–0.40 against a chance of 0.17. A second aircraft raises it to 0.53–0.59. Each extra aircraft is worth a lot.
2. **Enrolment of a never-trained model did not work here.** A network trained on five other types does not place a sixth type's aircraft near each other: mean recall 0.31, and type F is almost never recognised. The approach I proposed is not supported by this first test.
3. **Even two aircraft per type give only about 0.56 inside one timing group.** With every model on the same link structure, an 8 ms spectrogram does not carry enough to name them reliably on a new aircraft. This is the hard case identified at the start, now measured on six models.
4. **Errors are structured, not random.** C and E are confused with each other, and D, G, F and A form another cluster. Those pairs may share a radio or link mode; the dataset gives only letters, so this cannot be checked yet.

## Limits

One small network, twelve epochs, two seeds, no receiver or channel augmentation, and no metric-learning loss: this is a first setup, not the best achievable. The model names are unknown. Windows from one segment are correlated, though segment votes agree with window results. Link presence comes from the probe, which misses about half the segments.

## What to try next

- Physical descriptors per segment in place of raw 8 ms spectrograms: the lag profile's secondary peaks differed between types in the intake check, as did bandwidth and burst timing over the full 50 ms.
- Longer observation: 50 ms segments and several segments per encounter, since frame and hop structure is not visible in 8 ms.
- The dataset's paper, to learn the six model names and whether the confused pairs share a link.

## Artifacts

- `tools/drone_ml/enrolment_experiment.py`; `data/drone_ml/2026-11-11/enrolment_01/build_views.py`, `run_01/results.json`.
