# Initial drone-system RF activity experiment

The initial offline path connects reviewed live captures, presence training and
bounded IQ scoring. Its target includes drone/controller system activity.
Transmitter role remains unresolved unless independently labelled.

## Data preparation and training

Use the existing Model Inference audit recorder to collect exact intervals.
Create and review a manifest using `tools.drone_ml.presence_dataset`. Each
admitted interval needs an independently supported label, verified frequency
view and collection grouping. Keep controller-only observations distinguishable
in the evidence. For this target, controller-system activity can be positive
without establishing an aircraft link or flight.

The trainer requires at least two independent groups of each label in each of
train, calibration and development. These are minimum software readiness checks.
Representative scenes, sufficient negative hours and independent final sessions
are still needed for qualification. Review uncertain identity as unresolved
explicitly in the existing `drone_model` field when presence alone is supported.

After preparing a reviewed corpus, run:

```bash
data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.presence_train \
  --corpus /absolute/path/prepared/corpus.json \
  --output /absolute/path/new-training-output --device cpu
```

Training balances labels and collection groups, fits normalization on training
data, fits diagnostic thresholds on calibration and evaluates development
separately. Existing output directories are refused.

## IQ scoring

```bash
data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.presence_predict \
  --model /absolute/path/new-training-output/model.pt \
  --iq /absolute/path/capture.cf32 --sample-rate-hz 20000000 \
  --offset-samples 0
```

The command reads one bounded 8 ms CF32 interval and emits JSON with source and
checkpoint hashes. It uses the training preprocessing and normalization.
`activity_score` is an experimental softmax score. Presence and accepted identity
remain null. Finite varying observations are forwarded for identity assessment,
including noise-like or tone-like observations. Invalid or constant samples
defer the observation. Protocol recognition never vetoes Wi-Fi-based drones.

## Current completion limits

The reviewed inventory has no verified negative evaluation corpus. No real-data
presence weights were trained for this increment. The synthetic training test
checks checkpoint-to-IQ scoring, source geometry, routing and invalid input.
It supplies no RF accuracy evidence.

The next stage requires reviewed background and positive groups, then real-data
training and complete-session shadow replay. Multi-region localization, live
presence-worker integration, event aggregation and qualified background skipping
remain pending. This initial scorer uses the existing five-center frequency
selector, whose coverage limitations must be measured before promotion.
