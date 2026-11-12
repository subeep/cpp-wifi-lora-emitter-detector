# T07 (first part) — cyclic-prefix lag probe on X310 data

10 November 2026. **Feasibility result, not a qualified detector.** Offline and read-only. It tests whether OFDM symbol timing alone separates the Air 3S link from ordinary Wi-Fi and Bluetooth on the deployment receiver.

## Result in one paragraph

The Air 3S video link, as recorded on 23 October, has a useful-symbol time of 33.3 µs, which is **30 kHz subcarrier spacing, not the 15 kHz the plan assumed**. Every capture with the link present was flagged at 30 kHz in 82–97% of its 6.6 ms blocks. In the no-drone data, 30 kHz was flagged in 0 of 494 blocks on 23 October and in 16 of 14,220 blocks (0.11%) today, and no single no-drone capture exceeded 6.7%. The 15 kHz hypothesis was flagged in none of the link captures.

## Method

- Tool: `tools/drone_ml/cp_lag_probe.py`. It computes the normalised autocorrelation magnitude against lag in blocks of 131,072 samples, and flags a block when the value at a hypothesis lag is more than 4 times the median of neighbouring lags. It also lists blind peaks with no hypothesis assumed.
- Hypotheses and the factor of 4 were fixed before any real data was run: Wi-Fi 312.5 kHz, 802.11ax 78.125 kHz, and LTE-like 15, 30 and 7.5 kHz.
- Tests (`tests/test_drone_ml_cp_lag_probe.py`, 3 passing): synthetic Wi-Fi timing flags only Wi-Fi; synthetic 30 kHz and 15 kHz signals resampled to 20 MSps (non-integer lags) are told apart; noise flags nothing.

## Data

| Set | Receiver | Scene, as recorded at the time | Captures |
|---|---|---|---|
| 23 Oct background | X310, 20 MSps | Air 3S and controller off (user confirmation) | 13 × 0.25 s, 13 centres |
| 23 Oct controller-only | X310, 20 MSps | Controller on, aircraft off | 13 × 0.25 s |
| 23 Oct connected | X310, 20 MSps | Aircraft and controller linked, live feed | 13 × 0.25 s, plus 2 × 1 s follow-up |
| `air3s_whole` | X310, 20 and 50 MSps | Air 3S, 2444 MHz; scene not recorded | 4 files, about 13 s |
| 10 Nov `bg-ambient-A` | X310, 20 MSps | Mixed non-drone, as stated by Sudeep; approval pending | 237 × 1 s, 9 centres |

## Captures flagged at 30 kHz

Fraction of blocks flagged, and the three strongest blind peaks (useful time in µs):

| Capture | Blocks | Flagged at 30 kHz | Blind peaks |
|---|---:|---:|---|
| connected, 2432 MHz | 38 | 0.82 | 142.8, 33.3, 126.0 |
| connected, 2452 MHz | 38 | 0.97 | 142.7, 33.3, 126.0 |
| connected follow-up, 2452 MHz | 152 | 0.88 | 142.7, 33.3, 126.0 |
| `air3s_whole` 2444 MHz, 20 MSps (two files) | 200 each | 0.91, 0.91 | 142.7, 33.3, 126.0 |
| `air3s_whole` 2444 MHz, 50 MSps (two files) | 95, 200 | 0.84, 0.87 | 142.7, 33.3, 126.0 |

No other capture in any 23 October set was flagged at 30 kHz: not the background, not controller-only, and not the connected captures at the other eleven centres.

## No-drone session, 10 November

| Centre (MHz) | Blocks | Wi-Fi 312.5 kHz | 15 kHz | 30 kHz | Captures with any 30 kHz flag (worst capture) |
|---:|---:|---:|---:|---:|---|
| 2401.0 | 1500 | 0.249 | 0.0000 | 0.0000 | 0 of 25 |
| 2413.5 | 1620 | 0.388 | 0.0006 | 0.0019 | 3 of 27 (0.017) |
| 2438.5 | 1560 | 0.276 | 0.0083 | 0.0019 | 2 of 26 (0.033) |
| 2463.5 | 1680 | 0.538 | 0.0024 | 0.0054 | 5 of 28 (0.067) |
| 5181.5 | 1680 | 0.314 | 0.0006 | 0.0006 | 1 of 28 (0.017) |
| 5221.5 | 1440 | 0.203 | 0.0007 | 0.0000 | 0 of 24 |
| 5746.5 – 5826.5 | 4740 | ≤ 0.003 | 0.0000 | 0.0000 | 0 of 79 |

## What this supports

