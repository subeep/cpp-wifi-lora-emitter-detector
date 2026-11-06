# Phantom wideband grid and cross-recording alignment

The 37-symbol PHA_0100_01.dat burst has wideband cyclic-prefix coherence of 0.975. A 4096-bin transform at 44.8 MSps preserves the proposed 10.9375 kHz raster across the receiver passband. The reference's broadest continuous power region within 10 dB of its smoothed peak spans 858 bins (9.38 MHz). This support estimate depends on the signal, receiver filtering and cutoff. Its absolute subcarrier numbering remains unresolved.

One train of at least 12 symbols per eligible filename was selected by length and then prefix coherence. This gave the reference plus 32 other previously inspected recordings. All reconstructed grids originate directly from the parent IQ at the stored source offsets. Each grid uses the source timing hypothesis, rational resampling, a 512-sample prefix, fractional offset correction inferred from that prefix, and no channel equalization. Original source coverage is limited by the recording passband. Full complex reference bins and pilot candidate indices are in reference.npz.

| Test on the 32 other recordings | Observation |
|---|---|
| Wideband cyclic-prefix coherence | Median 0.960, range 0.783–0.987 |
| Integer translation selected on even symbols | Exactly reproduced as the best shift on odd symbols in 1 recording |
| Fine spectral-shape correlation at the selected translation | Median 0.130 on fit symbols, 0.045 on recheck |
| Rank of the 16 fixed pilot candidates in target odd symbols | Median 51.9th percentile |
| Moving boosted pilot comb | Median second-half excess 0.14 dB; positive in 19 of 32 files |

The one exactly reproduced translation was zero bins on PHA_0000_00.dat, with correlations of 0.328 and 0.255. Common receive-chain features can contribute to zero-shift matches. The moving-comb experiment searched spacings 3, 4, 6, 8, 12 and 24 with linear bin advance. The strongest fit used spacing 24 and advance 20 modulo 24. Its excess fell from 0.96 dB to 0.15 dB across halves of the reference. Spacing and advance were frozen across files while starting phase was estimated on each target's first half. Frequency translations used both temporal halves' even symbols, so the moving-comb split tests phase extrapolation only. Unequalized fixed BPSK pilots and linearly moving boosted combs cover a limited set of pilot designs.

The experiment supports symbol timing in these selected trains. The frequency alignment and pilot results do not establish a common frame structure. Decoding still requires verified subcarrier mapping, demodulation, framing, error correction and independent packet integrity. No telemetry fields were recovered. Distinct filenames may share aircraft, conditions and receiver. These development results do not measure identification accuracy.

## Reproduction

Run with the workspace ML Python environment from the repository root, using a fresh output directory.

    python -m tools.lightbridge_wide_grid data/validation/lightbridge-full-2026-11-06-b/trains.json OUTPUT
    python -m tools.lightbridge_pilot_comb OUTPUT

Run outputs are in data/validation/lightbridge-wide-grid-v1. Synthetic checks recovered an imposed 317-bin translation and a known moving comb with spacing 12, advance 3 and phase 2.
