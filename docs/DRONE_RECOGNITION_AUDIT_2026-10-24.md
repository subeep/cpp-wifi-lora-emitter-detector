# Recognition audit and frequency-view experiment

24 October 2026. First execution of the revised recognition plan. All work was offline; application behavior, checkpoints, source datasets and frozen partitions were preserved.

## Result

**Frequency selection is a demonstrated input limitation on the selected Leuven reference examples, but recentering is not a standalone model fix.** The current five-center selector crops two broad regions and misses three other report regions. Proposal-centered views contain them, yet both existing residual models change from four Mini 2 top candidates to zero across the eight diagnostic contexts. New selection and model training must be developed together.

The label review also found acquisition-format problems in the proposed FPV background data and exposure of two reserved source plots. These need explicit handling before a larger training run.

## Completed source/split review

`tools/drone_ml/recognition_review.py` creates a separate audit using the existing frozen review and split hashes. It checks group/label agreement and duplicate member entries without repartitioning the original experiment.

- 517 existing source/member records, 42 conservative groups.
- 21 training classes. Only Air 2, Air 2S and Mini 4 Pro have existing independent calibration-unit groups; the other 18 remain training-only coverage.
- 77 report entries and 305 proposed energy regions. Twenty-eight entries match specific records in the existing review; 49 are unresolved against that admitted-record list. Unresolved does not mean corrupt or useless: many are equipment classes or previously excluded sources requiring a different label contract.
- `mavic3C` and `mavic3S` report pages match reserved unknown-model source records. The report plots have been seen, while their raw test IQ remains unopened. Exposure is recorded without moving either source into training or inventing a fresh sealed evaluation.
- Existing Leuven diagnostic prefixes overlap original sources already used in training. Their `development_only` designation permits diagnostics; it does not establish independent validation. Parent lineage is retained explicitly.
- NIST's 180 capture records remain development-only and unlabelled. No NIST data were relabelled or read as new training examples in this run.
- No verified presence/identity labels were created merely from filenames, plots, thresholds or collection context.

Saved outputs:

- `data/drone_ml/2026-10-24/recognition_review_01/review.json`: frozen records, group counts, report associations, FPV metadata/probes, NIST assignments, derivative lineage and evidence hashes.
- `data/drone_ml/2026-10-24/recognition_review_01/exclusion_ledger.json`: unresolved/excluded source reasons and format issues.
- `data/drone_ml/2026-10-24/recognition_review_01/summary.json`: compact counts.

This is a reviewed metadata/source manifest, not an approved supervised training corpus. It preserves source-context labels rather than converting them into per-emitter truth. M1 has an executable audit and an explicit ledger; source-label reconciliation and a new training split remain incomplete.

## FPV background candidate findings

All 48 session metadata CSVs were read from the ten original ZIP archives. Their small CSV members passed ZIP CRC checks, and every described block was checked for valid offset/count bounds under the complex-float32 interpretation used by the existing report. No archives were extracted or modified.

One bounded first block per session was read for a **format probe**, using that block's own declared sample count. These first blocks are at 100 MHz, not the drone bands. They cannot support a drone-band background-performance claim. IQ prefix hashes and unchanged source-stat tuples are recorded; the complete multi-gigabyte IQ member CRC/hash was not verified.

| Observed first-block format | Sessions | Declared duration at 20 MSps | Observed nonzero extent |
|---|---:|---:|---:|
| 262,144 complex samples, final 131,072 exactly zero | 33 | 13.1072 ms | 6.5536 ms |
| 65,536 complex samples, no trailing zero tail | 15 | 3.2768 ms | 3.2768 ms |

All ten source-declared target-absent sessions are in the first row. Their same-frequency timestamp spacing has medians around 6.55 ms, consistent with the shorter extent but not proof of exact device-clock continuity. This pattern supports a zero-padding/timing mismatch hypothesis. The audit does not silently trim, join, relabel or certify those captures.

Consequences:

1. An apparent 13 ms array is not evidence of 13 ms measured RF. Existing 8 ms preprocessing could include padding and learn the acquisition artifact as a background signature.
2. The ten target-absent sessions cover only two declared indoor environments. They are not ten proven independent environments; all related sweeps are conservatively grouped by environment in the review.
3. The fifteen shorter probes also do not supply a single 8 ms observation. Neither format can be blindly reshaped into 128 ms temporal examples.
4. Verify the original writer/format, actual valid-sample counts and gaps; then inspect relevant-band blocks. Eligible short snippets may support a separate detail/presence experiment with explicit missing-context masks. They cannot establish long-sequence temporal performance.
5. Target-transmitter absence is narrower than verified absence of all drone-associated RF. Preserve that label scope.

## Frequency-view experiment

`tools/drone_ml/frequency_view_audit.py` used the existing hash-verified 100 ms Leuven Mini 2 video and RC prefixes, with zero native source offset and matching archive-member lineage. It matched the report's eight region references to their original time/frequency coordinates. Raw protected partitions were rejected before IQ access.

For each region, all views came from the same real native 8 ms interval. The experiment compared:

