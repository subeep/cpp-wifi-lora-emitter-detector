# First STFT cross-unit drone-model experiment

13 October 2026. Standalone offline research only. The C++ receiver, GUI, cyclostationary
analysis and security processing were not changed. SDA was mounted read-only after being
found unmounted. No radio was opened and no dataset original was modified.

## Result and decision

The initial STFT-only CNN does not yet support reliable operational identification.
The first frozen experiment provides a real held-unit baseline rather than a training
accuracy claim. Keep its results fixed; future improvements must be selected using training
and calibration data, with new sealed evaluation needed for release.

| Measure | First result | Scope |
|---|---:|---|
| Forced known-model classification | 58 / 108 windows = 53.7% | Three known classes; one held aircraft unit per class |
| Known windows correctly identified and accepted | 13 / 108 = 12.0% | Experimental score-plus-distance rejection |
| Known windows accepted at all | 26 / 108 = 24.1% | Half of these 26 accepted windows have the wrong class |
| Known windows rejected | 82 / 108 = 75.9% | Large loss of useful identification coverage |
| Held unknown-context windows accepted as known | 3 / 36 = 8.3% | One publisher model context, one unit, three recordings |
| Unknown calibration windows accepted as known | 12 / 72 = 16.7% | Mini 3 Pro, two publisher units |

These percentages describe correlated windows in this experiment. They are not general
drone accuracy, receiver qualification, false alerts/hour, independent encounter confidence
intervals or evidence of reliable unknown-drone detection. A low unknown acceptance result
must be considered together with the very high rejection of known examples.

Whole-recording averages correctly classify six of nine known test recordings (66.7%),
but those nine recordings represent only three held aircraft units. This is a diagnostic
of temporal aggregation, not nine independently tested aircraft. Open-set decisions have
not yet been qualified at encounter level.

## Reviewed source selection and frozen partitions