1. Symbol timing separates this Air 3S link from the Wi-Fi and Bluetooth present in these recordings by a wide margin: at least 0.82 against at most 0.067 per capture.
2. The plan's 15 kHz example does not describe this link. 15 kHz is documented for DroneID bursts; the Air 3S video link measured here is 30 kHz. The structural detector needs a hypothesis bank built from measurements, not from one assumed spacing.
3. The peak at 142.7 µs equals four symbols of 35.7 µs, which would mean a cyclic prefix of about 2.3 µs. That matches the 30 kHz numerology used by 5G NR. This is an interpretation of the peaks, not a decoded fact; the 126.0 µs peak is not explained.

## What it does not show

- **One aircraft, one sitting.** All link captures are from 23 October, at 2.4 GHz, at high signal level (about −13 to −21 dBFS). Sensitivity at range or low signal is untested.
- **Background and link were recorded 18 days apart** for the larger sets. The 23 October background is same-day but small (3.25 s in total).
- **30 kHz is a family-level cue at best.** Other O4 aircraft probably share it, and other radio systems use 30 kHz spacing. It says nothing about model name.
- **Controller-only was not detected.** The controller uplink did not show at any hypothesis.
- **Short bursts are invisible to it.** A DroneID burst of well under a millisecond is diluted in a 6.6 ms block.
- **The per-capture comparison was made after seeing the data.** Only the hypotheses and the per-block factor of 4 were fixed in advance; any capture-level threshold still has to be set on calibration data and tested on sealed data.
- **Today's session is not yet an approved negative.** Its manifest lacks the antenna position and approval.

## Consequences for the plan

- T07: add measured hypotheses (30 kHz for this link) to the structural bank; `src/cyclostationary/ofdm_structure.cpp` currently holds Wi-Fi timings only. Fractional-sample lags must be supported, since 33.3 µs is 666.67 samples at 20 MSps.
- Measured timing groups (table below) should replace advertised family names as the family label in the manifest (T03).
- Paired Air 3S sessions on a background day remain the missing evidence for a same-conditions comparison.

## Public datasets: measured symbol timing per model

Run the same day on DRFF-R2 (non-locked units only: train, calibration and the Mini 3 Pro development units; 39 files, 16 blocks of 5.2 ms each, 100 MSps, 5745 MHz) and on all 19 Tampere recordings (40 blocks each). No locked test unit was read. "Flagged" is the fraction of blocks; ranges are across files.

| Model (source) | Advertised link | 15 kHz flagged | 30 kHz flagged | Strongest blind peaks (µs) | Measured timing group |
|---|---|---|---|---|---|
| Air 2, units 1–4 (DRFF) | OcuSync 2.0 | 0.00–0.94, 5 of 10 files ≥ 0.9 | 0.00 | 66.7 | 15 kHz only |
| Mavic Pro (Tampere) | OcuSync 1 | 1.00 | 0.00 | 66.7 | 15 kHz only |
| Mini 3 Pro, units 1–2 (DRFF) | O3 | 0.50–0.75 | 0.00, one file 0.38 | 66.7 | 15 kHz only |
| Mini 5 Pro, unit 1 (DRFF) | O4+ | 0.56–0.94 | 0.00–0.06 | 66.7 | 15 kHz only |
| Mavic 3, unit 1 (DRFF) | O3+ | 0.88–0.94 | 0.00, one file 0.94 | 66.7, 33.3 in one file | 15 kHz, sometimes 30 kHz |
| Air 2S, units 1–4 (DRFF) | O3 | 0.56–0.94 | 0.25–1.00 in all 10 files | 66.7 and 33.3 | 15 kHz and 30 kHz together |
| Mini 4 Pro, units 1–4 (DRFF) | O4 | 0.00 | 0.75–1.00 | 142.7, 33.3, 126.0 | 30 kHz, four-symbol pattern |
| Air 3S (local X310) | O4 | 0.00 | 0.82–0.97 | 142.7, 33.3, 126.0 | 30 kHz, four-symbol pattern |
| Inspire 2, Matrice 100/210, Phantom 4, Phantom 4 Pro+ 5 GHz (Tampere) | Lightbridge | 0.00 | ≤ 0.05 | 30.5, 60.9, 91.4 | 32.8 kHz (found blind; not in the hypothesis list) |
| Mavic Mini (Tampere) | Enhanced Wi-Fi | 0.00 | 0.00 | 12.8 (weak, 0.15 flagged) | 78 kHz, weak |
| Parrot Disco, Parrot Mambo video, Yuneec Typhoon H 5 GHz (Tampere) | Wi-Fi | 0.00 | 0.00 | 3.2 | Standard Wi-Fi timing |
| Yuneec Typhoon H 2.4 GHz, Phantom 4 Pro+ 2.4 GHz, Parrot Mambo control (Tampere) | — | 0.00 | 0.00 | none | No OFDM timing found |

