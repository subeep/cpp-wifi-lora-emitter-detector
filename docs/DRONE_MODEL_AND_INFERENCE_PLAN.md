# Drone model improvement and optional live inference tab

Updated: 20 October 2026  
Status: planning only. This revision does not start training or implement integration.

Implementation update, 22 October 2026: the user subsequently authorized execution.
Data expansion, both 40-epoch training runs and development audits are complete.
The new residual did not justify promotion; see the [final comparison](DRONE_ML_EXPANDED_FINAL_VALIDATION.md).
The optional native inference tab/data adapter are implemented in the
separate `build-ml-inference` build. Calibration audits remain development-only. See
[implementation and remaining validation](DRONE_ML_LIVE_INTEGRATION.md) for measured
results and incomplete milestones. The original plan below is retained as the target;
it does not imply that every qualification milestone has been achieved.

## 1. Requested outcome and priority

First develop the most appropriate model using lessons from the completed experiments.
Then add a native C++/Dear ImGui **Model Inference** tab with a **Start Inference** button,
model results, clearly labelled scores/probabilities, and capture/runtime details.
Reuse data already acquired by the existing workers. Preserve every existing receiver,
scanner, decoder, cyclostationary, security and GUI function.

“All datasets” means accounting for all available sources and using every eligible source
in an appropriate role. It does not mean putting validation/test samples, ambiguous labels
or unrelated transmitter labels into supervised aircraft-model training. Perfect recognition
cannot be promised: some aircraft share link hardware/waveforms, recordings can contain
controller and aircraft transmissions, and unseen conditions can change predictions.

The immediate priority is milestones M0–M4 below. The tab and live wiring are subsequent
work, explicitly included here rather than implemented now. Live Mini 2 validation remains
postponed until the user is ready. No further Remote ID work is required for this plan.

## 2. Evidence from the completed experiment

Source: `data/drone_ml/2026-10-14/expanded_training_01/comparison.json`.
Both models completed 40 epochs on the same prepared corpus.

| Development metric | Residual CNN, 2,137,952 parameters | Compact CNN, 179,093 parameters |
|---|---:|---:|
| Top-1, all 432 known-class windows | 71.53% | 68.75% |
| Top-1, 380 activity-eligible windows | 69.21% | 67.63% |
| Group-balanced window accuracy | 68.40% | 67.71% |
| Recording mean-probability vote, 36 recordings | 83.33% | 77.78% |
| Median highest score on withheld Mini 3 Pro windows | 87.17% | 64.98% |
| 95th percentile highest score on those unknown windows | 97.78% | 95.88% |

The recording aggregation used in this report is not yet a validated streaming decision.
Only Air 2, Air 2S and Mini 4 Pro have known-class calibration groups in this experiment.
Results do not establish performance across all 21 trained labels. Residual per-class
window accuracy was 146/180 for Air 2, 93/180 for Air 2S, and 70/72 for Mini 4 Pro.
The unknown data covers one withheld model and two groups, not general unseen-drone coverage.

Interpretation:

- The larger network is the leading reference, but its group-balanced gain is small.
  Increasing parameter count alone is not the next justified step.
- Strong unknown scores show why a simple high softmax threshold is insufficient.
- Air 2S errors and confusion into related DJI classes need direct investigation.
- Only 4,328 original 8 ms intervals were prepared: 34.624 seconds of sampled RF across
  362 source recordings/members. Augmentation raised the tensor count to 7,367 without
  adding independent aircraft. Broader representative sampling is a major opportunity.
- Dataset/source/receiver differences and mixed controller/aircraft labels can create
  shortcuts. High training performance does not resolve those issues.
- Some classes have extremely sparse eligible originals (for example, Disco: three).
  Additional windows from the same aircraft cannot establish cross-aircraft generalization.

## 3. Data expansion before a new training run

### Inventory and defensible labels

