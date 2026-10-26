# Multiscale RF localization and review preparation

25 October 2026, Asia/Kolkata. Offline continuation of the 24 October baseline.

## Decision

Keep the new multiscale path as an **experimental alternative**, not a replacement for live inference. It substantially improves the tested 0 dB synthetic coverage and reduces proposal counts on seven of eight diagnostic recordings. However, it increases fragmentation on Tampere, broadens some reference boundaries and costs more processing time. These results do not qualify a drone-presence detector or solve the current model's false alerts.

The work produced a separate implementation, paired experiments, source-preserving real-data audits and an **unlabelled 24-snippet review pack**. No model was trained on real data, no identity policy enabled, and no acquisition, GUI or live inference behavior changed.

## Implementation

`tools/drone_ml/rf_localizer_multiscale.py` retains the native 1024-point FFT / 512-sample hop and two-frame power pooling. It uses single-precision SciPy FFTs with one worker. Actual sample rates remain explicit; no upsampling or universal rate is imposed.

It has four evidence paths:

1. **Fast transient:** per-frequency power relative to the current chunk's temporal median, with 8/4 dB high/low contrast thresholds.
2. **Integrated transient:** a 9×9 time/frequency average of normalized power, renormalized against its temporal median; 2.5/0.8 dB thresholds. This reduces fragmentation of weak signals but broadens boundaries. Its seed cells exclude the first and last four time positions to avoid treating reflected boundary support as independent evidence.
3. **Stable detail:** the median spectral profile compared with a smooth spectral trend, retaining narrow persistent structure. Such structure can still be receiver interference or a non-drone emitter.
4. **Stable envelope:** broad sustained spectral excess, retained with an explicit background/receiver ambiguity flag. It is not automatically discarded as noise, accepted as an emitter, or interpreted as a drone.

The integrated support spans approximately 486.4 microseconds at 20 MSps and 97.28 microseconds at 100 MSps. This is a localization integration scale, not a chosen model observation duration or a guarantee of sensitivity. Physical resolution changes with native rate. The whole-chunk median is offline adaptive context; this is not a demonstrated causal streaming implementation.

Stable and transient evidence stay separate. Overlapping proposals from comparable scales can combine, while cross-chunk stitching requires a one-to-one match with compatible frequency center and width. It does not establish emitter identity. Limits are 256 regions per chunk and 4,096 per record, with explicit dropped counts. None of the final real-source runs hit those limits.

Short final tails of fewer than 16 pooled cells are folded into the preceding read without inventing samples or exceeding the four-million-sample read budget. A regression test covers a previously observed two-cell tail that otherwise created spurious stable features.

All outputs retain null drone presence and accepted identity, false training eligibility, and the requirement for a full-band fallback. The stored coarse spectral profiles support inspection; no fallback classifier or live scheduler was added. `rf_localizer.py` remains unchanged as the baseline, and the offline audit CLI still defaults to it.

## Synthetic comparison

Both algorithms see identical generated IQ. Settings were developed on seed 7; five previously examined seeds and five additional seeds were evaluated. The latter are new noise draws from the same generator families, **not independent receiver or field validation**. The final rerun uses the corrected tail handling and conservative stitching.

The following table uses the five additional seeds. Each condition contains approximately 0.10 seconds of analyzed IQ in total. SNR means added signal/noise PSD within occupied bins; these are Gaussian band-limited signals, not simulated drone protocols.

| Condition | Reference regions | Baseline: ≥80% reference-area coverage | Multiscale: ≥80% coverage | Baseline proposals | Multiscale proposals |
|---|---:|---:|---:|---:|---:|
| Three simultaneous/intermittent/persistent signals, +10 dB | 15 | 14 | 15 | 20 | 15 |
| Same pattern, 0 dB | 15 | 0 | 15 | 163 | 15 |
| Same pattern, −5 dB | 15 | 0 | 0 | 4 | 1 |
| Continuous broad signal, +10 dB | 5 | 5 | 5 | 5 | 5 |
| 31 microsecond burst at 100 MSps, +20 dB | 5 | 5 | 5 | 10 | 7 |
| White Gaussian noise only | 0 | N/A | N/A | 3 | 1 |
| Colored Gaussian noise only | 0 | N/A | N/A | 85 | 6 |
| Gaussian noise with a gain step | 0 | N/A | N/A | 8 | 5 |

Five of the six colored-noise proposals are retained stable-background ambiguities, one per recording; the remaining proposal is transient. This is a reduction in fragmentation and proposal load, not proof that shaped noise has been identified and rejected. Gain changes still look like RF activity and must remain distinct from drone evidence.

