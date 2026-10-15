# Standalone drone RF STFT learning

This experimental Python project has no connection to the C++ acquisition, decoder,
security monitor or GUI. It inventories SDA metadata, prepares numerical STFTs from
existing derived IQ, trains a compact CNN and performs bounded offline inference.
It does not currently supply validated drone/model identification.

## What is implemented

- `audit`: inventory the five explicit SDA dataset directories, list ZIP members,
  read bounded DRFF HDF5 metadata and produce an excluded-by-default review template.
  RAR payloads are not decompressed. Inventory does not establish whole-file integrity,
  aircraft absence, window labels or independent sessions.
- `prepare`: verify hashes of the eight existing Leuven/Tampere development extracts,
  select deterministic contiguous intervals and generate float32 spectrogram arrays.
  The supported input schema is `local_link_references.v1`; this is deliberately a
  bounded development importer, not an importer for the entire SDA corpus.
- `train`: CPU training with a small CNN and group/class-balanced sampling. Means and
  scales are fitted to training tensors only. A development smoke checkpoint cannot
  silently become an independently validated model.
- `splits`: explicit recording-group assignment with derivative/source overlap checks,
  known-class partition requirements and completely withheld, distinct unknown
  calibration/test classes. Eligible decisions require written evidence.
- Open-set calibration machinery: class score and embedding-distance thresholds,
  independent-group minimums and separate final unknown metrics. Tested using synthetic
  fixtures; **not yet calibrated on independent real RF sessions**.
- `predict`: reusable standalone engine with bounded IQ and compatible numerical FFT
  sequence inputs, experimental status and uncalibrated scores.
  The smoke checkpoint never emits an accepted class or an unknown-drone assertion.

Source handles are regular-file, read-only, nonblocking and no-atime. Output directories
must be new and outside source roots. SDA is additionally mounted read-only on this host.
Preparation checks original source statistics and hashes; audit checks file statistics.
Archive directory CRC fields are metadata, not verified member CRCs. No publisher scripts
are executed. Originals are never extracted in place.

## Larger model: prepared; training started 14 October 2026

The expanded experiment, source exclusions, model layers and isolation boundary are
specified in [DRONE_ML_EXPANDED_PRETRAINING.md](../../docs/DRONE_ML_EXPANDED_PRETRAINING.md).
Its final paths and checks are in `data/drone_ml/2026-10-13/expanded_pretraining/handoff.json`.
Use those paths rather than interrupted intermediate preparation folders.

- `corpus_sources`: metadata/role review and conservative group assignments.
- `expanded_corpus`: bounded IQ extraction, training-only physical augmentation and NPY shards.
- `residual_model`: 2.14 million parameter residual CNN with a 128-value embedding.
- `expanded_training`: preflight by default; optimization requires `--start-training`.

This broader experiment has 21 recording-context classes, but only three currently have
cross-aircraft calibration groups. It has no verified non-drone head or release-ready
unknown-drone decision. The preparation handoff stopped before optimization. Training was subsequently authorized
and started; see [the current run details](../../docs/DRONE_ML_TRAINING_RUN.md).

## Optional FFT input

The standalone `InferenceEngine` loads a checkpoint once and accepts IQ arrays or
compatible complex/power FFT sequences. See [the FFT input contract and isolation rules](../../docs/DRONE_ML_FFT_INPUT.md)
for API/CLI examples, producer requirements and future integration boundaries. No live
USRP, C++ or GUI connection exists. Current GUI max-hold spectra are not suitable inputs.

## Physical preprocessing contract v1

The development profile uses one 1 ms contiguous IQ interval, a 20 MSps frequency view,
256-point periodic Hann windows and a 128-sample hop. This produces a **256 × 155**
frequency/time array with 78.125 kHz bins and a 6.4 microsecond hop. The 20 MSps profile
is an initial model input experiment; it does not impose a receiver sample-rate cap.

Processing removes the complex mean, normalizes source RMS, optionally mixes an explicitly
requested frequency offset, applies Kaiser polyphase low-pass resampling, then computes
a two-sided FFT-shifted log-power STFT with a -100 dB numerical floor. There are no plotted
axes, labels, colour maps or image resizing. RMS normalization removes absolute power;
gain is not an identification feature. Resampling uses boundary padding; filter-edge
effects remain present in this initial profile and require robustness evaluation.

The default frequency offset is zero relative to the capture centre. This examines only
the central 20 MHz of the 100/120 MSps development recordings. It does **not** cover every
hop, detect all candidate bursts or provide 20 ms recurrence context. Missing drone
activity inside an interval is not a negative label. These are recording-context learning
examples, not verified window-level labels. Multi-frequency candidate selection and longer
context must be evaluated before an operational training corpus is built.

