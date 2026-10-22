# Optional model inference: implementation and validation

Updated: 22 October 2026. Software integration, expanded training and development audits
are complete. Scientific/live qualification remains incomplete. The new residual was
not promoted: see the [final comparison and insights](DRONE_ML_EXPANDED_FINAL_VALIDATION.md).

## Run the GUI

The separate build containing this addition is:

```bash
cd /home/sudeep/Documents/newrocktest-cpp
./build-ml-inference/rf_monitor_gui
```

The tab also displays a **Live model STFT** for each completed inference interval:
256 frequency bins by 625 time frames, covering 8 ms of the selected 20 MSps view.
The preview uses the actual pre-checkpoint-normalization log-power array, with an
8-bit display encoding; it does not recompute the FFT or modify model inputs/scores.
Frequency is vertical (MHz), time is horizontal (ms), and hover shows bin values.
The colour scale is per interval, in source-RMS-normalized relative dB, not dBm.
It refreshes on processed intervals (at most two capture jobs/s, up to three intervals/job);
stale/retuned/stopped observations are labelled historical, and invalid IQ has no preview.
Only the latest result retains its 160,000-byte display grid; immutable snapshots
share it, and the GPU texture updates only when a new grid arrives. A separate bounded
binary IPC frame is requested explicitly, so an already-running older GUI remains
compatible with the updated worker. Recording/replay keeps exact IQ for full-precision
reconstruction; display bytes are not training data. Restart the GUI to load UI changes.

Open **Model Inference**, then **Start Inference**. This loads the selected model in a
separate Python process and waits for existing Wi-Fi capture data. It does not connect,
retune or start the receiver. Connect/monitor through the existing controls when wanted.
The tab is also available in the legacy layout. Opening or hiding it does not start/stop
inference. **Stop Inference** affects only this add-on.

The default remains the previously completed residual checkpoint:
`data/drone_ml/2026-10-14/expanded_training_01/residual_model/model.pt`.
Use an absolute checkpoint path in the stopped tab to select another compatible model.
Newly trained models are not promoted automatically. Python is isolated under
`data/drone_ml/runtime-2026-10-14/`; the existing DJI decoder retains `/usr/bin/python3`.
The original build directories/executables were not replaced.

## What the tab displays

- Start/Stop Inference, checkpoint and Python runtime paths, worker state and errors.
- Undetermined presence / insufficient evidence, with candidate rankings in collapsed diagnostics.
- Optional exact-IQ audit recording and first/middle/end capture sampling; both keep exact interval provenance.
- Scores explicitly labelled **uncalibrated known-class softmax**; no drone-presence or
  unknown-drone probability is invented. Current live acceptance is disabled.
- Capture/session/run/source/receiver provenance, RF centre, selected model view, native
  sample rate, 8 ms interval, result age and historical/stale status.
- Preprocessing plus model time, queue-plus-processing time, queue depth, submitted,
  processed, skipped, invalid and stale counters, and maximum optional-copy callback time.
- Model hash, architecture, class list, bounded history (100 observations), raw response
  including full class scores and capture metadata, and explicit Copy JSON.

The raw response retains source device/host timestamps and clock domain. Processing time
does not include acquisition or existing detector processing before the observer call.
Observation counts are not physical drone counts. Clear history affects only this tab.

## Data and isolation contract

The existing `Scanner::WifiIqObserver` supplies immutable raw IQ and capture records after
existing packet processing. The application composes the new observer after the existing
DJI observer. `src/scanner.cpp` and `src/scanner.hpp` retain their task-baseline hashes.
No scanner, decoder, security threshold, acquisition schedule, radio owner or sample-rate
setting was changed by this implementation.

The new C++ supervisor has one waiting slot and one processing slot. Slots reserve at most
4,000,000 complex samples each outside acquisition. Submission uses a try-lock and bounded
copy, at most twice per second. If busy, it skips optional work and increments counters.
This avoids waiting on inference, but copying and resource contention still have measurable
cost: live on/off impact qualification is pending.

By default, the add-on selects up to three disjoint 8 ms intervals spread across an eligible
capture, within the existing per-job sample budget; a stopped-state checkbox restores
first-window-only sampling. Each interval is processed separately and carries its original
sample offset, interval index/count and capture-time anchor. These are not independent
votes or continuous temporal context. See [presence baseline and review-gated training](DRONE_PRESENCE_BASELINE.md).
Captures must
provide at least that many samples, have finite integer sample rate in the supported
20–500 MSps input range and no recorded overflow, timeout or exception. The upper input
limit derives from the bounded per-job allocation; it does not change USRP settings.
The model view remains 20 MSps. Shorter/narrower/gapped captures produce no prediction.
The first release does not assemble across captures or concatenate scan dwells. It samples
observations rather than claiming complete RF coverage or every-window processing.

The supervisor uses a local length-framed socket, starts one Python model process, closes
unrelated inherited descriptors, validates response lengths and exact capture identifiers,
and enforces a 15-second request deadline (60 seconds for model startup). The worker has
two Torch threads, reduced priority and a 4 GiB virtual-memory limit. Failure is local to
the add-on; restart is explicit. Main-app startup does not require Python/model availability.

Source/run/session/frequency changes and GUI monitoring-source changes invalidate queued
results. Stop also invalidates in-flight work. Results older than two seconds are displayed
as historical; receiver timestamps remain available separately. The two-second display
freshness is not proof that an emitter is still transmitting.

Inputs must match the 8 ms / 20 MSps model view / 256-point FFT / 256-sample hop profile.
IQ processing and compatible numerical FFT inference share the original preprocessing.
The live acquisition adapter currently feeds IQ only; arbitrary receiver/display FFTs do
not satisfy the model contract.