Read dataset originals without modifying, renaming, extracting into or annotating their
directories. Store derived manifests/tensors only under a new workspace experiment directory.
Compare with the existing 808-item inventory; account for newly available sources as well.
Do not assume filenames resolve the previously recorded metadata conflicts.

For each source record dataset/version, aircraft label, transmitter role, physical unit,
session/day, receiver, frequency, sample rate, numeric format, duration, continuity evidence,
checksum scope and label confidence. Keep unknown fields unknown.

Assign explicit roles:

| Evidence | Permitted use |
|---|---|
| Trusted model label and independent groups | Supervised identity development and separated evaluation |
| Model-context label, unresolved emitting source | Weak-label development; report this limitation |
| Controller/transmitter equipment only | Role/link research or verified non-target examples; not an aircraft identity |
| Ambiguous/conflicting label or unresolved format | Quarantine pending review; no invented label |
| Verified non-drone Wi-Fi/other RF | Background and false-alarm evaluation, with separate training/calibration/test groups |
| Unseen aircraft classes | Disjoint unknown-development and unknown-test roles |
| Mixtures | Separate robustness evaluation; not a clean single-source label |

Router/phone labels require suitable ground truth. Ordinary Wi-Fi alone does not identify
a router, phone or drone. “Test transmitter off” does not establish that no drone was present.

### Sampling and leakage control

Replace sparse fixed prefixes with bounded, deterministic sampling across accepted recordings:
different time regions, operational states, receivers, bands and channel conditions. Audit
activity eligibility and record excluded windows so the gate cannot hide failures.
Use stratified quotas to prevent a large dataset or long recording from dominating.

Split by physical aircraft first, with all its sessions, bands, windows and augmentations
kept together. Use conservative grouping where aircraft identity is unresolved. Audit
cross-dataset duplicates and temporal overlap; never randomly split adjacent windows.
Report source-domain holdout performance where the data actually supports it.

Separate training, model-development, probability/rejection-calibration and final test.
The already inspected calibration set is development evidence; it is not a fresh final test.
Where independent groups are too scarce, use grouped development folds and out-of-fold
predictions, disclose limitations, and defer a validated class claim.
Preserve existing reserved-test assignments. Any later change in an unknown class's role
requires a new experiment version and new independent unknown evaluation classes.

Use training-only augmentation with physically plausible CFO, timing shifts, noise and
multipath, plus receiver variations only where justified. Never infer true SNR from the
existing observed-power/added-noise ratio. Fit normalization on training data only.

Deliver a class coverage table listing independent units/sessions/domains and eligible
original duration in each partition. Distinguish class availability from validated coverage.

## 4. Model development strategy

### Recommended first candidate

Keep the existing residual CNN as the reference architecture. Its current path is:

`8 ms IQ → conditioned 20 MSps view → 256 × 625 log-power STFT → residual encoder
→ 128-dimensional embedding → cosine class logits`

| Layer group | Current implementation |
|---|---|
| Stem | 5×5 convolution, 32 channels, stride 2; GroupNorm; SiLU |
| Residual stages | Two blocks each at 32, 64, 128 and 192 channels; downsample at stage transitions |
| Each residual block | Two 3×3 convolutions with GroupNorm and a skip connection |
| Pooling | Adaptive average pooling to 2×4 |
| Embedding | Flatten 1,536 → linear 128 → LayerNorm → SiLU → L2 normalization |
| Identity head | Training dropout 0.2; normalized class weights; cosine logits |
| Decision layer | Separately fitted calibration and novelty policy, not an extra generic “unknown” class |

Retain the compact model as the CPU cost/accuracy comparator. Do not assume that the residual
model wins once latency and unknown false accepts are considered.

### Ordered experiments

1. **Diagnose existing models without retraining.** Confusion matrices, mistakes by group,
   source, band and activity status; embedding distance distributions; class margins;
   prototype coverage; calibration reliability; error examples from development data.
   Compare eligible-only and all-input metrics and count no-decision outcomes.