For 0 dB, 13/15 multiscale matches also reach rectangle IoU ≥0.5. For the short 31 microsecond cases, neither algorithm achieves IoU ≥0.5 despite coverage. The regions are too coarse/oversized to claim precise packet or internal-feature localization. At −5 dB, neither system sufficiently covers the injected regions.

On the previous five seeds, 0 dB coverage likewise improves from 0/15 to 15/15, and colored-noise proposals fall from 80 to 6. White-noise proposals worsen from 0 to 1 in that subset. This small regression is retained in the results. Neither test duration nor synthetic diversity supports an operational false-alert rate.

Complete evidence: `data/drone_ml/2026-10-25/localizer_synthetic_03/{frozen_settings,report,summary}.json`.

## Real-source results

The same initial approximately 100 ms of each admissible source was analyzed as in the baseline. Full source hashes were verified, including the NIST HDF5 file; stat tuples, including atime, were unchanged. Reserved final-test IQ was not accessed. Existing source overlap, collection grouping and continuity limitations remain in force.

**Candidate count is not false-alert count.** Unlabelled activity and non-drone traffic can legitimately produce proposals. Conversely, fewer proposals can result from missed signals or overly broad grouping. Both count and bounding-area coverage are therefore reported.

| Source | Baseline proposals | Final multiscale proposals | Baseline box-union coverage | Multiscale box-union coverage |
|---|---:|---:|---:|---:|
| Leuven Mini 2 video, 100 MSps | 489 | 49 | 8.81% | 8.62% |
| Leuven Mini 2 RC, 100 MSps | 480 | 74 | 8.14% | 7.59% |
| Tampere Mavic Pro 2G, 120 MSps | 712 | **1,516** | 38.36% | 47.60% |
| Local linked-target recording, 25 MSps | 539 | 89 | 93.10% | 83.85% |
| Local target-off recording, 25 MSps | 362 | 290 | 94.44% | 81.87% |
| NIST 144700, 30 MSps | 291 | 87 | 99.38% | 86.10% |
| NIST 161700, 30 MSps | 199 | 61 | 53.45% | 45.24% |
| NIST 174600, 30 MSps | 281 | 94 | 89.96% | 79.24% |

Box-union coverage is the exact union of the proposed time/frequency bounding rectangles divided by observed duration × native bandwidth. It is **not measured occupied spectrum, detector recall, or area that contains drones**. The NIST records share one conservative group and are not verified globally drone-free truth. The local target-off recording only has the narrower target-state context already recorded.

Tampere is a meaningful regression. Of its 1,516 final proposals, 833 come from the fast transient path alone, 526 from combined transient paths, 155 from integration alone, and two are stable spectral regions. Its uniformly sampled review image contains broad repeated bursts and internal vertical structure; the image alone does not prove which small proposals are signal features versus artifacts. Do not tune a source-specific threshold or relabel the extra proposals as false positives without review.

### Reference containment and boundary precision

All eight selected Leuven weak-reference boxes retain full rectangular coverage and frequency containment within their proposed views. These boxes were consulted only after the IQ-only run. They are not per-emitter ground truth and cannot establish independent drone recall.

The broad video reference IoUs change from 0.884/0.869 to **0.668/0.713**. Only three of eight final matches exceed IoU 0.5, versus four with the original baseline. Smoothing/grouping improves coverage of weak synthetic regions but reduces boundary precision in some strong real examples. Some tiny report boxes remain internal features of larger bursts; IoU against them is not a complete parent-transmission metric.

### Why earlier low counts were not accepted as the final result

Exploratory runs `_01` and `_02` are retained for traceability. An area audit showed that permissive joining could combine stable background and transient regions into nearly full-band, whole-record rectangles. That artificially lowered candidate counts. The final `_03` runs separate those paths and enforce conservative one-to-one stitching. The resulting higher Tampere count is disclosed rather than hidden by over-grouping.

The earlier review pack `_01` corresponds to those superseded proposals. Use **`localizer_review_pack_02`** for current review. Neither pack is training eligible.

### Runtime

The final CPU measurements were approximately 1.47 s and 1.42 s for the two 100 MSps Leuven prefixes, 2.09 s for Tampere, 0.257/0.270 s for the local recordings, and 0.40–0.42 s for NIST. Each input is about 0.10 s long. This path is not real-time on the measured CPU implementation.

These single measurements include reads, FFT, contrast estimation, connected components and stitching; they exclude source hashing, model execution and acquisition. Host contention and the different dates of baseline runs preclude a controlled speedup claim. The paired synthetic runs also show additional computation. GPU acceleration and optimized candidate processing remain unimplemented; transport bandwidth is not the limit measured here.

Saved final audit reports:

