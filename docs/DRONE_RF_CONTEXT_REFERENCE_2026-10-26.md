# Weak-signal and high-duty context reference — 26 October 2026

Implemented and tested a complementary context proposal path. It recovers the synthetic 90%-duty bursts that the previous paths failed to localize, and modestly improves combined weak-signal geometry. It does not solve 98%-duty traffic, narrow/short features or gain-change ambiguity. Real-recording proposals remain too broad to replace the strong-core crop path.

This completes the next offline reference experiment. No existing application functionality, production model, source label, original dataset or live configuration was changed. Artifacts remain under the experiment directory started on 25 October; this report was completed on 26 October.

## Implementation

`tools/drone_ml/rf_context_reference.py` implements a bounded, stateless branch with:

- Native 1024-point complex FFT, 512-sample hop, power pooling over two frames.
- Integrated power over nine pooled time cells and 33 frequency bins. The broader frequency average reduces random-noise variation but blurs narrow signals. Its frequency scale is about 0.64 MHz at 20 MSps and 3.87 MHz at 120 MSps; this is not a fixed-bandwidth representation.
- A per-frequency temporal 5th-percentile reference over each block, plus a common low-quantile bias correction estimated across frequencies. Without that correction, the low reference makes stationary noise look elevated. The correction assumes some frequencies are less temporally occupied; it is not receiver calibration.
- High/low contrast thresholds of 1.5/0.8 dB after normalization, connected-component support requirements and exclusion of the first/last four time cells as seeds. These contrasts are not physical SNR or probabilities.
- A local lower-tail support check requiring a contiguous run of at least four pooled cells within 0.5 dB of the region-band lower reference. Even supported lower tails are never labelled verified noise.
- Explicit declaration of observation segments. Reads never cross declared gaps; there is no cross-block stitching or persistent history. Missing continuity/range declarations defer processing before IQ access.
- A separate `sample_index_only` mode for exploratory analysis when physical continuity is unverified. It does not authorize wall-clock duration or live-history claims.
- At most 4 million samples per read, 128 proposals per block, 1,024 per record, 128 declared segments and 200 ms of requested observation. Budget drops and unprocessed tails are reported.
- Broad components spanning at least 85% of sampled bandwidth are flagged as common-mode ambiguous. They are retained as RF proposals, not classified as drones or confirmed receiver artifacts.

The default reference block is 20 ms. This consumes the completed block, including future samples relative to its beginning; it is an offline method requiring buffering and delay accounting for any later live implementation. The 20 ms reference window does **not** change the classifier's input duration, replace an 8 ms model frame, or establish packet length.

The output keeps drone presence and accepted identity unset, with training eligibility and live qualification false. The earlier parent and strong-core implementations are unchanged. Combining the three lists in this experiment measures available geometry, not a production scheduler, emitter tracker or deduplicated event stream.

## Testing the assumptions

`tools/drone_ml/context_reference_experiment.py` freezes settings and hashes before comparing a 2×2 design:

| Variant | Reference window | Temporal percentile |
|---|---:|---:|
| 10ms_q20 | 10 ms | 20th |
| 10ms_q5 | 10 ms | 5th |
| 20ms_q20 | 20 ms | 20th |
| 20ms_q5 | 20 ms | 5th |

All four share integration, thresholds and support checks. Five seeds (907, 919, 929, 937, 947) cover 13 synthetic conditions, for **65 paired cases**. Development used seed 7: an initial low-quantile method flooded stationary noise, common-bias correction reduced that problem, and broader frequency integration reduced residual noise proposals. Settings were frozen before these five seeds and the real-recording comparisons. These are new realizations of synthetic families, not independent field validation.

The added conditions include 98%-duty bursts and 90%-duty bursts shifted relative to processing boundaries. Reference geometry includes clipped bursts at observation edges. Matching uses one-to-one intersection-over-union ≥0.5; this allows partial boundary error and must not be read as exact packet segmentation.

### What actually caused the improvement

