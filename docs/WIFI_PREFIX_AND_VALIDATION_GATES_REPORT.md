# Prefix structure, continuity and independent-validation gates

10 October 2026. This package completes the next offline software milestones. It does **not** complete accepted named-link identification. Mini 2 live testing, ML and Remote ID remain deferred. The production worker, receiver, live GUI labels and radio configuration are unchanged.

## Implemented signal checks

`marker_prefix.py` compares a 4 µs interval immediately before a candidate symbol with its corresponding tail at approximately 66.67 µs lag. It checks normalized complex coherence, both half-intervals, relative energy and two displaced-lag controls. The controls reject constant tones that otherwise correlate at every lag. Carrier offset contributes a constant phase and is not treated as transmitter identity.

Search is bounded to 33 starts and three lag choices, within at most 65,536 samples. Passing prefix candidates must pass the existing span-and-boundary test **at the same symbol start**. Results cannot borrow a good prefix from one alignment and a good span from another. All thresholds remain development policy. The additional timing search is recorded as part of the diagnostic; improved counts must not be interpreted as independent confirmation or a calibrated recall gain.

This establishes compatibility with prefix/tail repetition over the tested interval. It does not establish the exact total prefix length, protocol-exclusive content, complete payload structure, endpoint identity or an OcuSync generation. `family_accepted` remains false.

`assess_segmented_recurrence` requires complete marker support to fit one explicit acquisition segment. It never combines markers across segment boundaries, retunes or missing samples. Unknown continuity permits only apparent recurrence; a verified declaration additionally requires an audit reference. A reference string itself is not proof—the capture audit remains external. Current dataset manifests have no independently verified continuity, so none receives qualified recurrence.

## Results on the working corpus

All 23 existing derived intervals from Leuven, Tampere, DRFF-R2 and RFUAV were checked. All working-copy hashes were reverified; dataset originals were not reopened by this package.

| Recording context | Distinct eligible markers | Joint span and prefix support |
|---|---:|---:|
| Mini 2 video, two Leuven intervals | 10 | 8 |
| Matrice video, Leuven | 5 | 5 |
| Mavic Pro, two Tampere intervals | 10 | 10 |
| Mavic 3 Classic ascend, DRFF | 5 | 5 |
| Avata 2, RFUAV | 2 | 2 |
| Other selected contexts | 0 | Not evaluated |

Total: **30 of 32 candidate markers** pass joint structure checks. The refined common-start search supports one more initial Mini 2 marker and the second Avata down-chirp relative to the earlier independent span-start selection. This is a changed diagnostic search, not lowered coherence thresholds. Markers, crops and raw/filtered views do not constitute independent deployment encounters. No-match intervals are not drone-absence conclusions.

[Corpus structure summary](../data/wifi_drone_analysis/named_link_readiness_2026-10-10/structure_summary.json). Detailed per-record evidence is in the four `prefix_corpus_*_2026-10-10` directories.

## Robustness and software verification

The deterministic stress sweep covers 216 combinations of three slopes, 20/100/200 MSps, 0/+500 kHz carrier offset, ±50/0 ppm sampling-clock offset, 10/20 dB synthetic SNR, and optional 0.3-amplitude delayed echo. **178/216 passed**: 108/108 at 20 dB and 70/108 at 10 dB. Failures remain in the report; thresholds were not retuned to eliminate them.

The benchmark supplies the marker position and carrier estimate, so it is **conditional structure robustness**, not an end-to-end search, detector recall, full-search false-positive test or real receiver measurement. Approximate measured Python p95 was 13.9 ms per selected synthetic marker on this run; it does not qualify live throughput or the isolated C++ worker.

All 26 standalone tests passed. New tests cover missing/corrupt prefixes, continuous sweeps, tones, noise, five sample rates, carrier offsets, common-start association, malformed bounds, gaps/segments, and evaluation accounting. Syntax and whitespace checks passed.

[Stress cases and failures](../data/wifi_drone_analysis/marker_structure_stress_2026-10-10/summary.json).

## Independent evaluation tool

`evaluate_link_validation.py` reports named-label precision, a predeclared one-sided 95% Wilson lower bound, conditional and unconditional recall, accepted/unknown/ambiguous/unavailable coverage, and wrong known-label assignments to nonadvertised families. Eligible positives rejected as unavailable still count as conditional misses. No denominator produces `null`, never a fabricated zero or perfect score.

It requires sealed-test records, audited truth references, explicit independence, and a matching frozen pipeline digest. Multiple eligible rows in the same independence group are rejected: the evaluation must predeclare one group-level outcome instead of treating correlated crops as independent trials. Physical-unit and session diversity are reported. The proposed 99% precision, 95% precision lower bound, 95% conditional recall and three-unit/three-session starting targets are evaluated separately from release completion. Passing metrics cannot enable runtime acceptance; background exposure, receiver/live qualification and human audit remain separate gates.

Running it on the current 23 intervals correctly yields **zero eligible independent-test encounters**: all have already been used for development, and their labels do not supply audited link-mode truth. No accepted identity, precision or recall claim is produced. They cannot be relabelled as a sealed test after this exposure.

[Evaluation report](../data/wifi_drone_analysis/named_link_readiness_2026-10-10/evaluation.json).

## Completion ledger

| Work | Status |
|---|---|
| Prefix/tail and joint span diagnostic | Implemented and tested offline |
| Gap/segment-aware recurrence | Implemented and tested; received-source continuity remains unaudited |
| Synthetic channel/clock/carrier stress harness | Implemented; observed failures retained |
| Independent evaluation and accounting tools | Implemented and tested; eligible received test set missing |
| Named OcuSync-family acceptance | Pending independent mode truth, competing-link/full-search calibration and uniqueness evidence |
| OcuSync-version, Lightbridge and other proprietary signatures | Pending implementation backed by distinguishing reference evidence |
| Router/phone/drone attribution | Pending role evidence; Wi-Fi recognition alone cannot supply device category |
| Live integration of the new Python diagnostics | Pending bounded C++ implementation and live receiver/cadence qualification |
| Mini 2 live sessions and later model classifier | Deferred by user |

The pending identity and live gates cannot honestly be marked finished from these development recordings. The next offline work should focus on independently documented link modes and confusing waveforms, while retaining the current candidate-only live labels. More features on the same exposed recordings do not substitute for that validation.
