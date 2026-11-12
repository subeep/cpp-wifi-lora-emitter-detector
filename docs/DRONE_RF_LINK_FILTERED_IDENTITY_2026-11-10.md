# Identity training on link-present windows only

10 November 2026. **Diagnostic only**, on DRFF-R2 development data (train and calibration units; no locked unit read, no deployable weights). It asks two things: what the "link-absent" captures contain, and whether training a model name only where its link is measured present improves naming on unseen aircraft.

## 1. How often is the link in a training window?

The lag probe was run on every original DRFF window of the existing corpus (12,480 windows of 8 ms), inside the same 20 MSps frequency view the model sees. Share of activity-eligible windows showing the class's own symbol timing:

| Class | Training units | Calibration units |
|---|---|---|
| Air 2 (15 kHz) | 24–26% | 29% and 73% |
| Air 2S (15 kHz) | 67–82% | 76% and 94% |
| Mavic 3 (15 kHz) | 81–83% | — |
| Mini 5 Pro (15 kHz) | 69–72% | — |
| Mini 4 Pro (30 kHz) | 76–93% | 92% |
| Mini 3 Pro (15 kHz), unknown model | — | 72–74% |

Three quarters of the "Air 2" training windows show no 15 kHz link in the model's view.

## 2. What the link-absent windows contain

From twelve example spectrograms (`examples_link_vs_nolink.png`, rows: Air 2 without link, Air 2 with link, Air 2S without link, Air 2S with link) and spectrogram statistics on 150 windows per group:

- **Narrowband hopping bursts.** Two of three Air 2 examples show strong bursts about 1.5 MHz wide and under a millisecond long at changing frequencies, in a view 17.5 MHz away from the capture centre. That is what a control uplink looks like. The frequency-view selector picks the 20 MHz slice with the most power, so a strong burst pulls the view away from the video link. Link-absent Air 2 windows have a peak 67 dB above the median, against 18 dB for link-present ones.
- **Probe misses.** One "no link" Air 2 example plainly shows the same wideband frame as the link-present examples, with a continuous carrier line through it. A strong tone raises the autocorrelation at every lag, which hides the cyclic-prefix peak in the probe's ratio statistic. So the probe has false negatives when a strong narrowband signal is present.
- **Other signals.** Two Air 2S examples show a different structured wideband signal in a view 35 MHz from centre; one shows only a carrier line.

This is a small sample of examples, not a census.

## 3. Filtered versus unfiltered training

Five classes (Air 2, Air 2S, Mini 4 Pro, Mavic 3, Mini 5 Pro), compact CNN, identical units, sampler, epochs and three seeds. "Link-filtered" trains only on windows where the class's timing is present. Evaluation is on calibration aircraft, never trained.

Balanced window accuracy on unseen aircraft:

| Evaluation windows | Unfiltered | Link-filtered |
|---|---|---|
| Link present (248) | 0.625 (0.596–0.664) | 0.696 (0.665–0.725) |
| Link absent (132) | 0.859 (0.850–0.868) | 0.564 (0.462–0.654) |
| All (380) | 0.755 (0.739–0.774) | 0.749 (0.715–0.788) |

Recording votes on the 27 recordings with link-present windows: 17–19 correct unfiltered, 19–22 link-filtered.

Per unseen aircraft, link-present windows correct (three seeds):

| Aircraft | Unfiltered | Link-filtered |
|---|---|---|
| Air 2 unit 3 (36) | 15, 16, 16 | 17, 17, 18 |
| Air 2 unit 4 (22) | 4, 3, 10 | 10, 13, 14 |
| Air 2S unit 3 (93) | 62, 57, 59 | 60, 65, 73 |
| Air 2S unit 4 (31) | 4, 0, 16 | 28, 0, 0 |
| Mini 4 Pro unit 3 (33) | 33, 33, 32 | 33, 33, 31 |
| Mini 4 Pro unit 4 (33) | 32, 33, 30 | 33, 30, 32 |

## What this shows

1. **Filtering helps where it should, modestly:** about 7 points on link-present windows of unseen aircraft, with the seed ranges just separated. Naming is still only about 0.70 there.
2. **The remaining errors have two clear sources.** Mavic 3, a class with a single training aircraft, absorbs 148 of the 237 link-present errors summed over the three link-filtered runs (Air 2 and Air 2S called Mavic 3). And 58 Air 2S windows are called Mini 4 Pro, which symbol timing rules out, so the timing gate would remove those.
3. **Air 2S unit 4 is unstable, not different:** 28 of 31 with one seed and 0 of 31 with the other two. One aircraft with three recordings decides nothing; it shows the model has not learned a stable Air 2S.
4. **Correction to the timing-gate report.** I said the model's "Air 2" class was not the Air 2 link. That was too strong. The link-filtered model, which never saw a link-absent Air 2 window in training, still gets Air 2 unit 3's link-absent windows right (85–87 of 87). So those windows do carry Air 2 content: most likely the hopping bursts, which also occur inside link-present windows, plus links the probe missed. The link-absent windows are probably controller or uplink activity of the right system, not a context shortcut. That is not proven.
5. **The hopping bursts are a candidate for the missing controller-only evidence.** They are strong, distinctive and not detected by the symbol-timing probe.

## Consequences

- Fix the probe statistic so a strong tone cannot hide the cyclic-prefix peak, then re-run the no-drone false-flag check, since that changes the detector.
- Treat hopping-burst detection as its own evidence path, and test it on the 23 October controller-only captures.
- Single-aircraft classes such as Mavic 3 should not compete for a name; they are the largest source of wrong names here.
- More aircraft per model remains the limiting factor for naming. Label filtering alone moves accuracy from about 0.63 to 0.70.

## Limits

Six unseen aircraft, 27 link-present recordings, one publisher and band, all previously inspected development data. Three seeds. Link presence comes from the probe, which has the false negatives described above. The probe's flag factor of 4 is unchanged from its first use; no threshold was tuned for this experiment.

## Artifacts

- `data/drone_ml/2026-11-10/link_filtered_identity_01/window_timing.json` (per-window timing, model view and wideband), `measure_windows.py`, `examples_link_vs_nolink.png`, `train_01/results.json`.
- Code: `tools/drone_ml/link_filtered_identity.py`, `block_ratios()` in `tools/drone_ml/cp_lag_probe.py`.
