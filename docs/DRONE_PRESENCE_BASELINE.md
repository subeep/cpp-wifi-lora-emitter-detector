# Presence baseline and capture sampling

Implemented 22 October 2026. This extends the false-positive recovery work without changing
the receiver, security, decoder or cyclostationary functions. **No real-data presence model
has been trained or promoted yet.** The default recorder directory currently has no fresh
sessions. The initial review and readiness report are under
`data/drone_ml/2026-10-22/presence_baseline_01/`; readiness is false for missing reviewed data.

## Live sampling changes

Model Inference now has a stopped-state checkbox, **Sample across each capture**, enabled
by default. A capture supplies up to three disjoint 8 ms intervals spread over its beginning,
middle and end, instead of only the beginning. Each interval uses the unchanged trained
20 MSps / 256-point / 625-frame preprocessing contract and automatic frequency-view selection.
This samples more temporal positions; it does not guarantee the selected frequency view
contains a drone. Turning the checkbox off preserves first-window-only selection.

The acquisition callback still has one pending slot and one processing slot. Each slot's
existing 4-million-complex-sample budget is shared by the intervals. At higher actual rates,
the number of views drops to fit that budget; receiver sample rates are not capped by this
feature. At 20 MSps, a sufficiently long capture provides three 160,000-sample intervals.
At 200 MSps, the same budget permits two 1.6-million-sample intervals; at 500 MSps, one
4-million-sample interval. Short captures similarly produce fewer views without duplication.
No new queue or unbounded full-capture copy was introduced.

At most two capture jobs are submitted each second, with up to three inference calls per
job (six intervals/second maximum). Compute/copy work can therefore be up to three times
the previous single-window mode. Busy jobs are dropped rather than blocking acquisition.
Actual live throughput and on/off resource impact still require qualification. The copy-time,
job, interval, sample and drop counters remain visible. Per-model time and total queue time
are measured separately. Faster transport does not guarantee this inference cadence.

Every result, STFT and audit record carries the interval index/count and original sample
offset. Device time remains explicitly anchored to the capture's first sample; add
`offset_samples / sample_rate_hz` for the interval's position. The intervals are never
concatenated into one waveform, passed to the model as continuous context, or counted as
independent votes. Retuning/context invalidation discards remaining views from the old job.
Only the latest observation retains display pixels. Original IQ is unchanged.

This is a coverage improvement, **not evidence of a lower false-alarm rate**. A presence
classifier and qualified decision thresholds are still needed. Current live decisions
continue to withhold both presence and identity claims.

## Independent presence-training pipeline

`tools.drone_ml.presence_dataset` creates a review template from complete live-audit pairs,
checks readiness, and prepares STFT files only from accepted rows. It never converts
`background_observation`, `controller_only`, `mini2_linked_*` or model candidates into labels.

Each accepted row must have:

- `reviewed=true`, `label=background` or `drone`, written `label_evidence`, and the exact
  `verified_view_offset_hz`. Positive rows also identify the observed `drone_model`.
- A `collection_group` and written `independence_evidence`. All on/off recordings from the
  same collection remain together. Recorder restarts alone do not establish independence.
  One receiver application run or recorder session cannot be split into multiple purportedly
  independent groups. Start a genuinely independent collection, including a fresh receiver
  application run, for separate evaluation partitions.
- An explicit `partition`: `train`, `calibration`, `development`, `final_test`, or `excluded`.
  Uncertain rows remain excluded. Each of the first three partitions needs at least two
  reviewed collection groups per label. This minimum is a software guard, not adequate
  statistical evidence of broad field reliability.

Partition leakage checks cover collection group, receiver run, recorder session, metadata
path and IQ hash. Preparation verifies metadata/IQ hashes and the actual selected frequency
view. It rejects duplicate intervals, changed reviews, non-live/invalid-continuity provenance
and output locations inside source sessions. All input opens are read-only/no-atime.
Final-test IQ and metadata files are not opened by preparation; their supplied review
identifiers participate in leakage checks. Nothing is silently reassigned to training.