- Current production selection: choose one of five candidate centers and resample to 20 MSps.
- 20 MSps centered on the report region, clipped to existing source guard constraints.
- Corresponding 25/50 MSps diagnostic views with appropriate FFT sizes.

Proposal-centered views use offline report coordinates. They are **not a working live detector**, and the report boxes are not verified drone/packet labels.

| View | Fully contained reference boxes | Mean geometric box coverage |
|---|---:|---:|
| Current production 20 MSps | 3 / 8 | 52.24% |
| Proposal-centered 20 MSps | 8 / 8 | 100% |
| Proposal-centered 25 MSps | 8 / 8 | 100% |
| Proposal-centered 50 MSps | 8 / 8 | 100% |

These are selected-reference containment measurements, not detection recall or classification accuracy. The eight boxes are a small, biased selection from two related source prefixes; they include narrow and wide components with unverified emitter attribution.

The two wide video regions are approximately 19.336 and 18.750 MHz by the report detector. Production chooses an offset of -35 MHz, while their reported centers are near -26.7 MHz. This contains only **59.1% and 58.9% of their geometric bandwidth**. A native event-spectrum calculation retains about 74.7% and 74.3% of power within the reference frequency boxes, respectively. That power includes whatever emitters/noise lie in the boxes and excludes resampling transition-loss analysis.

A centered 20 MSps view contains both nominal boxes, but its central 80% covers only approximately 82.7% and 85.3% of them. The centered 25 MSps view supplies more margin. This distinguishes coarse center selection from the separate question of filter margins; it does not establish a universal 25 MSps requirement.

![Same native intervals with three frequency-view choices](../data/drone_ml/2026-10-24/figures/mini2_frequency_views.png)

Numeric arrays and detailed measurements: `data/drone_ml/2026-10-24/frequency_views_01/`.

## Existing-model replay: why a preprocessing-only swap is unsafe

Both the previous and expanded residual checkpoints were replayed on production and proposal-centered **20 MSps / FFT 256 / hop 256 / 8 ms** inputs. Every replay STFT was verified byte-identical to the corresponding diagnostic array. Each checkpoint retained its own normalization and weights. Wider 25/50 MSps inputs were never sent through these existing models.

| Checkpoint | Current view: Mini 2 top candidate | Centered view: Mini 2 top candidate |
|---|---:|---:|
| Previous residual | 4 / 8 | 0 / 8 |
| Expanded residual | 4 / 8 | 0 / 8 |

With centered views, both models returned Mini 5 Pro three times, Air 2 twice, and insufficient evidence three times. Their activity proxy also changes with the input view. All accepted identities remain null through the existing unqualified policy.

This does **not** measure true accuracy: not every reference region has verified emitter identity, and sources overlap training. It shows that a better-contained signal is not automatically recognized by weights trained under a different selection distribution. Learned dependence on cropping/context is a hypothesis requiring controlled retraining, not a proven causal diagnosis from eight examples.

Replay results and a repeatable recipe are saved in `frequency_views_01/model_replay.json` and `replay_existing_models.py`.

## Validation

Eight new tests cover immutable split/label assignments, duplicate detection, protected report exposure, sweep bounds, zero-tail reporting, frequency geometry, exact production STFT parity, source preservation, output non-overwrite and reserved-IQ rejection before opening the source. Combined with existing public/temporal/presence tests, **31 tests passed**. The presence-training test uses a temporary synthetic corpus; no real-data model was trained.

The reviewed archive and IQ source-stat tuples were preserved. Existing checkpoints were hash-recorded and unchanged. No receiver was opened, no inference policy enabled, and no GUI/acquisition code modified.

## Next work, in order

Follow-up: the first offline IQ-only localizer and its synthetic/real-source diagnostics are now implemented. See [RF localization baseline](DRONE_RF_LOCALIZER_BASELINE_2026-10-24.md). It finds the selected reference regions but exposes weak-signal, colored-noise, proposal-load and runtime limitations; it has not been integrated into live inference.

1. Build an **offline** proposal/localization candidate that can find the relevant frequency region without using report coordinates. Test weak and continuous emissions as well as bursts; retain multi-region and full-band fallback coverage. Evaluate against separately reviewed regions, not its own generated boxes.
2. Freeze new preprocessing profiles and create a reviewed numerical corpus with full source lineage. Include both current-selection controls and corrected views; explicitly model truncated/partial signals rather than labelling them negative.
3. Resolve FPV padding/encoding and extract bounded relevant-band review blocks. Keep short observations separate; do not manufacture temporal continuity. Use source-supported negative context only within its stated scope.
4. Complete source/alias reconciliation and independent positive/negative/calibration groups. Preserve old reserved partitions and disclose report exposure when choosing fresh final groups.
5. Train the small temporal and separate-cascade baselines on the corrected, scoped corpus. Compare presence/identity performance at matched false-alert budgets before considering a larger network.

The view selection problem is now evidenced; it is not fixed in the live application. The proposed corpus and model remain unqualified until the label, continuity and validation gaps above are resolved.