## Expanded model experiment

Root: `data/drone_ml/2026-10-20/expanded_v2/`.

- Re-inventoried 808 source items across all five available dataset collections.
- Reviewed 517 recording/member entries, versus 468 previously; 411 enter development
  preparation and 106 remain in reserved test partitions. There are 331 training records.
- Expanded to 27,264 tensors: 14,832 originals and 12,432 training-only augmented variants.
  Original sampled duration is 118.656 seconds, not that many independent encounters.
- Original windows by source: Leuven 1,056; Tampere 864; RFUAV 432; DRFF 12,480.
  Verified aircraft labels remain unavailable for the FPV equipment data; it is accounted
  for and excluded from aircraft-identity training rather than mislabelled.
- Training uses up to 48 spaced windows per recording, up to 16 Leuven archive members,
  and up to eight eligible RFUAV packs (only nine packs total qualified in this review).
  RFUAV sampling spans reviewed member duration through bounded streaming reads rather
  than the earlier 160 ms prefix. Whole-member CRC verification is not claimed for RAR.
- Calibration remains the same 432 known and 528 unknown original intervals. Independent
  aircraft partition assignments are preserved. Dataset originals remain unchanged.
- The fixed compact-then-residual comparison targets 40 epochs each. Compact training
  completed on CPU; residual training continued on the RTX 3060 Ti after 14 CPU epochs.
  Atomic epoch states permit resume. Frozen implementation and launch hashes are stored
  with the run. See [GPU migration evidence and reproducibility limits](DRONE_ML_GPU_MIGRATION.md).

The compact model completed, but is not an overall improvement:

| Metric | Previous compact | Expanded compact |
|---|---:|---:|
| All known-window accuracy | 68.75% | 68.29% |
| Activity-eligible accuracy | 67.63% | 66.32% |
| Group-balanced accuracy | 67.71% | 69.21% |
| Recording-vote accuracy | 77.78% | 69.44% |

Only three known classes have cross-aircraft calibration groups. Eighteen other classes
are training-only extensions, not validated identity coverage. More windows from the same
aircraft do not fix this evidence gap. The original residual error audit found zero correct
Air 2S predictions for aircraft unit 4's calibration windows despite high scores.

## Calibration and model promotion

`calibration_audit.py` builds group-balanced class prototypes from eligible original
training embeddings, fits a scalar logit temperature on one development unit per known
class, and selects score/distance thresholds using a separate unknown-development group.
It evaluates those frozen choices on the other known/unknown development groups. These
groups were previously inspected; this is development evidence, not a sealed final test.

The original residual's candidate rejection policy correctly accepted only 17.17% of
known assessment windows while falsely accepting 1.34% of unknown assessment windows.
The expanded compact's corresponding values are 19.19% and 3.23%. Neither supports release.
There is still only one unknown calibration aircraft model and no verified ordinary-RF
background evaluation. No rejection policy or calibrated live probability was enabled.

The dependent calibration job completed both audits and the development ranking.
The new residual correctly accepted 31.31% of known assessment windows, with 1.88%
unknown false acceptance; 68.69% of known windows were rejected. This does not qualify
the policy for release. The job did not change the GUI default model or enable live
acceptance. Its code is separately frozen under
`postprocessing_implementation/`.

Progress commands:

```bash
watch -n 2 'cat /home/sudeep/Documents/newrocktest-cpp/data/drone_ml/2026-10-20/expanded_v2/status.json; cat /home/sudeep/Documents/newrocktest-cpp/data/drone_ml/2026-10-20/expanded_v2/residual_model/progress.json; cat /home/sudeep/Documents/newrocktest-cpp/data/drone_ml/2026-10-20/expanded_v2/calibration_status.json'
```

Source preparation used a managed execution session with the sandbox's read-only dataset
view. The GPU training continuation and calibration coordinator are detached host
processes and read only prepared workspace tensors. Run/session metadata is
in `process.json` and `calibration_process.json`; checkpoint recovery remains available
if the training process is interrupted. A status file alone is not a liveness guarantee.

## Verification and outstanding work

Evidence is under `data/drone_ml/2026-10-20/integration/`:

- All 69 Python ML regression tests passed, including framed-worker/direct-inference parity,
  input validation, archive streaming, group separation, FFT parity and exact epoch resume.
- C++ supervisor tests: default-disabled isolation, gaps, invalid rates, input preservation,
  bounded duty, 64-bit capture IDs, invalidation, timeout, crash and concurrent capture/restart.
- All five selected CTest targets passed: the C++ supervisor, existing scanner Wi-Fi lane,
  DJI add-on and Wi-Fi GUI regressions, and real native GUI
  navigation tests including the new tab. The initial build's decoder Python mismatch was
  corrected to match the existing build; final regression evidence supersedes that failure.
- Real trained checkpoint + existing offline IQ through the production C++ supervisor:
  candidate, score, accepted-class and decision exactly match direct Python inference.
  A single probe measured about 160 ms of preprocessing plus model execution; this is
  not a throughput, percentile latency or live-radio qualification claim.
- A rendered, disconnected native GUI preview was visually inspected. It is labelled
  synthetic and does not depict live RF observations.

Outstanding: Air 2S unit-failure investigation, finalist repeat-seed/architecture ablations
as justified by the new results, broader independent units and unknown classes, verified
non-drone background, final frozen test evaluation, and live USRP/ Mini 2 accuracy and
on/off resource-impact qualification. Exact drone identity and unknown-drone recognition
remain unvalidated. Existing code/data preservation and offline tests do not establish
zero live performance impact.