- `data/drone_ml/2026-10-25/localizer_real_03/report.json`
- `data/drone_ml/2026-10-25/localizer_nist_03/report.json`
- `data/drone_ml/2026-10-25/localizer_comparison_03/{comparison,real_summary}.json`

## Review pack for the next corpus stage

`tools/drone_ml/localizer_review_pack.py` prepares **24 numerical 16 ms snippets from eight diagnostic source records**. These represent five distinct files plus three NIST records in one HDF5 file, not eight independent experiments. The selection includes:

- Eight deterministic uniform-time samples, chosen independently of detector coordinates.
- A largest transient candidate context per record.
- A stable ambiguous context per record.

Each NPZ preserves a native 1024/512 STFT grid, frequency offsets and frame-center times. Each manifest entry includes source/hash/partition/group, source sample coordinates, snippet IQ hash, tensor hash, proposed regions, truncation indicators and separate empty fields for reviewed parent regions and internal features. Original source amplitudes are retained in the numerical STFT; display scaling in the contact sheet is per-panel and is not a training transform.

**The pack is unlabelled review material, not an approved supervised corpus.** Presence and identity labels remain null. Source publisher context is retained separately. Uniformly selected windows are not automatically negative. A visually recognizable burst is not evidence of a specific drone model. The 16 ms duration is a review convenience and does not decide the eventual model's temporal context.

![Uniform source windows for review](../data/drone_ml/2026-10-25/localizer_review_pack_02/uniform_review.png)

The eight uniform panels were visually inspected for morphology and rendering. Parent bursts, narrower regions, persistent lines and broad activity are visible, but no per-emitter identity or scene-absence label was assigned from appearance. The remaining snippets are available for detailed evidence review; the manifest deliberately remains `unreviewed` for annotation purposes.

Current pack: `data/drone_ml/2026-10-25/localizer_review_pack_02/manifest.json`.

## Verification and reproduction

**48 tests passed**, including ten new multiscale/review-pack tests. They cover weak simultaneous regions, persistent broad activity, unknown/zero-input behavior, gain scaling, chunk tails, read/record limits, source preservation, independent uniform selection, absent verified labels, output non-overwrite, reserved-source rejection and separation of transient/background regions during stitching. Existing presence tests use a temporary synthetic one-epoch fixture; no real-data training occurred.

The old model checkpoints and baseline localizer are hash-checked against previous evidence in `localizer_comparison_03/verification.json`. No code in `src/` or the live worker was changed. The unrelated existing `gui_plan.md` edit was preserved.

Run from the repository root, supplying fresh output paths:

```bash
data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.localizer_audit run \
  --algorithm multiscale --seconds .1 \
  --manifest data/drone_ml/2026-10-23/temporal_research_01/sources.json \
  --output data/drone_ml/2026-10-25/localizer_real_repeat

data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.localizer_comparison_experiment \
  --output data/drone_ml/2026-10-25/localizer_synthetic_repeat

data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.localizer_review_pack \
  --reports data/drone_ml/2026-10-25/localizer_real_03/report.json \
            data/drone_ml/2026-10-25/localizer_nist_03/report.json \
  --output data/drone_ml/2026-10-25/localizer_review_repeat
```

## Next work and remaining gates

1. Review parent transmissions separately from internal features, starting with Tampere fragmentation and mixed/target-off windows. Preserve unknown attribution and inspect uniform samples to avoid reviewing only successful proposals.
2. Refine strong-region boundaries and hierarchical region grouping without reconnecting unrelated activity. Compare lower-SNR integration carefully: −5 dB coverage is still inadequate, and gain changes and stable receiver response remain ambiguous.
3. Complete label scope, alias/lineage reconciliation and independent presence/calibration groups. Resolve FPV valid samples and gaps before admitting it. The current source-context and morphology review cannot replace these labels.
4. Freeze compatible numerical overview/detail/temporal inputs, then train the small temporal and separate-cascade baselines on admitted data. Use measured presence/identity/unknown tradeoffs; do not feed these new views directly into the existing checkpoint.
5. Optimize the winning pipeline and qualify the additive live consumer. Controlled receiver backgrounds and later Mini 2 sessions remain necessary for live performance claims.

The offline comparison and review-pack preparation are complete. Robust localization, a labelled training corpus, model training under the revised inputs, calibrated drone recognition and live qualification remain pending. The next offline review work needs no receiver connection.

## Follow-up reference and label review

The [reference experiment and 24-snippet label review](DRONE_RF_REFERENCE_AND_LABEL_REVIEW_2026-10-25.md) tests an opt-in lower-tail variant. It reduces Tampere fragmentation but enlarges boxes and regresses some boundary metrics; it is not a replacement recommendation. The median results above remain the original frozen experiment.