2. **Data-only improvement.** Retrain both reference architectures on the expanded,
   reviewed corpus with the same preprocessing. Use class/group/recording-balanced batches.
   Ensure metric-learning batches contain genuinely independent positive groups where
   available; do not treat augmented copies as independent aircraft.
3. **Temporal evidence.** Compare one-window predictions with bounded aggregation across
   consecutive, compatible observations. Use a fixed latency budget, not an entire recording.
   Reset on retune/gap/session changes and do not merge co-channel emitters by assumption.
4. **Optional multiresolution candidate.** Only if error analysis supports it, compare a
   second STFT resolution from the same IQ interval. A concrete candidate uses a shared
   residual encoder on separately normalized 256-point and 512-point STFT views, adaptive
   pooling to 2×4, two 128-value embeddings, concatenation and a 256→128 fusion projection,
   then the cosine head. This requires a new shape/preprocessing contract; the current
   fixed-shape engine cannot consume it unchanged. Benchmark added cost before selection.
5. **Optional role/background head.** Train only after trustworthy drone/non-drone and
   emitter-role labels exist. Mask missing task labels rather than inventing negatives.
   Leave this head absent if the dataset cannot support it.

Keep these as controlled comparisons; do not change data, architecture, augmentation and
loss simultaneously. Compare at least three seeds for finalists. Select using grouped
development performance, novelty discrimination and latency, not final-test outcomes.
Do not choose a transformer or a larger network merely because it is larger.

This iteration remains an STFT model. Existing cyclostationary evidence may nominate
candidate intervals and appear alongside model results. It does not silently become a model
feature or a ground-truth identity label. Measure the candidate gate's misses separately.

## 5. Probability, rejection and final evaluation

Fit probability calibration on held-out groups, for example scalar temperature scaling of
logits. Check reliability, Brier score and calibration error. A calibrated distribution over
known labels is conditional on that label set; it is not automatically the probability that
the received signal is a drone or that its top prediction is correct out of distribution.

Compare rejection using maximum score, top-two margin, energy and class embedding distance.
Fit prototypes/distributions from training embeddings only. Select policy on independent
calibration groups, including multiple withheld aircraft classes and verified non-drone RF.
Use leave-one-model-out development folds where feasible to avoid tuning to Mini 3 Pro alone.
Do not multiply correlated scores or display `1 − max_score` as an unknown-drone probability.

Outputs must distinguish:

- Accepted named-model candidate within validated coverage.
- Unrecognized RF / outside supported classes.
- Drone suspected, model unknown: only when separate qualified drone evidence supports it.
- Insufficient evidence: gaps, weak/unusable data, unsupported input or mixed evidence.
- Experimental candidate: model produced scores but identification policy/coverage is unavailable.

If related models remain inseparable, prefer a validated family-level result or abstention
over a forced exact name. A family mapping must be documented and empirically evaluated.

Pre-register operating targets before final testing. Proposed engineering targets are
at least 95% recording/event macro recall for release-supported known classes and at most
1% unknown/non-drone false acceptance, measured separately. These are goals, not achieved
results. Report per-class coverage, abstention and precision so rejecting most inputs
cannot manufacture apparent success. Revise targets explicitly if deployment needs differ.

Report confidence intervals at the independent encounter/unit level, not by treating
correlated windows as independent trials. Even zero errors in a small test cannot establish
a 1% false-accept bound (roughly 300 independent trials are needed for a one-sided 95% bound
near 1% with zero failures). If evidence is insufficient, mark the target unverified.

Freeze architecture, preprocessing, checkpoint and policy before the final test. Report
per-class confusion, macro metrics, unknown false acceptance, verified background false
alarms, rejection/coverage, and p50/p95/p99 end-to-end latency. Preserve failed results.
Do not tune on final tests or train on their samples to claim improved validation.

## 6. Reuse existing acquisition, isolate optional inference

