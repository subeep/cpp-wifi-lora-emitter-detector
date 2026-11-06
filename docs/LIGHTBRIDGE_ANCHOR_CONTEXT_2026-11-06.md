# Wideband structure around the four-symbol anchor

All 46 four-symbol matches in the two comparison Phantom recordings supplied full original-IQ neighborhoods spanning symbol offsets −8 through +16. Each file supplied 23 events. Minimum anchor separation exceeded the 25-symbol observation length in both files. All offsets are relative to the useful-symbol start of the first template. Packet boundaries remain unknown.

The analysis resamples nominal 60 MSps CF32 to 44.8 MSps, uses a 4096-point FFT and a 512-sample prefix, and estimates fractional carrier offset from the four anchor symbols. The score compares the phase difference between adjacent carriers to its value at symbol zero, then measures whether those relationships agree across events and files. This cancels common symbol phase and stationary channel phase. Timing drift, integer carrier shifts and a later loss of synchronization can still reduce agreement. Symbol zero has agreement 1 by construction and is excluded from interpretation.

846 adjacent-carrier pairs passed a power mask chosen on the Wi-Fi-labelled comparison recording. The mask selects pairs within the nominal receiver passband and 20 dB of the strongest supported anchor carrier after excluding the centre ±4 bins. The centre guard was added after an exploratory run showed persistent near-DC relationships. Of the retained pairs, 199 fall in the band used to find the original matches and 647 elsewhere. All results below use the guarded analysis.

| Relative symbol | Cross-file agreement | Agreement outside search band | Pairs passing recurrence cutoffs |
|---:|---:|---:|---:|
| −1 | 0.195 | 0.194 | 7 |
| +1 | 0.313 | 0.305 | 75 |
| +2 | 0.219 | 0.207 | 19 |
| +3 | 0.236 | 0.219 | 26 |
| +4 | 0.185 | 0.181 | 3 |
| +5 | 0.156 | 0.144 | 0 |
| +8 | 0.086 | 0.082 | 0 |
| +12 | 0.040 | 0.038 | 0 |

Scores are mean real phase agreement, uncalibrated. Exploratory pair selection requires mean phase-vector magnitude above 0.75 in the first file and projected agreement above 0.75 in the second. The JSON records the carrier indices for every offset. Each index refers to the lower bin of a pair, relative to the receiver centre at 10.9375 kHz spacing. These observations do not identify physical pilot or data carriers.

For each event, a template formed only from the other file scored offsets +4…+7 higher than +9…+12 in 23/23 Wi-Fi-labelled and 20/23 clean-labelled events. Late prefix coherence also fell. Requiring coherence above 0.7 at every symbol in both ranges retained 11 and 6 events respectively; the earlier range still scored higher in 11/11 and 4/6. This exploratory timing-qualified subset is small. Median energy at +12 stayed within 0.2 dB of the anchor in each file. The loss of repeated structure therefore cannot be explained solely by an obvious median power drop. Changing symbols, burst alignment shifts and timing loss remain plausible. The observations provide no exact header/payload boundary, recovered bits or checksum.

The anchors were selected for matching offsets 0…3. Agreement in that interval is conditioned on selection. Results elsewhere, especially outside the original search band, provide the more useful tests. All comparisons concern two development recordings; file labels alone cannot attribute a protocol. The next discriminating experiment is local symbol re-synchronization after +3, followed by a comparison of the timing-corrected carrier grids and candidate bit sequences across bursts.

Reproduce with the workspace ML Python runtime using the command below and a new output directory. The completed results are in data/validation/lightbridge-anchor-context-2026-11-06.

    python -m tools.lightbridge_anchor_grid data/validation/lightbridge-wide-grid-v1/sequence_check.json OUTPUT

Synthetic checks verified invariance to common symbol phase and stationary per-carrier phase, and the expected reference-symbol score.
