# Phantom symbol trains and pilot recurrence

On the 60 MSps DroneDetect Phantom 4 captures, the 91.43 µs useful-symbol and 11.43 µs cyclic-prefix hypothesis yielded 96 candidate trains in 85 of 200 previously selected partial-band excerpts, spanning 40 of 50 files. The 40 comparison excerpts from ten Parrot Disco and DJI Air 2S files yielded zero trains. The median train contained 14 consecutive symbols, with a range of 8–37 (0.82–3.81 ms). Multiple frequency regions can contain the same physical burst. These counts describe selected excerpts and the comparison files represent only two other aircraft types.

The earlier study required a fixed timing contrast across both halves of a 20 ms excerpt. This experiment used its estimated phase on every excerpt and looked for shorter runs of at least eight consecutive symbols. That difference accounts for the larger set of candidate files. Per symbol, normalized prefix coherence had to exceed 0.55 and exceed both off-phase controls by 0.25. These exploratory cutoffs need independent qualification. The runs include 24 in clean, 18 in Bluetooth, 27 in Wi-Fi and 27 in combined-interference recordings. Each result retains its file, source offset, frequency band, approximate first sample and symbol range.

## Cross-recording tests

For each of four frequency regions, the longest eligible clean train supplied a reference. The longest eligible train in each other matching file supplied one comparison, with starts at the first observed symbol of a selected run. Runs beginning at the excerpt edge were excluded from template comparison. A run can also start mid-frame if an earlier symbol fails the prefix threshold.

For a pilot hypothesis, the reference supplied 16 bins with relatively stable squared phase after a train-wide fractional carrier correction. A sign-changing BPSK pilot under stable channel conditions could exhibit this property. Target bins were ranked against eligible bins in the target file. For a preamble hypothesis, the first reference symbol was compared with the first and subsequent target symbols using normalized complex correlation maximized over circular timing shifts. Deep fades, frequency-selective channels, carrier-grid translations, burst timing shifts and moving pilots remain unresolved. Only 512 central bins of the partial-band transform were compared.

| Region centre, MHz | Other files | Median pilot percentile | Median onset similarity (0–1) | Median onset rank vs interior |
|---:|---:|---:|---:|---:|
| 2427.65625 | 20 | 48.0 | 0.104 | 0.361 |
| 2434.21875 | 17 | 49.5 | 0.118 | 0.889 |
| 2440.78125 | 18 | 48.1 | 0.009 | 0.646 |
| 2447.34375 | 21 | 49.6 | 0.113 | 0.600 |

Selected pilot positions ranked near the middle of the comparison distributions. Onset similarities peaked at 0.219 across comparisons. The elevated relative rank at 2434.21875 MHz warrants a follow-up, but the absolute matches were weak. The evidence supports isolating symbol trains for further analysis. A pilot map, frame boundary, error-checked payload and Lightbridge telemetry fields remain unverified.

## Reproduction

Run from the repository root with the project's drone ML Python environment.

    python -m tools.lightbridge_train_probe data/validation/lightbridge-full-2026-11-06-b NEW_OUTPUT.json
    python -m unittest tests.test_lightbridge_train_probe

The completed result is data/validation/lightbridge-full-2026-11-06-b/trains.json. Synthetic checks recover independently generated trains carrying known pilots and a repeated preamble, and reject a continuous tone using off-phase controls. The test excerpts were previously inspected during timing discovery. The comparisons are exploratory, and distinct files can share an aircraft or session.

A concrete next target is PHA_0100_01.dat, where adjacent regions each yielded a 37-symbol run. Use the recorded source offsets to check physical overlap before joining any observations. Recover the occupied carrier grid from the original wideband IQ, align integer carrier offsets across bursts, and then test moving pilots and several candidate symbols around the observed onset.
