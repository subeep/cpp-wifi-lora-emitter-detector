# DSP reference evidence and identification validation

Updated: 3 October 2026. Implementation is authorised; ML and Remote ID remain deferred. The user's Mini 2 testing is deferred until they are ready. No USRP, HackRF or other radio was opened during this stage.

## What is implemented

The isolated worker now measures three chirp hypotheses in addition to SCF, held-out CP structure and time/frequency ROIs. The optional Wi-Fi panel displays these measurements. Build and runtime defaults remain OFF; existing packet, identity and security outputs are unchanged in the regression checks. A deterministic experimental waveform-evidence policy is implemented. Named-family acceptance remains disabled and no learned classifier exists.

The [research by Cwalina et al.](https://doi.org/10.3390/s25154552) describes chirp symbols in some DJI emissions and a carrier-offset-independent, frequency-shifted lag correlation. This implementation uses two adjacent 32 µs fragments and three reported approximate slopes: +135.2, +270.2 and −135.3 kHz/µs, associated with nominal 9/18/9 MHz sweeps. It examines every complete span inside each admitted tile. A sample rate insufficient for the nominal sweep is qualified as unsupported; meeting this necessary Nyquist condition does not establish complete receiver passband coverage. The paper's reported 20 ms repetition is **not** measured here.

For `L = round(Fs × 32 µs)` and `G = L`, the per-position measurement is:

```
p[n] = x[n+L] conj(x[n]) exp(-j 2π slope L n / Fs²)
C²(start) = |sum(p[start : start+G])|² /
            (sum(|x[start : start+G]|²) sum(|x[start+L : start+L+G]|²))
```

Whole-tile complex-mean removal and peak-magnitude normalization precede the products. Each half needs energy above `1e-10` of the whole-tile mean power times the gate length. Rolling sums rebuild every 1,024 positions to bound cancellation after strong bursts. Per hypothesis, only the maximum and its original sample coordinates are returned, with search-position counts, two subgate coherences and half-energy balance. This is bounded O(hypotheses × samples) work, with at most 65,536 contiguous samples and three fixed hypotheses; it never scans additional live IQ or joins tile gaps.

A complementary shape diagnostic differentiates complex phase at the selected maximum and measures a linear instantaneous-frequency slope and RMS residual over that same span. Every adjacent pair must meet the energy floor for a frequency fit; otherwise the fit is unavailable with `partial_phase_support`. Phase branch changes are counted. This numerical DSP fit trains no classifier. It does not prove absence of aliasing, a single emitter, or independent confirmation: both diagnostics use the same selected data.

Maxima, fitted slopes and residuals are measurements, not probabilities or significance tests. A two-tone confusable produces approximately 0.25 squared correlation in the tests. A chirp reference match alone cannot establish DJI, OcuSync generation, drone use, video role or flight state. The classification axes therefore still report waveform/link `unknown`, drone `insufficient_evidence`, and model `not_trained`.

The wire protocol is now version 4 (including selected-span mean frequency); older workers are rejected. The 64 KiB reply cap, four-window/262,144-sample copy cap, 128 MiB child limit, two-second request deadline and fault supervision remain unchanged. The process-test target now depends on all fault fixtures so its helpers rebuild with the current wire version.

## Corpus and results

The new [reference replay tool](/home/sudeep/Documents/newrocktest-cpp/tools/cyclostationary/validate_reference_corpus.py) builds an auditable manifest and replays originals read-only. Source labels remain outside the DSP worker. It checks sizes, hashes each selected IQ window, checks source size/mtime/inode after processing and retains all measurement JSON. These selected-window checks are not full DroneDetect integrity or continuity verification.

The [final manifest](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/reference_phase_validation_2026-10-03/manifest.json) and [report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/reference_phase_validation_2026-10-03/summary.json) cover:

| Sources | Recordings | Sampled windows | Qualifications |
|---|---:|---:|---|
| DroneDetect | 390 | 1,560 | All seven model codes, four conditions and populated states; 60 Msps remains provisional; full-file phase continuity and modes unknown |
| Locally verified Zenodo recordings | 6 | 24 | Native 120/200 Msps; publisher model labels; link modes, physical units and sessions unknown |
| Received ordinary Wi-Fi beacon crops | 3 | 3 | 20 Msps; narrow controls from one capture session, with related AP crops |
| Total | **399** | **1,587** | No replay errors; no radio, training or RID decoding |

All same-model DroneDetect conditions/states share one conservative group; Zenodo same-model bands and recording parts share another; ordinary Wi-Fi crops share one session group. There are 11 groups, **zero verified independent unit/session test groups**, and no named-drone-family truth records. Everything remains development-only. Partition auditing rejects source/group/unit/session leakage and independent partitions with unknown unit/session metadata. Interference-labelled DroneDetect sets still contain drones and are never ordinary negative controls. Folder `ON/HO/FY` labels are context, not generated activity verdicts.

The Wi-Fi crops are waveform confusables, not certified drone-free physical-platform evidence. Their ordinary-beacon decoding establishes PHY context; it does not prove which physical device emitted them or absence of every drone signal. The manifest keeps `drone_free_control: false` for all current sources.

Four distributed windows cover only approximately 0.22% of nominal DroneDetect recordings. Longer files are limited to their first nominal two seconds; short files use their available extent. Missing a rare chirp in these windows says nothing about its absence elsewhere or encounter recall. Native wideband results are not validation for the unchanged live 20 Msps passband.

Three selected development maxima were notable:

| Recording ID | Squared correlation | Frequency slope, kHz/µs | RMS residual, MHz | Phase branch changes |
|---|---:|---:|---:|---:|
| `BLUE/MP2_ON/MAV_0100_02.dat` | 0.924 | +270.233 | 0.750 | 0 |
| `CLEAN/MP2_ON/MAV_0000_00.dat` | 0.854 | +261.500 | 1.812 | 4 |
| `CLEAN/MP1_ON/MA1_0000_02.dat` | 0.803 | +264.359 | 1.601 | 6 |

These are selected maxima across positions/hypotheses/recordings, not corrected significance or confirmed links. Only the first has particularly close slope agreement with the nominal reference; the other two show poorer shape quality. The ordinary Wi-Fi controls' largest chirp scores are 0.0137–0.0162, but three correlated controls cannot measure false alarm rate. This previous measurement-only stage fitted no thresholds. The subsequent v1 preview policy uses development-informed heuristics described below; no precision/recall is claimed. Missing reference metadata is not replaced with a model-to-protocol lookup.

The prior 13-region/43-window ROI baseline retains **exactly identical existing per-tile SCF, CP and ROI fields** with the added chirp diagnostics: [preservation report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/chirp_phase_preservation_2026-10-03/summary.json). Shared-run corpus round trips ranged up to approximately 450 ms under concurrent builds and replays; that is not a live receiver latency benchmark.

A [private passband exercise](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/reference_phase_validation_2026-10-03/passband_exercise.json) reused the first selected candidate, with an exploratory −4.8 MHz offset derived from its mean instantaneous frequency and a provisional 28 MHz source passband. Its maximum falls from approximately 0.924 native to 0.481 with an 18 MHz passband at 30 Msps. The current FIR policy requires 20% output-rate guard space, so an 18 MHz passband at 20 Msps is correctly rejected. A 16 MHz passband at 20 Msps returns approximately 0.472, but cannot preserve the nominal full sweep. This exposes sensitivity to preprocessing/passband/quality; it does not validate a live 20 Msps detector. No filter guard, radio setting or acceptance rule was relaxed to obtain a stronger result. Explicit private-copy DC qualification is now available as described below. Automatic preparation, receiver calibration and statistical detection calibration remain R3 work.

Seven standalone suites and nine relevant opt-in integration suites pass after rebuilding the matching protocol fixtures. The default-off GUI builds and scanner equivalence suite pass. ASan/UBSan chirp and parent process/protocol checks pass with Release child/fixture paths and unchanged 128 MiB limits; leak checking is excluded for the environment's previously documented ptrace limitation. A phase-residual assertion was corrected to use the analytic noise/mean-removal perturbation bound rather than an unjustified fixed residual limit. That measurement-only stage introduced no acceptance threshold. [Software record](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/reference_phase_validation_2026-10-03/software_validation.json).

## Experimental evidence policy v1

`experimental_dsp_v1` is a frozen, inspectable **waveform preview** over the existing measurements. Prior development measurements informed its heuristics; it was frozen before the new software benchmark. It is neither a trained model nor a calibrated drone detector. File names, dataset model/state labels and capture-directory labels never enter the matcher. Native live results are assessed once by the supervisor after strict IPC validation and cached for the optional panel. The producer path and copy/resource limits are unchanged.

| Checks | Preview policy |
|---|---|
| Observed window quality | Spectral status measured; at least 16 FFT frames; positive variance and spectrum; DC power fraction ≤0.10; strongest-bin fraction ≤0.25; combined outer four bins at each edge ≤0.05 |
| Source integer rails | If the source is `ci16_le`, full-scale component fraction ≤0.01; unknown for float IQ and current live inputs |
| Generic OFDM structure | At least 16 symbols in each partition; train and held-out CP squared coherence ≥0.50; outside-prefix coherence ≤0.10; held-out contrast ≥0.40; symbol-alpha squared coherence ≥0.005 |
| Linear chirp reference | Maximum and both subgate squared coherences ≥0.80; half-energy balance ≥0.50; complete phase fit; zero phase branch changes; relative slope error ≤0.02; frequency RMS residual / nominal sweep ≤0.05 |
| Declared passband geometry | OFDM: whole-window 99% power extent plus one FFT bin lies inside the declared passband. Chirp: absolute selected-span mean frequency + half nominal sweep + one bin lies inside it |

An unknown integer-rail check is retained as unknown and does not fail the **observed** checks. Passing observed checks never verifies receiver quality or absence of clipping. Edge-power checks cannot establish absence of aliasing. Passband geometry uses a caller declaration or explicit private-filter width, never sample rate as a substitute for usable bandwidth. It does not calibrate analog response, prove the complete signal was acquired, establish source separation, or verify proprietary channel width. The chirp extent is an approximate conservative nominal-template check using the selected span, not measurement of an entire emission.

Each candidate exports every check, value, operator, threshold and known/pass state. Pattern consistency is independent of quality/passband status, so rejected candidates and their raw measurements remain visible. Results are `unavailable`, `no_match`, `quality_rejected`, `passband_unverified`, `passband_rejected`, or `experimental_match`. The last means a waveform pattern passed this preview policy under a declared profile. It is **not** named-family or drone acceptance. All classification axes remain unknown/insufficient-evidence/not-trained. Repetition, independent confirmations, receiver verification, physical platform attribution and family accuracy are explicitly absent. Current live passband and ADC rails remain unknown.

### Software controls and impairment results

The [96-case benchmark](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/evidence_benchmark_2026-10-03/summary.json) retains generator/policy SHA256, IQ hashes, checks and measurements. It contains 34 conservative source groups. Derivatives stay with their analytic waveform source; the three received Wi-Fi crops remain one session group. This is a software development benchmark, not an independent real-unit/session test set. Three clean analytic chirps match, all six ±0.3 MHz carrier-offset cases and six amplitude-scale cases match, and unknown/narrow passbands respectively abstain/reject. The ordinary analytic OFDM and all three received Wi-Fi controls match generic CP structure; none is promoted to a drone.

Twenty seeded stationary-noise cases and the tested tones, two-tone mixture, FSK, frequency hopping, impulse train, repeated non-OFDM symbol and nonlinear chirp produce no corresponding waveform-policy match. These finite software controls do not establish field false-alarm bounds, correct for corpus-wide maximum selection, or cover the missing real confusable platforms.

The policy has material sensitivity limits: only the three 20 dB added-noise cases match; all injected 10/0/−5 dB cases fail. These are analytic active-symbol SNR settings, not measured receiver SNR. The two ±500 ppm clock cases for the 18 MHz chirp fail, while the ten other tested clock cases match. All six two-path cases (0.5-amplitude echo at 3/20 samples), all three tone mixtures, all three hard-clipped int16 chirps and all three sweep masks fail. The source-DC cases preserve a pattern match but are quality-rejected. No threshold was adjusted after observing these misses. The matcher is therefore not a robust multipath/mixture detector; full R3 and production G2 remain open.

The [new local-corpus report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/evidence_corpus_2026-10-03/summary.json) replays 399 originals / 1,587 windows with no errors. It finds 19 generic OFDM pattern matches and one chirp pattern match. Fifteen OFDM candidates and the chirp candidate fail observed quality; the remaining four OFDM candidates have unknown passband. Across all windows, 1,412 fail the preview DC check and 92 fail the strongest-bin check; counts can overlap. This exposes acquisition/preparation artifacts rather than measuring drone recall. All passbands are unverified in this native replay. There are still zero independently verified unit/session groups and no named-drone-family truth. [Preservation audit](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/evidence_corpus_2026-10-03/preservation.json) confirms every previous tile value, classification, selection and selected-window IQ hash is exactly unchanged.

### Explicit source-DC and passband exercise

The first selected development candidate has approximately 27.34% constant complex-mean power. Downmixing by −4.8 MHz translates this component into a +4.8 MHz tone; later analysis mean removal does not remove that translated tone. The new `--remove-source-dc` option subtracts the finite source-window mean **before** private downmix/filtering. It is explicit and defaults off. It does not modify original IQ or receiver processing; it can also remove genuine near-DC signal content, so it is not automatically applied live.

The [recorded exercise](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/evidence_policy_2026-10-03/dc_passband_exercise.json) keeps the provisional 60 Msps / 28 MHz source declarations and records all preparation parameters:

| Preparation | Chirp maximum | Preview outcome |
|---|---:|---|
| Native first tile | 0.924 | Pattern consistent; DC quality rejected |
| 30 Msps, −4.8 MHz offset, 18 MHz FIR passband, no pre-mix DC removal | 0.481 | No pattern match |
| Same filter with explicit pre-mix DC removal | 0.933 | Pattern consistent; nominal extent + margin crosses declared passband |
| 20 Msps, −4.8 MHz offset, 16 MHz FIR passband, pre-mix DC removal | 0.931 | Pattern consistent; 18 MHz nominal sweep outside declared passband |
| 30 Msps, −4.6 MHz offset, 18.4 MHz FIR passband, pre-mix DC removal | 0.933 | Experimental waveform match under this declared profile |

The final preparation was chosen from this same development example to demonstrate a fitting geometry; it is not held-out validation. Its recovered correlation supports the DC-artifact diagnosis. High correlation in the truncated 20 Msps case demonstrates why a correlation-only rule is unsafe. The 20% FIR guard remains intact: an 18 MHz passband at 20 Msps is still rejected. No radio profile, channel selection or automatic preparation was changed.

Eight standalone suites, all 22 registered opt-in integration/regression suites (including ten cyclostationary/scanner suites), nine additional legacy Wi-Fi targets and the default-off scanner check pass. Both GUI variants build. ASan/UBSan checks pass for math/preparation, chirp, evidence and parent process/protocol; the process check uses Release child/fault fixtures with unchanged limits, and leak checking remains excluded for the documented ptrace limitation. [Current software record](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/evidence_policy_2026-10-03/software_validation.json).

## Reproduce

```sh
cmake -S tools/cyclostationary -B build/cyclostationary-offline -DCMAKE_BUILD_TYPE=Release
cmake --build build/cyclostationary-offline -j 4
ctest --test-dir build/cyclostationary-offline --output-on-failure
python3 tools/cyclostationary/validate_reference_corpus.py \
  --tool build/cyclostationary-offline/wifi_cyclo_replay \
  --output data/wifi_drone_analysis/reference-replay-new-run --workers 2
```

The replay output directory must be new. `--manifest-only` audits metadata without replay. `analyze --measure-chirps` supports the existing private band-selection flags but requires at most 65,536 analysis samples after filtering; larger inputs fail rather than silently crop. `analyze --assess-evidence` includes chirp diagnostics and the preview checks, limited to 65,536 analysis samples. Native `--usable-bandwidth HZ` is caller-declared; filtered analysis uses `--source-bandwidth` and `--roi-bandwidth` instead. `--remove-source-dc` requires all band-selection flags. `shadow-replay` always includes native-rate chirp diagnostics and evidence checks, with optional declared `--usable-bandwidth`. Reproduce software controls using `python3 tools/cyclostationary/benchmark_evidence.py --tool build/cyclostationary-offline/wifi_cyclo_replay --output data/wifi_drone_analysis/evidence-benchmark-new-run`. Python is only an offline validation dependency.

## Identification milestones

| Milestone | Delivered evidence / next exit | Current status |
|---|---|---|
| R1 — Conservative corpus and replay | All local references, original coordinates/hashes, source grouping and explicit missing truth | Implemented and software-tested; broader M1 independent corpus pending |
| R2 — First proprietary-link research feature | Fixed chirp hypotheses, independent direct correlation oracle, injected signs/CFO/scales/nulls/confusable, selected-span phase diagnostic, bounded IPC/panel | Implemented experimental measurement; no validated family |
| R3 — Qualify candidates for live profiles | v1 experimental quality/geometry checks, transparent OFDM/chirp matcher and 96 software/control cases delivered; receiver passband calibration, robust mixtures/multipath, source separation, look-elsewhere calibration and broader banks still required | Partially implemented; full M2 remains open |
| R4 — First Mini 2 family pilot | Labelled local drone/controller states and firmware/mode, ordinary confusables, off/on/off receive-only captures, multiple sessions; frozen rules followed by held-out evaluation | Deferred until user is ready; one drone supports a pilot, not independent-unit family validation |
| R5 — Named family and activity acceptance | Independent units/sessions and supported receiver profiles pass G2/G2A; otherwise explicit experimental/unknown scope | Pending; no named family enabled |
| R6 — Broader links and field non-interference | Additional reference families and missing waveform groups; receiver/resource/soak checks and capability ledger | Pending; no current breadth or release gate declared complete |

DJI's [Mini 2 support page](https://www.dji.com/mobile/support/product/mini-2) specifies OcuSync 2.0 and distinguishes image transmission from QuickTransfer Wi-Fi/Bluetooth. Later labelled tests must record the actual active mode; detecting QuickTransfer must not be treated as OcuSync. No flight is needed for the first grounded RF-link pilot. Flight/activity claims need separate operator ground truth. Current 20 Msps X310 transport limits must be preserved; wider offline source bandwidth does not authorise raising the running receiver rate.

The remaining named-family catalogue is intentionally unaccepted until evidence exists. Lightbridge/OcuSync variants, Wi-Fi drone links, ELRS/other RC, other digital FPV and analog video need their own mode-qualified references and confusable results. Three slope templates do not count as three drone-link families.

The [broader integration plan](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_DRONE_ADDON_PLAN.md) remains authoritative for the intended deliverables; this record describes an implementation slice, not completed drone attribution.