| Condition | References | 10 ms/Q20 | 10 ms/Q5 | 20 ms/Q20 | 20 ms/Q5 |
|---|---:|---:|---:|---:|---:|
| 90% duty, aligned | 10 | 0 | 10 | 0 | 10 |
| 90% duty, shifted | 25 | 0 | 25 | 0 | 25 |
| 98% duty | 20 | 0 | 0 | 0 | 0 |
| Mixed signals, 0 dB | 15 | 0 | 0 | 5 | 5 |
| Mixed signals, −5 dB | 15 | 0 | 0 | 5 | 5 |

These columns score only the new context branch. The 5th percentile drives the high-duty gain; extending the window alone does not. At 98% duty, insufficient quiet support still defeats both percentiles. For weak mixed signals, the 20 ms window contains the wide burst crossing the old 10 ms boundary. Because this branch deliberately does not stitch blocks, its advantage is partly less geometric fragmentation, not proof that longer context universally improves sensitivity.

The five weak matches at each SNR are all the wide burst, one per seed. The narrow transient and continuous narrow component are not precisely localized by this path. Wider frequency averaging trades their resolution for lower noise variance.

### Combined coverage against the existing two paths

The fair baseline below includes **both** the existing lower-tail parents and strong cores. The new column adds the context path. Lists are combined without suppressing old proposals, so unchanged or improved matching alone does not establish a better workload/false-alarm tradeoff.

| Condition | References | Prior parent + core | With new 20 ms/Q5 context |
|---|---:|---:|---:|
| 30% duty | 10 | 10 | 10 |
| 70% duty | 10 | 10 | 10 |
| 90% duty, aligned | 10 | 0 | 10 |
| 90% duty, shifted | 25 | 2 | 25 |
| 98% duty | 20 | 0 | 0 |
| Mixed signals, +10 dB | 15 | 10 | 10 |
| Mixed signals, 0 dB | 15 | 9 | 10 |
| Mixed signals, −5 dB | 15 | 4 | 5 |
| Continuous wide, +10 dB | 5 | 5 | 5 |
| Short 31 µs, +20 dB | 5 | 1 | 1 |

The high-duty improvement is substantial on these fixtures. The aggregate weak-signal improvement is modest. Continuous activity still depends on retained ambiguous parent context, and short-feature accuracy remains poor.

### Noise and receiver-state ambiguity

All four new-context variants produced zero proposals on the five white-noise and five colored-noise recordings. This is only 100 ms of observation per noise family and does not establish deployment false-alarm performance. The old parent/core paths remain, including their ambiguous/noise proposals.

The 20 ms/Q5 branch still produces five proposals across the five gain-step recordings. Common-mode flags describe those broad proposals; they do not reject them or establish their cause. Ordinary Wi-Fi also produces many real-recording proposals. A gain change, Wi-Fi transmission and drone transmission cannot be distinguished by an energy threshold alone.

## Real-recording review

The same eight approved prefixes were reused, each no longer than 100 ms at its declared sample rate. All source hashes and stat tuples were preserved. No additional dataset, reserved final-test IQ or live receiver was accessed. All real comparisons explicitly use sample-index analysis: the inherited metadata does not independently verify physical continuity.

| Source | New 20 ms/Q5 proposals | Box union | Context processing time |
|---|---:|---:|---:|
| Leuven Mini 2 video | 108 | 7.2% | 0.785 s |
| Leuven Mini 2 RC | 110 | 6.4% | 0.767 s |
| Tampere Mavic Pro | 49 | 60.4% | 1.048 s |
| Local linked target | 74 | 91.1% | 0.176 s |
| Local target off | 82 | 72.7% | 0.173 s |
| NIST 144700 | 72 | 73.0% | 0.231 s |
| NIST 161700 | 27 | 40.9% | 0.242 s |
| NIST 174600 | 37 | 78.6% | 0.250 s |

