# Boundary refinement and scoped supervision — 25 October 2026

Implemented an offline energy-core refiner, local reference-quality checks and a separate scoped-label exporter. The refiner gives substantially tighter geometry on the reviewed Tampere bursts, but loses weak-signal coverage and cannot recover missing parent boundaries. It must remain an optional proposal path alongside the original regions and full-band context. It is not ready to replace production preprocessing or reduce live drone false alarms by itself.

No receiver access, live application changes, source-dataset edits, production training or model promotion occurred. Both existing model checkpoints remain unchanged.

## Implemented behavior

### Optional energy cores

`tools/drone_ml/rf_boundary_refinement.py` takes the completed lower-tail parent proposals and rereads bounded IQ intervals around transient parents. It does not use report annotations or source identity to choose boundaries.

- Native-rate 1024-point FFT with 512-sample hop; no resampling or image resizing.
- Up to 0.5 ms of surrounding context on each side, clipped to the analysed prefix.
- Frequency support within the parent is proposed from a temporal 90th-percentile power profile, retaining bins within 12 dB of the strongest profile bin. This selects strong energy, not all occupied bandwidth.
- Per-band local edge checks compare the 98th and 10th temporal power percentiles. They require at least 6 dB contrast and a contiguous run of at least four frames within 3 dB of the lower reference. These are heuristic support checks, not calibrated noise estimates.
- Temporal cores must exceed both the lower reference +6 dB and the upper reference −10 dB, with at least two consecutive frames. Their geometry is clipped to the original parent.
- Stable/ambiguous parents remain context only. Unsupported local references defer refinement. An empty core list never means no RF activity or no drone.
- At most eight frequency bands and eight cores per parent. Excessive fragmentation defers the entire parent; it does not select the first or loudest eight fragments.
- At most 256 transient-parent attempts, 4 million samples per read, 64 million samples reread per record, and 1,024 emitted cores. Deferrals and output-budget drops are recorded. No state persists between calls, so no history is carried across receiver gaps, tuning or gain changes.
- Parent lineage, original regions and the full-band fallback requirement are retained in the result. Presence, accepted identity and training eligibility remain unset/false.

The surrounding context comes from a completed recording. This prototype is not a causal streaming implementation; using it live would require buffering, delay accounting and gap-aware scheduling. Retaining fallback metadata does not implement a full-band classifier or scheduler.

### Scoped label contract

`tools/drone_ml/label_contract.py` validates a new manifest format and exports scoped training units, without reading IQ. It is separate from the existing trainer; it does not retrofit enforcement into legacy training paths.

It distinguishes:

| Scope | Permitted interpretation | Required evidence |
|---|---|---|
| Recording bag | Drone/controller system associated with a whole recording | Reviewed publisher-controlled recording evidence, matching source and exact target |
| Region | A specifically adjudicated time-frequency region | Matching region geometry, boundary review and region-level evidence |
| Controlled session | No-drone supervision over the declared recording/session | Reviewed controlled-session evidence explicitly confirming all-drone absence |
| Review only | Context with no training target | No label is inferred |

The exporter rejects source/group overlap between training and development, duplicate record IDs, unsupported/reserved partitions, changed evidence hashes, mismatched capture keys, and unsupported label scope. All partition checks precede evidence-file access. A recording-level identity cannot be inherited by a crop. Target/controller-off context does not satisfy verified absence. Accepted outputs use a new schema requiring a scope-aware trainer; conversion into legacy per-window labels is explicitly disallowed.

The contract validates consistency with curated evidence, not the truth or authenticity of the curator's assertions. Evidence hashes prevent silent changes; they do not make a claim independently true. Source checksums must be reverified before a future trainer reads IQ.

Applied to the eight currently reviewed source contexts, it accepted **one whole-recording Tampere Mavic Pro bag** and retained seven contexts without supervision. That bag refers to the complete publisher recording, not each reviewed 16 ms snippet or the analysed 100 ms prefix. Its collection group remains `unresolved_physical_unit:DJI_Mavic_Pro`; it does not establish independent aircraft coverage. No region labels, verified background labels or per-window training tensors were exported.

The bag evidence derives from the previously checked publisher record and matching file checksum, preserved in [the source-evidence review](../data/drone_ml/2026-10-25/morphology_review_01/source_evidence.json). This is one valid supervision unit for future scope-aware training, not a complete recognition corpus.

## Evaluation and limitations

### Synthetic geometry

The study used five seeds (809, 811, 821, 823, 827) across eleven known synthetic families: 55 cases. Seed 7 was used for development. The initial pass revealed severe fragmentation on busy real captures; a second pass added resource/fragmentation guards and replayed the same seeds and records. Therefore the final results are development replays, not an untouched evaluation set.

Numbers below count one-to-one matches with intersection-over-union ≥ 0.5. Parent proposals and refined cores are separate alternatives; the original parents remain available even when cores fail.

| Fixture | Reference bursts | Lower-tail parents | Refined cores |
|---|---:|---:|---:|
| 30% duty, +10 dB | 10 | 6 | 10 |
| 70% duty, +10 dB | 10 | 10 | 10 |
| 90% duty, +10 dB | 10 | 0 | 0 |
| Short 31 µs, +20 dB | 5 | 0 | 2 |
| Mixed signals, +10 dB | 15 | 8 | 5 |
| Mixed signals, 0 dB | 15 | 10 | 0 |
| Mixed signals, −5 dB | 15 | 5 | 0 |
| Continuous wide, +10 dB | 5 | 5 | 0 |

The main improvement is tighter boundaries for separable strong bursts. The 6 dB local contrast requirement deliberately refuses many weak signals. A strong relative spectral threshold can also discard weaker simultaneous components. Continuous signals are retained through ambiguous parent context, not reconstructed as confidently timed cores. At 90% duty the parent often lacks usable burst boundaries; a child clipped to that parent cannot recover the missing transmission extent.