`tools.drone_ml.presence_train` trains a **separate compact binary CNN**, not the 21-model
identity checkpoint. Architecture: four convolution stages (16, 32, 64, 96 channels),
GroupNorm/SiLU, 4×4 adaptive pooling, 64-dimensional feature layer and a two-logit head
(`background`, `drone`). The feature extractor can later be evaluated against temporal or
IQ-correlation inputs, but those are not claimed as implemented here.

Normalization is fitted on training tensors only. Sampling balances label and collection
group, so long collections do not dominate. Tensor hashes are checked before and after
the experiment. CUDA is used with `--device auto` when supported by the selected Python
runtime, or explicitly with `--device cuda`; CPU-only runtimes remain CPU-only. The trainer
does not install packages, switch environments, access a radio or replace an existing model.

After training, calibration-group scores fit a **diagnostic** presence threshold using up
to 201 score quantiles plus endpoint candidates. Default targets are group-balanced
background-window acceptance ≤1% and drone-window recall ≥80%, both configurable experiment
settings. These are **not** mission acceptance criteria or a measured false-alert/hour
guarantee. If no threshold meets both targets, the diagnostic threshold stays unavailable;
rejecting everything cannot satisfy the recall target. Development metrics use the fitted
threshold without refitting it. A reference threshold curve is separately reported.

Reports include per-collection and per-observed-drone-model results, background acceptance,
drone recall, seed, runtime/device, parameter count, loss history and data/model fingerprints.
The saved checkpoint has a distinct presence schema, `live_qualified=false` and `policy=null`.
The existing identity engine rejects that schema. A synthetic training smoke test verifies
execution and finite weights; it is not an RF result and its temporary model is discarded.

## Commands after fresh collection and review

Run from the project directory and use new output paths. The first command only inventories
metadata and writes an excluded-by-default template; fill it after examining exact views.

```bash
data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.presence_dataset template \
  --sessions data/drone_ml/live_audits --output /tmp/presence-review.json

data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.presence_dataset check \
  --review /tmp/presence-review.json --output /tmp/presence-readiness.json

data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.presence_dataset prepare \
  --review /tmp/presence-review.json --output data/drone_ml/presence-reviewed-01

data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.presence_train \
  --corpus data/drone_ml/presence-reviewed-01/corpus.json \
  --output data/drone_ml/presence-model-01 --epochs 10 --device auto
```

## Still needed to reduce and demonstrate false alarms

1. Collect and review ordinary RF/background and drone-positive views across independent
   sessions, settings and receiver conditions. The sparse recorder is useful for learning
   examples; separate long continuous evaluation recordings are required for false alerts/hour.
2. Run the presence experiment and compare threshold trade-offs on untouched collection
   groups. Include more than Mini 2 before making general unknown-drone claims. Check that
   background rejection improves without unacceptable drone misses.
3. Evaluate longer contiguous context (roughly 100–500 ms as an experimental starting range)
   and bounded burst proposals against the dispersed-window baseline. This requires suitable
   continuous recordings, not stitching separated 8 ms views. No temporal model or temporal
   voting policy is deployed by this change.
4. Evaluate ZC/IQ correlation or cyclostationary features as optional extra evidence; no ZC
   detector or mandatory waveform gate is added here. Then calibrate identity rejection
   separately, with withheld unknown drone models and verified non-drone backgrounds.
5. Freeze the whole decision pipeline, run reserved final and live tests, and measure false
   alerts/hour, missed encounters and latency before enabling no-drone, unknown-drone or
   accepted-identity decisions. Preserve an insufficient/unclassified state for weak evidence.

## Verification

Synthetic tests cover readiness with absent data, review requirements, cross-partition
and false-independent-group leakage, IQ/view integrity, reserved-file exclusion, finite
binary training, deployment-schema rejection, altered tensors and infeasible thresholds.
C++ tests verify distinct beginning/middle/end samples, exact offsets and grouping,
copy budgets, single-window compatibility, STFT lifetime, queue/stop/retune behavior and
unchanged acquisition IQ. These checks validate software behavior, not RF detection accuracy.
