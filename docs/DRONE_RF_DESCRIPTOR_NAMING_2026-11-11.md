# Naming drone types from physical descriptors

11 November 2026. **Diagnostic on development data.** It tests whether measured link properties name the six DroneRFb-DIR types on an unseen aircraft better than the spectrogram network did.

## Setup

- One feature vector per 50 ms segment, from the full 80 MHz: the symbol-timing lag profile from 5 to 150 µs (290 values), the spectrum shape made independent of where the link sits in the band (130 values), and burst and frame timing from the power envelope (64 values).
- A plain softmax regression and a one-hidden-layer network, trained on aircraft 1 and 2 (or 1 only), tested on aircraft 3. Three seeds. Chance is 0.167.
- Segment-level results, so they compare with the spectrogram network's segment votes: 0.53–0.59 with two aircraft, 0.37–0.40 with one.

## Results on the unseen third aircraft (balanced accuracy, mean of three seeds)

| Features | Two aircraft, softmax | Two aircraft, small network | One aircraft, softmax | One aircraft, small network |
|---|---|---|---|---|
| Lag profile | 0.39 | 0.41 | 0.39 | 0.35 |
| Spectrum shape | 0.48 | 0.58 | 0.33 | 0.35 |
| Envelope timing | 0.40 | 0.60 | 0.26 | 0.31 |
| All together | 0.46 | 0.50 | 0.38 | 0.38 |

On the segments where the probe found the link, the best figure is 0.66 (envelope timing, two aircraft).

## Transfer check

The same features and small network, changing only which aircraft are on each side:

| Split | Envelope | Spectrum | All |
|---|---|---|---|
| Same aircraft in training and test (random 70/30 of segments) | 0.75 | 0.81 | 0.93 |
| Aircraft 1 held out | 0.42 | 0.44 | 0.47 |
| Aircraft 2 held out | 0.59 | 0.58 | 0.44 |
| Aircraft 3 held out | 0.60 | 0.58 | 0.50 |

## What this shows

1. **Descriptors do no better than the spectrogram network.** Two aircraft give about 0.5–0.6 and one aircraft about 0.35–0.4, by either method. Three different approaches now land in the same place, so the limit is not the choice of representation.
2. **What separates the recordings is tied to the aircraft, not the model.** With the same aircraft on both sides the features reach 0.93; with a new aircraft they fall to about 0.45–0.5. The measurable differences are mostly properties of each aircraft or its recording session.
3. **Combining features makes transfer worse, not better** (0.50 against 0.60 for envelope alone), which is what happens when extra features carry aircraft-specific detail.
4. **For these six types, RF alone does not give a reliable model name at this observation length.** Roughly one answer in two is wrong with two training aircraft.

## Limits

Six types known only by letter, three aircraft each, one publisher and receiver, 50 ms segments. If two letters are the same radio in different airframes, no method could separate them, and that cannot be checked without the model names. Longer observations and more aircraft per type were not tested.

## Consequence

Inside a timing group, the model name should come from decoded identity where it exists. Every type here is in the 15 kHz group used by DJI links that broadcast DroneID, whose payload carries the product type, and a C++ DroneID demodulator is already in the repository. RF naming inside a group should be reported as a candidate list, not as an accepted name.

## Artifacts

- `tools/drone_ml/descriptor_naming.py`; `data/drone_ml/2026-11-11/descriptors_01/extract.py`, `results.json`, `transfer_check.json`; features in `/media/sudeep/Storage/datasets/_derived/dronerfb_dir_descriptors_01`.