Planned flow:

`existing USRP acquisition workers → bounded copied-IQ subscription → optional ML worker
→ preprocessing → model → calibrated decision → immutable result snapshot → new tab`

The Python `InferenceEngine` already loads a checkpoint once and accepts IQ or compatible
FFT arrays. Live acquisition subscription, continuous-window assembly, IPC and GUI result
presentation remain to be implemented. Initial integration keeps Python/PyTorch in a
separate process; C++ owns the small adapter and native UI. ONNX/C++ inference is a later
option only after numerical parity and runtime measurements.

Implementation constraints:

- Reuse existing acquired IQ and capture metadata. No second USRP handle, new tuner,
  changed scan schedule, sample-rate restriction or additional capture command.
- Use a dedicated bounded inference queue. Do not run the CNN in the scanner, existing
  decoder/cyclostationary worker thread, or rendering thread.
- Preserve existing submit order, budgets and outputs. Make an optional nonblocking,
  bounded copy/ownership transfer; report queue drops rather than blocking acquisition.
- Avoid coupling ML enablement to the cyclostationary enable switch. Both consume the
  same existing acquisition source independently where the source contract permits it.
- A capture must supply the required continuous interval. Existing cyclostationary
  `ShadowWorker` slots hold at most 262,144 samples; an 8 ms interval at 100 MSps needs
  800,000 samples. Therefore its sampled buffers cannot be assumed sufficient.
  Inspect original capture boundaries and continuity metadata before choosing the tap.
- Assemble across chunks only with verified continuous sample indices, matching rate,
  centre, clock/session and acquisition epoch. Never join across scan dwells, dropped
  samples or retunes. If existing captures cannot provide an interval, report
  “waiting for compatible data” without extending dwell or altering acquisition.
- Preserve capture ID, receiver/session ID, channel/centre, sample rate, sample range,
  clock/timestamp provenance, continuity and quality flags, model/policy/preprocessing
  versions and result completion time. Distinguish receive centre from selected model view.
- Compatible FFT sequences are optional. Screen spectra, single/averaged FFTs and
  arbitrary FPGA FFTs do not satisfy the existing normalization/filtering contract.
- On stop, invalidate queued/in-flight results by inference generation; stop only the
  optional worker. On source retune/restart, expire incompatible results. Keep old rows
  visibly historical rather than presenting them as live.
- Bound worker CPU, memory, queue and IPC size; detect timeout/crash and permit restart.
  A missing ML environment/model must not prevent the main application from starting.

Measure sampling coverage and model throughput separately from receiver sample rate.
Training speed does not establish inference speed. No claim of processing every sample
or real-time identification is permitted before an on/off load comparison.

## 7. Model Inference tab specification

Add a separate top-level native tab. Existing tabs, controls and default behavior remain.
Opening the tab performs no receiver or inference action. Inference is off by default.

### Controls and states

- **Start Inference**: validate the selected model bundle, start/load only the optional
  worker, then subscribe to existing acquired data. It does not connect or retune the USRP.
- **Stop Inference**: stop subscription and invalidate pending results; monitoring continues.
- Model selector and mode badge: **Experimental** or **Validated for listed coverage**.
  Model changes require stopped inference or an explicit atomic reload with a new generation.
- Source selector: existing compatible active capture stream only. Display receiver/band
  details; link to existing monitoring setup if disconnected instead of duplicating controls.
- States: disabled, loading, waiting for source, waiting for compatible data, running,
  overloaded/dropping optional work, stale, stopped and failed with a readable reason.
- Clear display history affects only this tab. Export bounded result metadata on explicit
  action; no automatic raw-IQ recording or changes to existing persistence.

### Results and key details