The CNN returns class logits and a normalized 64-value embedding. It uses four convolution
stages, GroupNorm, spatial pooling and a small linear head. The six-context smoke model has
178,118 parameters. Pooling happens inside the network; physical input axes are not warped
to a generic image shape.

## Reproduce the first development run

Run from the repository root. Python 3.14 was used for this run. The isolated environment
currently exists at `/tmp/rf-stft-venv`; `/tmp` can be cleared between sessions.

```bash
python3 -m venv /tmp/rf-stft-venv
/tmp/rf-stft-venv/bin/python -m pip install numpy scipy h5py
/tmp/rf-stft-venv/bin/python -m pip install torch --index-url https://download.pytorch.org/whl/cpu

/tmp/rf-stft-venv/bin/python -m tools.drone_ml.audit \
  --root /media/sudeep/Storage/datasets \
  --output data/drone_ml/new-run/inventory

/tmp/rf-stft-venv/bin/python -m tools.drone_ml.prepare \
  --references data/wifi_drone_analysis/local_link_references_2026-10-09/manifest.json \
  --output data/drone_ml/new-run/stft --windows-per-record 24

/tmp/rf-stft-venv/bin/python -m tools.drone_ml.train \
  --corpus data/drone_ml/new-run/stft/corpus.json \
  --output data/drone_ml/new-run/model --epochs 3 --development-smoke

/tmp/rf-stft-venv/bin/python -m tools.drone_ml.predict \
  --model data/drone_ml/new-run/model/model.pt \
  --iq data/wifi_drone_analysis/local_link_references_2026-10-09/KULeuven_DroneRF_mini2vid.cf32 \
  --format cf32_le --sample-rate-hz 100000000

/tmp/rf-stft-venv/bin/python -m unittest discover -s tests -p test_drone_ml.py -v
```

Use a fresh output directory for every run. `requirements-tested.txt` records the tested
top-level versions; the run directory also contains a full package freeze. A standard PyPI
Torch install may select a different build, so use the CPU index explicitly for this run.

## Real-data smoke results: 13 October 2026

Artifacts are in `data/drone_ml/2026-10-13/` (ignored generated data):

| Artifact | Result |
|---|---|
| `inventory/inventory.json` | 808 RF files/archives: Leuven 16, Tampere 19, RFUAV 37, DRFF 726, FPV 10 |
| `inventory/review_template.json` | All 808 excluded pending label/independence review |
| `development_stft/corpus.json` | Eight derived recordings, 192 intervals, seven conservative groups |
| `development_stft/spectrograms.npz` | 192 numerical 256 × 155 STFT arrays |
| `development_model_verified/model.pt` | Six recording-context classes, three CPU epochs, repeated run |
| `development_model_verified/report.json` | Training loss only; code/dependency provenance, no independent accuracy claim |

Context classes: Mini 2, Inspire 2, Matrice 300 context, Mavic Pro, Parrot Disco and Parrot
Mambo. Mini 2 controller/video stay in the same group. Inspire 2 from Leuven and Tampere
are separate collection groups, but their independence is not verified. Having class names
in a checkpoint does not establish reliable identification of those aircraft.

The audit re-observed two DRFF indoor-environment paths with `mini5PRO_1` / `hover` metadata;
both remain ineligible as certified negatives. It also retains the prior RFUAV FRSKY/FLYSKY
conflict. RFUAV RAR internal labels were not freshly revalidated by this inventory.

No production CMake target, GUI page, radio setting, detector threshold, cyclostationary
feature, ML/cyclostationary fusion or Remote ID functionality was changed.

## Before independent training and release

The activity/temporal development pass is documented in
[DRONE_STFT_ACTIVITY_CONTEXT_DEVELOPMENT.md](../../docs/DRONE_STFT_ACTIVITY_CONTEXT_DEVELOPMENT.md).
Its corpora physically exclude known-test and unknown-test IQ. Development-only training
also rejects those partitions. The new quality gate is a structured-RF proxy and is shared
with offline prediction; it does not establish a drone or a window-level model label.

```bash
/tmp/rf-stft-venv/bin/python -m tools.drone_ml.development \
  --review data/drone_ml/2026-10-13/cross_unit_review/review.json \
  --output data/drone_ml/new-development/context
/tmp/rf-stft-venv/bin/python -m tools.drone_ml.train \
  --corpus data/drone_ml/new-development/context/long_activity/corpus.json \
  --split-plan data/drone_ml/new-development/context/split_plan.json \
  --output data/drone_ml/new-development/model --epochs 20 \
  --development-only --joint-calibration
```