What this adds:

1. **Symbol timing sorts these models into about five physical groups**, measured, with no training. Mini 4 Pro on a public receiver and your Air 3S on the X310 show the identical 30 kHz pattern, including the 142.7 and 126.0 µs peaks. That is the first evidence in this project of a link signature carrying across receivers.
2. **Advertised family names are not reliable waveform labels.** Mini 5 Pro (advertised O4+) measures 15 kHz here, unlike Mini 4 Pro. Air 2S and Mini 3 Pro (both advertised O3) differ: Air 2S shows 30 kHz in every file, Mini 3 Pro in one of five.
3. **That weakens the earlier same-family diagnostic.** Air 2S versus Mini 3 Pro was not a true same-waveform pair. I therefore ran a pair that does share measured timing, Air 2 versus Mini 3 Pro (both 15 kHz only), under the same protocol: balanced accuracy on unseen aircraft was 0.757 (0.649–0.829) in fold X and 0.875 (0.834–0.901) in fold Y. Models with the same symbol timing are still partly separable, and the spread across units and seeds is again large. In fold X, trained on receiver `u2` only, Air 2 unit 1 on receiver `u1` was recognised in 15–67% of windows, so receiver change is a second source of failure.
4. **Wi-Fi-link drones look like Wi-Fi** to this probe, as expected. They need decoder evidence.
5. **Lightbridge timing was found by the blind search**, not by a listed hypothesis, which supports building the bank from measurement.

Limits specific to this section: 16 blocks per DRFF file is a small sample; several Air 2 files showed little or no flag, so the link is not always present in a capture; a 30 kHz peak beside a 15 kHz one can come from signal structure (for example a comb pattern) and does not prove a second numerology; Tampere has one recording per model and band; KU Leuven, RFUAV and DroneDetect were not run (archive members, and DroneDetect is not yet format-verified).

## Probe revision 2: strong narrowband signals (same day)

The first statistic could miss a link when a strong tone was present in the same block: a tone adds the same amount at every lag and hides the cyclic-prefix peak. Revision 2 Hann-windows each block and caps every power-spectrum bin at 20 times the median of its 256-bin neighbourhood before forming the autocorrelation. The flag factor of 4 is unchanged. All results above this section are from revision 1.

| Check | Revision 1 | Revision 2 |
|---|---|---|
| Synthetic 30 kHz link under a tone 40 dB stronger | not flagged | flagged; tone alone flags nothing (new test) |
| X310 no-drone captures assigned a drone-link group | 0 of 263 | 0 of 263 |
| X310 captures containing the Air 3S link, assigned 30 kHz | 7 of 7 | 7 of 7 |
| No-drone session, 30 kHz flagged blocks | 16 of 14,220 (0.11%) | 19 of 14,220 (0.13%) |
| No-drone session, worst single capture at 30 kHz | 0.067 | 0.083 |
| No-drone session, 15 kHz flagged blocks | — | 16 of 14,220 (0.11%), worst capture 0.033 |
| No-drone session, 32.8 kHz flagged blocks | — | 8 of 14,220 (0.06%), worst capture 0.033 |
| DRFF calibration recordings with a link found | 23 of 36 | 24 of 36 |
| DRFF eligible windows with the class's link present: Air 2 training | 25.5% | 25.6% |
| Same: Air 2 calibration | 37.9% | 42.5% |
| Same: Air 2S calibration | 80.0% | 85.8% |
| Same: Mini 4 Pro calibration | 91.7% | 98.6% |
| Same: Mini 3 Pro | 72.9% | 79.6% |

Reading: the fix closes the tone blind spot and recovers a few percent of windows, at a slightly higher but still small no-drone flag rate. It does **not** change the main finding about Air 2: about three quarters of its training windows have no video link in the model's view, so tone masking was a minor cause. Mini 5 Pro moved in both directions (153 windows gained, 179 lost), which suggests its timing sits near the flag threshold.

Revision 2 results: `cp_lag_probe_01/bg_ambient_A_2026-11-10_v2.json`, `timing_gate_02/results.json`, `link_filtered_identity_01/window_timing_v2.json`.

## Artifacts

- Results: `data/drone_ml/2026-11-10/cp_lag_probe_01/` (one JSON per set, including `drff_r2_units.json` and `tampere.json`; `run_drff.py` selects the non-locked DRFF files).
- Same-timing diagnostic: `data/drone_ml/2026-11-10/same_family_diagnostic_02/`.
- Reproduce one set:

```bash
data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.cp_lag_probe --input data/air3s_o4/2026-10-23/connected-live-feed-01/*.cf32 --format cf32_le --sample-rate 20e6
```