Colored-noise cases emitted five parent proposals and zero cores. White-noise cases emitted one parent and one core. Gain-step cases emitted five parents and five cores. These are energy proposals, **not drone false-alarm measurements**; gain changes remain an unsolved ambiguity.

### Real-recording geometry

Each record uses the same approved first 100 ms as the preceding audits. No new dataset or reserved final-test IQ was used. “Union” means the geometric union of boxes divided by analysed time × sampled bandwidth. Smaller union can mean tighter crops or lost coverage; it is not accuracy.

| Source | Parents | Cores | Parent union | Core union | Fragmented parents deferred |
|---|---:|---:|---:|---:|---:|
| Leuven Mini 2 video | 121 | 51 | 14.77% | 4.13% | 0 |
| Leuven Mini 2 RC | 145 | 70 | 12.61% | 3.61% | 3 |
| Tampere Mavic Pro | 51 | 44 | 72.81% | 10.93% | 0 |
| Local linked target | 44 | 24 | 98.52% | 2.70% | 5 |
| Local target off | 72 | 8 | 99.03% | 8.71% | 9 |
| NIST 144700 | 65 | 62 | 91.22% | 10.19% | 13 |
| NIST 161700 | 35 | 27 | 50.68% | 6.10% | 3 |
| NIST 174600 | 47 | 72 | 89.17% | 18.64% | 7 |

Before the expansion guards, the local linked and target-off records produced 376 and 739 cores. The guarded results above are bounded by deferring complex parents, not by establishing their correct segmentation. No record-level core-budget drops occurred. Some parents yield several children, so core count can exceed parent count.

Six of eight selected weak Mini 2 report boxes retain ≥80% area coverage in the cores, versus eight in the retained parents. Two core coverages fall to approximately 75.8% and 58.2%. Some small internal-feature boxes still have very poor IoU despite containment. These references are selected, dependent and not per-emitter ground truth; no detection-recall claim follows.

![Boundary review](../data/drone_ml/2026-10-25/boundary_study_02/boundary_review.png)

The visual review compares the same numerical snippets: Tampere broad bursts become much tighter; the busy target-off window has no retained cores in this view after deferral; NIST still contains partial boxes within broader Wi-Fi activity. The blank core view is not an absence decision. Original context remains available in every case.

Refinement alone took about 0.62/0.72 s for the Leuven prefixes, 0.74 s for Tampere, 0.14–0.15 s for local prefixes and 0.15–0.33 s for NIST, each containing only 0.1 s of observation. This is additional CPU work after localization. It excludes full-file checksums, acquisition, model inference and rendering. The prototype is not real-time, and these single-run measurements are not a production benchmark.

## Recommended next work

1. Use the core and parent paths as complementary inputs in the next preprocessing comparison. Never train or infer solely from this core stage: its weak-signal and continuous-signal losses are measured failures.
2. Add a weak-signal boundary estimator with explicit uncertainty and evaluate it on independent recording groups. For high-duty traffic, investigate a longer reference history only where capture continuity is known. Treat gain changes and receiver state as inputs, not drone evidence.
3. Expand the scoped evidence inventory across the approved datasets. Keep unknown context, recording bags and adjudicated region labels separate. Verified background sessions and leave-drone-model-out evaluation remain necessary before any claim of no-drone/unknown-drone reliability.
4. Implement a scope-aware bag/context trainer only after the label inventory and group split audit are adequate. Compare it with event-crop inputs under identical splits and matched preprocessing. The exporter produced declarations, not a trained model or ready tensors.
5. Optimize/port only the successful bounded stages after geometry and recognition acceptance criteria are met. Then measure full acquisition-to-result latency and qualify live USRP/Mini 2 behavior separately.

## Reproduction and artifacts

From the repository root, using the existing runtime:

```bash
data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.boundary_experiment \
  --output /tmp/drone-boundary-replay-NEW \
  --reports data/drone_ml/2026-10-25/lower_tail_real_01/report.json \
            data/drone_ml/2026-10-25/lower_tail_nist_01/report.json

data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.label_contract \
  --manifest data/drone_ml/2026-10-25/supervision_inputs_01/contract.json \
  --output /tmp/drone-supervision-replay-NEW
```

Outputs must be new directories; existing experiments are never overwritten.

- [Frozen configuration and hashes](../data/drone_ml/2026-10-25/boundary_study_02/frozen_settings.json).
- [Synthetic results](../data/drone_ml/2026-10-25/boundary_study_02/synthetic.json), [real results](../data/drone_ml/2026-10-25/boundary_study_02/real.json), [weak-reference comparison](../data/drone_ml/2026-10-25/boundary_study_02/weak_reference_comparison.json).
- [Scope decisions](../data/drone_ml/2026-10-25/supervision_export_01/decisions.json) and [scoped units](../data/drone_ml/2026-10-25/supervision_export_01/scoped_units.json).
- [Verification](../data/drone_ml/2026-10-25/boundary_study_02/verification.json): **68 tests passed**, including 15 new boundary/contract tests. Source hashes and stat tuples were preserved; baseline localizer and both model hashes match prior records. The previous presence-training test uses one epoch of temporary synthetic data only.

The first pass is preserved in `boundary_study_01`, including a hash-verified snapshot of its original refiner. The second pass is the bounded candidate reported here. Neither was promoted into the live application.

## Follow-up context experiment

The [26 October context-reference report](DRONE_RF_CONTEXT_REFERENCE_2026-10-26.md) adds a separate weak/high-duty branch and tests reference percentile against window length. It improves synthetic 90%-duty geometry while retaining this strong-core path and its frozen results. Neither path is a validated drone-presence classifier.