Joint calibration evaluates score, margin and prototype-distance choices with equal unit
weight. Its objective rewards correct identification and penalizes wrong-known and unknown
acceptance. An explicit abstention reference is included. If no candidate improves that
objective, policy stays unavailable and prediction cannot emit an accepted model. This
does not establish a false-accept guarantee or measure final-test generalisation.

The first reviewed cross-unit DRFF experiment is now complete. See
[the benchmark report](../../docs/DRONE_STFT_CROSS_UNIT_BASELINE.md) for the frozen split,
measured limitations and exact next priorities. It remains an exploratory publisher-labelled
benchmark; the original development smoke run is kept separately.

```bash
/tmp/rf-stft-venv/bin/python -m tools.drone_ml.drff curate \
  --root /media/sudeep/Storage/datasets --output data/drone_ml/new-cross-unit/review
/tmp/rf-stft-venv/bin/python -m tools.drone_ml.drff prepare \
  --review data/drone_ml/new-cross-unit/review/review.json \
  --output data/drone_ml/new-cross-unit/stft --windows-per-record 12
/tmp/rf-stft-venv/bin/python -m tools.drone_ml.train \
  --corpus data/drone_ml/new-cross-unit/stft/corpus.json \
  --split-plan data/drone_ml/new-cross-unit/review/split_plan.json \
  --output data/drone_ml/new-cross-unit/model --epochs 20
/tmp/rf-stft-venv/bin/python -m unittest discover -s tests -p 'test_drone_ml*.py' -v
```

This importer reads bounded slices directly from MATLAB v7.3 I/Q arrays and checks embedded
target, state, receiver, day, condition, rate and centre against the selected filenames.
It excludes mixtures and unverified environment negatives. The optional v2 frequency-view
selector averages source PSD, scores five guarded candidate views by central-band energy
and selects one deterministic 20 MSps view per interval. The selected offset is retained
in each record. It can select interference or noise and does not establish target activity.
The same selector is used by offline inference. Old v1 checkpoints remain readable.

If independent calibration support is insufficient for any class, training retains a
checkpoint and closed-set diagnostics with rejection marked unavailable. It does not
invent fallback thresholds. Frozen split checksums are enforced for new prepared DRFF
corpora. The first benchmark implementation has been preserved before subsequent guard
improvements; see `cross_unit_implementation/` under the run directory.

1. Review class aliases, aircraft/controller roles, mode, physical-unit and session evidence
   for SDA recordings. Record unresolved facts instead of inventing an automatic split.
2. Implement reviewed SDA IQ adapters and candidate selection. Do not edit development
   partitions to make existing exposed smoke samples look like a sealed evaluation set.
3. Prepare a genuinely reviewed corpus with the same tensor contract, indices, original
   recording/group identities, source hashes and explicit unassigned partitions. Supply
   a `rfmon.drone_ml.split.v1` plan via `train --split-plan PATH`. Each known class needs
   separate training/calibration/test groups; calibration needs at least two correctly
   classified groups per class. Completely different unknown classes are required for
   calibration and final testing. Excluded records remain excluded from training/metrics.
4. Add trustworthy ordinary Wi-Fi/controller/non-drone negatives and window-level activity
   labels. This initial model has no trained drone-versus-background category head.
5. Compare STFT-only against feature-only and combined models using identical group splits.
   Add realistic physical distortions and test partial-band, multi-source and receiver shifts.
6. Measure independent model confusion, unknown false acceptance, known rejection,
   end-to-end misses, false alerts/hour and decision latency. Current metrics are window
   summaries; encounter aggregation and confidence intervals remain pending.
7. Export ONNX, verify Python/C++ preprocessing and inference parity, then add a bounded
   optional inference worker in shadow mode. No ONNX/C++ integration exists yet.
8. Qualify later local USRP sessions, including the postponed Mini 2 tests. Do not treat an
   unknown RF signal as an unknown drone or automatically train on model-generated labels.

The reviewed-training code remains experimental: current score quantiles are a starting
policy, not a calibrated operational false-alert guarantee. Every report remains marked
ineligible for deployment until the independent evidence is established.

References for the implemented library operations: [SciPy polyphase resampling](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.resample_poly.html)
and [PyTorch convolution](https://docs.pytorch.org/docs/stable/generated/torch.nn.Conv2d.html).
