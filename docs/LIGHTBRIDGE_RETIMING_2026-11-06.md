# Local timing recovery after the four-symbol anchor

The experiment used all 46 previously selected anchors in PHA_1000_02 and PHA_0000_01. It reproduced the fixed-grid baseline to absolute tolerance 0.001, then compared three boundary searches on identical wideband IQ, 846 carrier pairs and anchor-derived fractional frequency correction. Symbol zero through +3 and all preceding positions retain their original timing.

For each later symbol the search maximizes normalized prefix-to-tail correlation at lag 4096 on the 44.8 MSps view. A correction requires a maximum of at least 0.7 and an improvement of at least 0.02. Among positions within 0.005 of the maximum it selects the closest to the predicted boundary. The broad radius is smaller than half of the nominal 4608-sample symbol period.

| Search radius | Median prefix coherence, +9 through +12 | Positions passing timing controls, out of 184 | Cross-file carrier agreement, +4 through +7 | Agreement, +9 through +12 | Agreement, +13 through +16 |
|---|---:|---:|---:|---:|---:|
| Fixed | 0.474 | 81 | 0.1429 | 0.0465 | 0.0221 |
| ±128 samples | 0.474 | 81 | 0.1427 | 0.0465 | 0.0221 |
| ±2048 samples | 0.985 | 149 | 0.1430 | 0.0466 | 0.0248 |

The timing controls require prefix coherence above 0.7 and an advantage above 0.25 against both a displaced phase (+768 samples) and displaced lag (+64 samples). Over offsets +9 through +12 the broad search adjusted 75 of 184 positions in 25 anchors. Of those adjusted positions, 72 passed the controls. Median absolute adjustment among the 75 was 1,098 samples, approximately 24.5 microseconds. Positive and negative shifts occurred. Counts refer to correlated symbol observations within two recordings.

Local boundary recovery restored cyclic-prefix coherence while the cross-recording differential carrier agreement persisted near its fixed-grid value. This weakens the explanation that modest boundary error alone produced the earlier drop. Later symbols may carry changing data, different pilot patterns or a different transmission. The search selects each position independently. A high local peak can align another emitter or train, and does not prove a continuous packet, payload semantics or a checksum. The carrier statistic cancels common phase, uses one lag of adjacent carriers, and cannot resolve all possible modulations.

A synthetic displaced-prefix check recovered a known 1,100-sample boundary displacement within eight samples. The report JSON retains all timing shifts, prefix/control scores and per-offset cross-file scores. For subsequent symbol mapping use runs with consistent signed timing shifts, control-qualified prefixes and a common occupied carrier grid before interpreting symbol decisions.

Reproduce with the workspace ML Python runtime. Supply a new output filename.

    python -m tools.lightbridge_retime data/validation/lightbridge-anchor-context-2026-11-06 OUTPUT.json
