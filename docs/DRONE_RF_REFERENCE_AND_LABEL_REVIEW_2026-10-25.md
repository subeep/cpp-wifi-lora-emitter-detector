# Reference estimation and label review — 25 October 2026

The lower-tail reference experiment addresses a demonstrated failure of the temporal median, but it is not ready to replace the current localizer. It reduces fragmentation in a selected Tampere prefix while enlarging regions and worsening several synthetic boundary scores. No production model, GUI, receiver worker, inference settings or dataset original was changed. No new recognition-model training was started.

## What was implemented

`tools/drone_ml/rf_localizer_multiscale.py` now has an opt-in `reference_method='lower_tail'`. The default remains `median`. `localizer_audit.py --algorithm multiscale_lower_tail` selects the experimental configuration. It uses a per-frequency temporal 20th-percentile reference for the transient branch, with fast high/low thresholds of 10/6 dB. The integrated branch estimates its reference after power smoothing. Stable branches, bounded proposal budgets and separate stable/transient stitching remain in place. A stable region is explicitly ambiguous; neither branch decides drone presence or identity.

`tools/drone_ml/localizer_reference_study.py` freezes settings and code hashes, compares three algorithms, and uses one-to-one IoU matching. A large rectangle cannot count as two correctly localized bursts. The existing area-coverage metric is retained separately because containment can conceal poor boundaries.

This is a package of changes to reference estimation and transient thresholds, not a clean single-variable causal ablation. These results do not isolate the contribution of each change.

## Evidence for the failure mechanism

All 24 prepared 16 ms snippets were visually screened, with proposal overlays hidden. Eight were uniformly selected; the other sixteen were selected using prior proposals, so this is not an unbiased evaluation sample. Review notes are qualitative morphology observations, not independent packet or emitter annotations.

In the selected Tampere snippet, broad transmissions occupy most of a processing chunk. The temporal median therefore measures the transmission itself. In a visually selected +5 to +20 MHz diagnostic band, the median per-frequency Q50/Q20 ratio is about 45.3 dB. This is uncalibrated contrast, not SNR. The diagnostic uses the shifted review STFT grid and is not an exact reproduction of streaming frame alignment. Its selected band was never passed to the localizer.

![Tampere reference diagnostic](../data/drone_ml/2026-10-25/morphology_review_01/tampere_reference.png)

Leuven panels show broad blocks, narrow frequency-changing bursts and persistent lines. Tampere shows broad high-duty blocks, smaller bursts in gaps and fine structure within blocks. Local target-on and target-off captures both show broad and brief vertical activity. NIST Wi-Fi panels also contain repeated broad blocks and gaps. None of those visual shapes alone establishes drone identity. Fine structure inside a parent transmission must not automatically become a separate emitter or training example.

## Results and regressions

All real comparisons use only the already approved first 100 ms of each record. They are development diagnostics, with source/group dependencies; they are not independent recognition validation.

| Source | Median proposals | Lower-tail proposals | Median box union | Lower-tail box union |
|---|---:|---:|---:|---:|
| Leuven Mini 2 video | 49 | 121 | 8.62% | 14.77% |
| Leuven Mini 2 RC | 74 | 145 | 7.59% | 12.61% |
| Tampere Mavic Pro | 1,516 | 51 | 47.60% | 72.81% |
| Local linked target | 89 | 44 | 83.85% | 98.52% |
| Local target off | 290 | 72 | 81.87% | 99.03% |
| NIST 144700 | 87 | 65 | 86.10% | 91.22% |
| NIST 161700 | 61 | 35 | 45.24% | 50.68% |
| NIST 174600 | 94 | 47 | 79.24% | 89.17% |

Box union is the geometric union divided by the analysed time × sampled bandwidth. It is not occupied-energy fraction. No proposal-budget drops occurred. Tampere's largest box is a stable ambiguous envelope spanning the observation and about 53.1 MHz. Several transient boxes span approximately 93–103 MHz, far wider than the brightest roughly 20 MHz block in the reviewed panels. This is a substantive crop-quality problem despite the lower count. The local on/off union nearing 100% is another reason not to interpret proposal counts as presence.

All eight previously selected weak Mini 2 reference boxes retain at least 80% area coverage and proposed-view frequency containment. These selectively reported boxes include internal features and overlapping source material; this is neither detection recall nor eight independent successes.

Five fresh synthetic seeds (701, 709, 719, 727, 733) were evaluated with settings frozen in advance. Each duty-cycle recording contains two known Gaussian band-limited bursts. Results below count one-to-one matches at IoU ≥ 0.5, not merely overlapping area.

| Synthetic condition | References | Original baseline | Median multiscale | Lower-tail multiscale |
|---|---:|---:|---:|---:|
| 30% duty, +10 dB | 10 | 10 | 10 | 5 |
| 70% duty, +10 dB | 10 | 10 | 0 | 9 |
| 90% duty, +10 dB | 10 | 10 | 0 | 0 |
| Mixed signals, 0 dB | 15 | 0 | 14 | 9 |
| Mixed signals, −5 dB | 15 | 0 | 0 | 4 |
| Continuous wide, +10 dB | 5 | 5 | 5 | 5 |
| Short 31 µs, +20 dB | 5 | 0 | 0 | 0 |

The lower-tail candidate helps the 70% duty fixture, but at 90% occupancy even the 20th percentile can lie within signal. It also loses boundary accuracy at lower duty and 0 dB. All methods can cover the short 31 µs references without meeting IoU ≥ 0.5. These distinctions matter when choosing training crops.

