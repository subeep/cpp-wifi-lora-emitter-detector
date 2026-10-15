# STFT activity and temporal-context development

13 October 2026. This is standalone offline model development. SDA originals remain
read-only. No radio was opened, and no C++ acquisition, GUI, security or cyclostationary
behavior changed. The first cross-unit benchmark and its artifacts are preserved.

## What changed

1. Added numerical activity diagnostics and an explicit insufficient-evidence gate.
   Eight STFT frames are averaged per block. Spectral contrast and relative temporal
   power changes identify structured RF activity. The initial policy uses 6 dB contrast
   over at least 10% of blocks in the central 75% of frequency bins. This is a proxy:
   coloured noise, unrelated transmitters and interference may pass; valid noise-like or
   weak transmissions may fail. It is not drone detection or verified signal SNR.
2. Added paired 1 ms and 8 ms spectrogram preparation. Per anchor, three consecutive 8 ms
   candidates are examined; one is selected by activity rank without using labels or model
   predictions. Its first 1 ms supplies the short representation, and the full 8 ms supplies
   the longer representation. Frequency views remain numerical and physically scaled.
3. Added a strict development mode whose tensors and split plan omit known-test and
   unknown-test recordings. It permits only training, calibration and unknown calibration.
   Both source selection before IO and split validation prevent test use.
4. Added calibration with aircraft-unit-balanced score/margin/distance tradeoffs. It uses
   known and unknown calibration only. The objective is `correct_accept - 2*wrong_accept
   - 2*unknown_accept`. Forty-five candidate policies are compared with abstention.
5. Added a safeguard: if calibration support is inadequate or no candidate beats the
   abstention reference, no usable rejection policy is enabled. The checkpoint reports
   why identification is unavailable. It does not invent fallback thresholds or present
   universal rejection as successful unknown detection.

## Development data and scope

Only 42 previously reviewed DRFF hover recordings are opened for IQ:

| Partition | Publisher aircraft units | Recordings | Selected contexts |
|---|---|---:|---:|
| Training | Air 2, Air 2S, Mini 4 Pro; units 1/2 | 18 | 108 |
| Known calibration | Same models; units 3/4 | 18 | 108 |
| Unknown calibration | Mini 3 Pro; units 1/2 | 6 | 36 |
| Total | 14 unit groups | 42 | 252 |

Receiver/day/state remain `u2`/`d1`/hover. Six anchors per source examine three possible
8 ms intervals each. The model operates on the selected interval, not on all underlying
activity. The new corpora are approximately 252 × 256 × 155 (short) and 252 × 256 × 625
(long). The short representation uses a 6.4 microsecond hop; the long uses a 12.8 microsecond
hop. Thus duration and temporal resolution differ. An 8 ms context does not establish
20 ms recurrence, hopping continuity or full-source coverage.

These examples still have publisher recording-context labels. We have not independently
established target presence, decoded protocol or window-level aircraft truth. The gate
changes sample eligibility and the selector changes the observation distribution; it
cannot repair a wrong model label. Selecting favourable contexts makes these results
different from full-stream operational detection metrics.

## Measured development results

Both networks use 20 fixed CPU epochs, seed 17, the same CNN size and training-only fitted
normalization. No final-test IQ, predictions or metrics are consumed by this development pass.

| Measurement | Selected 1 ms | Selected 8 ms |
|---|---:|---:|
| Prepared contexts | 252 | 252 |
| Activity-eligible contexts, all development partitions | 103 | 252 |
| Training contexts used | 41 / 108 | 108 / 108 |
| Eligible known-calibration contexts | 46 / 108 | 108 / 108 |
| Eligible unknown-calibration contexts | 16 / 36 | 36 / 36 |
| Forced known classification on all calibration contexts | 50 / 108 = 46.3% | 62 / 108 = 57.4% |
| Usable calibrated identification policy | Unavailable | Unavailable |

For the short model, correct eligible examples do not span two calibration aircraft units
for every class. Policy calibration fails its support check.

For the long model, the initial policy search produced a best candidate that correctly
accepted 46/108 known windows, wrongly accepted 10/108 known windows and accepted 20/36
unknown-calibration windows as known (55.6%). Its objective was negative (-0.8704), worse
than abstention. The corrected search includes that reference and disables the policy.
These initial diagnostic tradeoffs are not accepted performance. The safeguard was added
in response to calibration diagnostics; this is development, not a frozen final evaluation.

The longer representation supplies more eligible signal context and a stronger calibration
classification result, but unknown discrimination remains poor. This paired experiment also
changes eligibility, training count and time-hop size; it does not isolate duration as the
cause of improvement. The new 57.4% is **calibration accuracy**, not a replacement for the
first benchmark's 53.7% test accuracy. Calibration-guided improvements need new sealed release
evaluation before any generalisation claim.

## Artifacts and safeguards

Generated artifacts are in `data/drone_ml/2026-10-13/`:

- `activity_context_development/`: activity diagnostics, a development-only split plan and
  both numerical corpora. The original full split was not edited.
- `activity_context_experiment.json`: pretraining configuration and objective. The subsequent
  abstention safeguard is documented here as a calibration-stage correction.
- `short_activity_model_verified/`, `long_activity_model_verified/`: final experimental
  checkpoints and reports. These report unavailable policy rather than an accepted model.
- `short_activity_model/`, `long_activity_model/`: initial development outputs retained for
  reproducibility. Their initial calibration policies are superseded; do not deploy them.
- `activity_context_tests_verified.txt`: standalone ML regression tests.
- `activity_context_verification.json`: source, tensor, model and implementation checks.

Activity policy is carried in the checkpoint and applied by offline IQ prediction. Legacy
checkpoints without a gate retain their original behavior. Final models with missing policy
produce `identification_unavailable`; below-gate observations produce `insufficient_evidence`.
Uncalibrated candidate scores remain diagnostic, not probabilities of aircraft presence.

Tests cover noise versus synthetic structured activity, broadband bursts, bad input,
partition exclusion before IO, forbidden test partitions in the development runner,
group weighting, score margins, synthetic joint calibration and harmful-policy rejection.
Synthetic cases establish software behavior only. Fixed-seed verification repeats training
to check unchanged weights under the calibration safeguard; it is not a new accuracy trial.

## Next priorities established by this pass

1. Improve training robustness across units with physically grounded IQ augmentation and
   unit-balanced representation learning. Additional classifier capacity alone is not yet justified.
2. Investigate class/model ambiguity and feature consistency using training/calibration data.
   Two aircraft sharing RF structure may require a model group rather than exact identification.
3. Expand unknown-development diversity beyond one Mini 3 Pro model, keeping final unknown
   classes and physical units separate. Include independently labelled non-drone activity;
   unknown RF and unknown drone remain different conclusions.
4. Compare raw-IQ/temporal features and existing cyclostationary measurements with STFT-only
   on common splits. Longer STFT duration has not solved the unknown acceptance problem.
5. Qualify source/activity attribution, cross-receiver/day/domain transfer and encounter-level
   decisions before release evaluation, ONNX/C++ shadow integration or later Mini 2 testing.

The safe outcome of a failed calibration is explicit unavailability, not a named drone
prediction or an apparently perfect unknown-rejection percentage obtained by rejecting everything.