Sources: `DRFF-R2/DRFF-R2/dataset3-single_drone_hover` on SDA. The local publisher scripts
were read as documentation, never executed. Their target identifier, receiver and day
fields were checked against embedded MATLAB `TD`, `U`, `D`, `State`, `C`, `Fs` and centre
frequency values. Different aircraft units are represented by the suffix after the model;
`u1/u2` identify receiver configurations. The [publisher paper](https://arxiv.org/html/2603.00106v2)
also describes unit, receiver and day variation. Matching metadata remains publisher
evidence; it does not independently verify every signal interval or protocol.

| Role | Selection | Files | 1 ms intervals |
|---|---|---:|---:|
| Known training | Air 2, Air 2S, Mini 4 Pro; aircraft units 1 and 2 | 18 | 216 |
| Known calibration | Same three models; aircraft units 3 and 4 | 18 | 216 |
| Known test | Same three models; aircraft unit 5 | 9 | 108 |
| Unknown calibration | Mini 3 Pro; aircraft units 1 and 2 | 6 | 72 |
| Unknown test | `mavic3S` publisher context; unit 1 | 3 | 36 |
| Total | 18 publisher unit groups | 54 | 648 |

All sources use receiver `u2`, day `d1`, hover and conditions `c1/c2/c3`, with embedded
100 MSps and 5.745 GHz. All conditions/windows of each aircraft unit remain in one
partition. Mixtures, environment controls, other receivers/days and unsupported units
are excluded from this experiment. It therefore measures limited cross-unit transfer
within one collection, not cross-day, cross-receiver, cross-band or deployment transfer.

The unknown `mavic3S` identifier is retained without automatically resolving it into an
exact commercial model. Mini 3 Pro is not a trained class in this checkpoint. The unknown
example is a publisher-labelled drone recording context, not a new named class discovered
from RF. This experiment supplies no ordinary router/phone/non-drone negatives and does
not establish drone-versus-background detection.

Partitions were recorded before extracting IQ and before training. Epochs (20), seed (17),
batch size (16), interval count (12 per file) and class selection were recorded before
training. No model choice or threshold was selected using final test results. Previously
exposed short development extracts were not promoted to this test set.

## Processing and model

Each source supplies 12 deterministic intervals spread through its recording, with capture
edge margins. A bounded HDF5 slice is read directly; whole IQ recordings are not loaded.
Each interval is independently mean/RMS-normalized. A five-view source PSD search chooses
the strongest guarded subband, followed by polyphase filtering/downsampling to a 20 MSps
view, numerical two-sided STFT, 256 frequency bins and 155 time frames. The array preserves
physical scales; it contains no filenames, device labels, plot axes or colour maps.

This 20 MSps view is a model input experiment, not a radio sample-rate limit. Selecting one
subband loses other simultaneous activity and may select interference/noise. Target presence
within each 1 ms interval is not verified. The current representation lacks hopping history
and longer burst context; it does not establish full-band detection coverage.

The CNN has 177,923 parameters for three classes and a 64-value normalized embedding.
Training is deterministic CPU-only with two Torch threads, group/class-balanced sampling
and training-only normalization statistics. The run used 20 fixed epochs without final-test
early stopping. Normalization, prototypes and gradients never use final-test examples.

Thresholds are derived from correctly classified known calibration examples with at least
two publisher aircraft-unit groups per class. Unknown-calibration score quantiles further
tighten acceptance. This policy is a research starting point and does not enforce a global
5% false-accept guarantee: classwise quantiles and correlated observations do not imply that
guarantee, and the actual unknown-calibration false-accept result is 16.7%.

## Confusion and useful acceptance

Forced classification on the 108 known test windows, rows = publisher truth:

| Truth / prediction | Air 2 | Air 2S | Mini 4 Pro |
|---|---:|---:|---:|
| Air 2 | 17 | 16 | 3 |
| Air 2S | 5 | 25 | 6 |
| Mini 4 Pro | 5 | 15 | 16 |

After rejection, correctly accepted windows are Air 2 **9/36**, Air 2S **0/36**, Mini 4 Pro
**4/36**. The Air 2S result demonstrates why ordinary closed-set classification accuracy
cannot stand in for useful open-set identification. Similar link structures, unit/domain
variation and unverified window activity are candidate explanations; this experiment does
not isolate their causal contributions or prove the models inseparable.

## Artifacts and verification

Generated artifacts are in `data/drone_ml/2026-10-13/`:

- `cross_unit_review/review.json`: 54 source records, embedded metadata and source statistics.
- `cross_unit_review/split_plan.json`: 18 group decisions and evidence scope.
- `cross_unit_experiment.json`: frozen experiment configuration.
- `cross_unit_stft/corpus.json`, `spectrograms.npz`: derived inputs and interval hashes.
- `cross_unit_model/model.pt`, `report.json`: trained checkpoint, thresholds and evaluation.
- `cross_unit_training.txt`: epoch losses and completion status.
- `cross_unit_implementation/`: exact implementation corresponding to the first benchmark.

Source preservation checks use stat tuples and read-only/no-atime handles. Selected-interval
hashes and tensor/model hashes are recorded. Whole-source IQ checksums, RF continuity and
calibrated analogue passbands are not verified by this work.

Code lives under `tools/drone_ml/`; runnable commands are in its README. Tests cover metadata
conflicts, receiver/unit grouping, indirect HDF5 storage, bounded intervals, physical STFT
axes, filtering, normalization, split leakage, frozen split binding and synthetic open-set
workflow. Synthetic tests measure software behavior, not drone accuracy.

## Next work, ordered by the findings

1. Diagnose training/calibration activity and signal-quality distributions. Build candidate
   activity labels/masks so quiet or unrelated intervals do not silently inherit drone labels.
2. Compare multiple simultaneous frequency views and longer temporal context using only
   training/calibration groups. Keep physical time/frequency scales and missing coverage.
3. Add realistic channel/noise/frequency perturbations and assess cross-unit consistency;
   compare against a simple feature classifier before increasing model size.
4. Improve rejection using group-aware calibration and measure accepted-error/coverage tradeoffs.
   Choose operating thresholds jointly with known-class retention, not solely unknown rejection.
5. Extend cross-day/receiver/state evaluations and introduce independently reviewed non-drone
   activity. Preserve aircraft-unit grouping across all datasets and derived files.
6. Qualify encounter-level aggregation and confidence intervals; repeated windows and height
   variations are not independent aircraft trials.
7. Compare STFT-only, cyclostationary-only and combined inputs on the same partitions. Exact-model
   ambiguity may require model groups; it cannot be solved merely by increasing a softmax score.
8. Reserve fresh independently labelled release evaluation after development, then ONNX/C++
   shadow integration and later local USRP/Mini 2 qualification. GUI integration remains deferred.

Do not tune the model to make this first test table look better and then call the same table
an untouched final validation. Preserve it as an exploratory baseline and disclose its scope.