Across five noise realizations, proposal counts were 105/6/7 for colored noise and 2/1/2 for white noise (baseline/median/lower-tail). Five colored-noise lower-tail proposals are stable ambiguous envelopes. A gain step still creates five proposals in each multiscale variant. These are RF proposal diagnostics, not drone false-alarm rates.

Lower-tail processing of 100 ms took approximately 1.65–1.71 s for Leuven, 1.92 s for Tampere, 0.30 s for local recordings and 0.44–0.46 s for NIST. These are single-run CPU localizer wall timings on this host, not full-system benchmarks; file checksums, model inference and live receiver transport are outside this timing. This implementation is not real-time at the tested rates.

## What the labels actually support

The [Tampere publisher record, version 1.0.1](https://zenodo.org/records/4264467) describes isolated anechoic-chamber recordings with the drone on a turntable and its controller nearby. The 2.4 GHz files are interleaved little-endian signed int16 IQ at 120 MSps, centered at 2.44 GHz. The local Mavic Pro file matches the published MD5 `607de5f2fc6327a3e7b11b7185e0fcc8`, in addition to its existing SHA-256 audit. Publisher split files must not be assumed temporally consecutive.

This supports whole-recording association with a Mavic Pro drone/controller system. It does not identify the transmitting endpoint of each region, label every crop positive, or prove independent physical-aircraft coverage. A future multiple-instance/recording-bag training contract can use that association; ordinary per-crop positive supervision cannot simply inherit it.

| Source | Supported scope | Unsupported promotion |
|---|---|---|
| Tampere Mavic Pro | Publisher-controlled recording of drone/controller system | Every ROI is a drone; endpoint identity; independent aircraft |
| Leuven Mini 2 | Existing archive/model context | Newly verified per-ROI identity |
| Local linked | User target/controller linked context | Every observed emission belongs to Mini 2 |
| Local target off | Specific target/controller off | No drones anywhere in scene |
| NIST indoor Wi-Fi | Publisher Wi-Fi collection context | Verified absence of all drones |

The sidecar records all 24 morphology reviews and keeps every per-region identity, verified absence and training-eligibility decision unset/false. No existing manifest was relabelled. There are zero newly admitted training labels in this step. Wi-Fi data is valuable as domain context and a hard-example source; confirmed negative supervision needs an appropriately supported label scope.

## Next implementation gates

1. **Separate parent envelopes from precise transient boundaries.** Keep broad/stable proposals as context. Estimate onset/offset from local edge contrast and refine frequency support without allowing weak skirts or internal features to join distinct emitters. Retain raw amplitude and bandwidth metadata alongside normalized crops.
2. **Estimate reference reliability.** A low quantile is not automatically noise. Detect insufficient quiet support, compare longer causal histories where continuity is established, and abstain from precise segmentation when reference quality is inadequate. Reset state across acquisition gaps or tuning/gain changes.
3. **Evaluate a bounded candidate against the same frozen tests.** Require one-to-one boundary accuracy, union inflation and proposal workload alongside containment. Include 30/70/90% duty, short bursts, gain steps, continuous emitters and noise. Keep new recording groups for later independent checks; do not repeatedly tune against final tests.
4. **Implement the label contract before corpus export.** Distinguish whole-recording bag labels, adjudicated region labels, unknown context and verified background. Preserve source hashes and collection groups. All windows from a source/session/physical-unit group stay together. Add explicit rejection tests for unsupported per-crop label inheritance.
5. **Train only after crop and label gates pass.** Compare full-band/context views against refined event crops under matching preprocessing. Use recording-level supervision where appropriate, and evaluate no-drone rejection, unknown-drone handling and known identity separately. Do not reuse old weights with silently changed crops. Fresh controlled local sessions remain necessary for deployment qualification.

The next immediate deliverable is reference-reliability and boundary refinement plus the label contract, not a larger classifier trained on these experimental boxes.

## Artifacts and verification

- [Synthetic report](../data/drone_ml/2026-10-25/reference_study_01/report.json) and [frozen settings](../data/drone_ml/2026-10-25/reference_study_01/frozen_settings.json).
- [Real comparison including bounding-box union](../data/drone_ml/2026-10-25/morphology_review_01/real_comparison.json).
- [Weak-reference comparison](../data/drone_ml/2026-10-25/lower_tail_comparison_01/comparison.json).
- [24 review annotations](../data/drone_ml/2026-10-25/morphology_review_01/review_annotations.json) and [source evidence](../data/drone_ml/2026-10-25/morphology_review_01/source_evidence.json).
- Review sheets: [1](../data/drone_ml/2026-10-25/morphology_review_01/sheet_1.png), [2](../data/drone_ml/2026-10-25/morphology_review_01/sheet_2.png), [3](../data/drone_ml/2026-10-25/morphology_review_01/sheet_3.png).
- [Integrity record](../data/drone_ml/2026-10-25/morphology_review_01/integrity.json): baseline localizer and both previous model checkpoints unchanged; all review tensor hashes verified; no reserved final-test IQ read; frozen experimental code hashes match.

**53 tests passed**, including five new tests for one-to-one matching, high-duty behavior, unknown zero-input handling and rejecting algorithm/reference mismatches before source access. The existing presence-training test uses a temporary synthetic fixture for one epoch; no production training occurred. Source read audits preserve file stat tuples and hashes. No live configuration was promoted.

## Follow-up implementation

The [boundary refinement and scoped-supervision report](DRONE_RF_BOUNDARIES_AND_SUPERVISION_2026-10-25.md) implements the next offline step, measures strong-burst improvements and weak-signal regressions, and exports one recording-level bag declaration under an explicit label contract. It does not change the frozen results above or promote a model.