| Region | Required information |
|---|---|
| Current result | Decision state, accepted name if any, separate top candidate, age and coverage badge |
| Top candidates | Top three labels and numeric bars; show full class distribution on expansion |
| Probability explanation | “Uncalibrated model score” now; “Calibrated known-class probability” only with a supported calibration bundle |
| Rejection detail | Margin/distance/policy outcome and reason; no invented unknown probability |
| Capture | Receiver, capture/session ID, RF centre/channel, selected view, native/model rates, interval duration, continuity and activity quality |
| Runtime | Queue depth/capacity, processed/dropped/invalid/stale counts, inference and total latency, result age, sampled coverage, worker health |
| Model card | Checkpoint/version/hash, architecture, class list, training-data version, preprocessing, calibration version, validated class/domain coverage |
| History | Time, source/capture, decision, candidate, score type/value, rejection reason, latency and freshness; bounded retention |

Never display the top class as accepted when `accepted_class` is null. Distinguish a
missing probability from zero. Explain why unvalidated classes remain experimental even
when included in the checkpoint. History rows represent observations, not counted drones.

Existing Wi-Fi identities, security alerts, link-identification decisions and drone
broadcast identities are unchanged; model predictions do not overwrite or silently merge
with them. Any later evidence fusion is a separate scoped change.

## 8. Milestones and completion evidence

| Milestone | Work | Exit evidence |
|---|---|---|
| M0 — diagnosis | Reproduce current evaluation; inspect errors, novelty scores and deployment-compatible capture duration | Versioned diagnostic report and explicit bottlenecks; existing checkpoints preserved |
| M1 — expanded corpus | Review all available sources; resolve formats/roles; improve representative sampling and independent splits | Coverage manifest, duplicate/leakage audit, source-preservation checks; unsupported labels excluded |
| M2 — model comparison | Data-only residual/compact retraining, then justified temporal/multiresolution ablations | Reproducible recipes, per-group metrics, seed variation, measured latency; selected model with rationale |
| M3 — calibration | Known probabilities, novelty thresholds, class coverage and background/unknown evaluation | Frozen calibration bundle and coverage/rejection curves; no unsupported exact-name acceptance |
| M4 — qualification | Independently assess frozen model/policy; package inference inputs and outputs | Final report, checkpoint+policy+preprocessing hashes and honest release/experimental status |
| M5 — optional data adapter | Subscribe to existing capture workers; continuity assembly, bounded IPC, lifecycle and crash recovery | IQ/FFT parity, gap/retune/stale tests, bounded resource use; current workflows unaffected |
| M6 — new tab | Start/Stop, results/probability labelling, capture/runtime/model details and bounded history | GUI tests for each state; no acquisition side effects from navigation; existing tabs preserved |
| M7 — live qualification | Replay load tests first; controlled live USRP and Mini 2 tests when available | Before/after acquisition-loss, scanner timing, decoder/security parity and UI latency measurements |

For M5–M7 establish an implementation baseline from the current working tree, including
uncommitted work. Test worker absence, crash, queue saturation, malformed input, retune,
receiver reconnect and user stop. Existing outputs on deterministic replay must match.
Suggested initial performance budget: no added acquisition loss and no more than 5%
regression in scanner/UI latency under a repeatable workload, subject to measurement noise
and explicit adjustment before qualification. If exceeded, reduce optional inference duty
or keep it disabled; do not silently reduce existing processing.

M4 may conclude “experimental, insufficient evidence” for some classes. Integration may
display those candidates honestly, but does not convert them into validated identities.
Do not declare this segment fully qualified while required data or live checks remain.

## 9. Deliverables and next action

Deliverables are a reviewed corpus manifest, experiment/ablation reports, selected model
weights, a versioned preprocessing and calibrated rejection bundle, reusable inference
engine, optional acquisition adapter, and the new native tab with operator documentation.

The next implementation task is M0 followed by M1. Preserve the current trained weights
as baselines. Do not launch another larger training run until data roles, splits, sampling
and success metrics are documented. This plan edit itself changes no application code,
dataset files, checkpoint, acceptance policy or running process.