Box union is bounding-box area divided by analysed time × sampled bandwidth, not occupied energy or correctness. No proposal-budget drops occurred in these runs. Timing is a single-run CPU measurement for context localization and reads; it excludes full source checksums, acquisition, previous paths, model inference and rendering. It is slower than real time. Synthetic timing fields additionally include scoring work and should not be used as pure localizer benchmarks.

All eight previously selected weak Mini 2 reference boxes are fully contained by some new proposal, but IoU ranges from about 0.0019 to 0.7009. Several are small internal features swallowed by larger boxes. This is not eight correctly identified emitters or a recall measurement.

![Complementary context review](../data/drone_ml/2026-10-25/context_reference_01/context_review.png)

Visual inspection of the numerical review snippets confirms the tradeoff: strong-core crops are tighter; the context path includes weaker surrounding energy, DC-related features and wider spectral skirts. In Tampere, the new union is about 60.4%, versus 10.9% for the strong cores. The new path must not replace those cores merely because it finds high-duty synthetic activity. NIST proposals and the local target-off proposals are still RF activity, not inferred drone presence.

## Decision and next deliverable

Retain the new branch as an **offline complementary candidate**. Keep parent/full-band context for continuous or unresolved activity, strong cores for separable high-contrast bursts, and context proposals for some weak/high-duty activity. Do not force these paths to vote as though they were independent drone detectors; they use the same samples and related energy statistics.

The next work should move to a **versioned corpus and evidence expansion**, rather than repeatedly tuning this small localization set:

1. Expand the scoped evidence inventory across the approved datasets, preserving whole-recording bags, adjudicated regions and verified background as different supervision types. Audit collection/physical-unit groups before assigning splits.
2. Freeze a small preprocessing comparison with full-band/context inputs and optional strong-core views, recording how each example was selected. Measure proposal workload and missed coverage; do not silently convert recording labels into positive crops.
3. Assemble independent background and leave-drone-model-out development evaluations. No-drone false acceptance, unknown-drone rejection and known-model classification are separate measurements. The existing one accepted recording bag is not a sufficient training corpus.
4. Train only after the evidence and split checks pass. Treat the current live model's false drone claims as unresolved until presence/rejection evaluation supports a replacement. Live USRP/Mini 2 qualification and a bounded streaming implementation remain later gates.

The new tests explicitly cover gap boundaries, unverified continuity deferral, sample-index-only scope, high-duty geometry, a weak wideband fixture, stationary-noise fixtures, gain-change ambiguity, budgets, malformed/short inputs, gain scaling and absence of cross-call history.

## Artifacts and reproduction

- [Frozen settings and hashes](../data/drone_ml/2026-10-25/context_reference_01/frozen_settings.json).
- [65-case synthetic ablation](../data/drone_ml/2026-10-25/context_reference_01/synthetic.json).
- [Fair comparison with both prior paths](../data/drone_ml/2026-10-25/context_reference_01/prior_paths_comparison.json).
- [Real results](../data/drone_ml/2026-10-25/context_reference_01/real.json), [compact summary](../data/drone_ml/2026-10-25/context_reference_01/real_summary.json), [weak-reference comparison](../data/drone_ml/2026-10-25/context_reference_01/weak_reference_comparison.json).
- [Verification](../data/drone_ml/2026-10-25/context_reference_01/verification.json): **78 tests passed**, including 10 new tests; prior localizer/refinement/label modules and both model checkpoint hashes verified unchanged. The existing training unit test uses temporary synthetic data for one epoch; no production training was performed.

From the repository root:

```bash
data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.context_reference_experiment \
  --output /tmp/drone-context-reference-NEW \
  --reports data/drone_ml/2026-10-25/lower_tail_real_01/report.json \
            data/drone_ml/2026-10-25/lower_tail_nist_01/report.json
```

Use a new output directory. The original experiment and dataset files are not overwritten.

## Corpus follow-up

The [versioned corpus inventory](DRONE_RF_CORPUS_INVENTORY_2026-10-26.md) expands scoped supervision to 18 checksum-verified Tampere recording bags across ten models, preserves the frozen partitions and audits background/derivative limitations. Numerical corpus extraction and model training remain pending.
