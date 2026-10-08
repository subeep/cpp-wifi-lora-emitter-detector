# Cyclostationary waveform coverage and qualification

Updated: 8 October 2026. Delivered analysis expansion under the [next-step plan](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_NEXT_STEPS.md). ML, Remote ID and further Mini 2-specific work remain deferred. No receiver was opened or setting changed.

## Delivered additions

The optional isolated worker now runs `bounded_caf_morphology_v1` on each admitted contiguous tile. The Wi-Fi panel has an expandable section displaying its cyclic-rate and envelope/frequency-state measurements. Existing ordinary SCF, OFDM, ROI, chirp and `experimental_dsp_v1` acceptance fields are unchanged. Classification remains unknown; these measurements do not enable additional drone-family or exact modulation labels.

Offline `analyze --expand-waveforms` exposes the same measurements. `--prepare-candidates` additionally proposes and privately prepares up to two candidate bands, while retaining the raw/context measurements. Candidate filtering is not promoted into the live worker. It needs caller-declared usable bandwidth and keeps unknown receiver calibration explicit. Both build/runtime defaults remain OFF.

## Capability ledger

| Scope | Current capability | Status / remaining qualification |
|---|---|---|
| Ordinary SCF | Existing coarse spectral/cyclic peaks | Experimental; no statistical significance or identity guarantee |
| Ordinary cyclic autocorrelation | Four lags, coarse cyclic-rate discovery, fine refinement and separate holdout | New experimental measurement; finite-duration/rate limits apply |
| Conjugate cyclic autocorrelation | Positive and negative cyclic rates, independent oracle and holdout | New experimental measurement; tones and carrier structure are confusable |
| W1 OFDM | Five WLAN timing hypotheses and explicit offline custom timing, held-out CP checks | Bounded general lag/period/prefix discovery now added; experimental and incomplete; see second expansion |
| W2 DSSS/chip-spread | Cyclic features plus bounded Barker-11 chip/carrier/code-phase compatibility | Experimental fixed-code measurement only; no generic DSSS/protocol acceptance |
| W3 chirp/CSS | Three fixed templates, bounded generic up/down linear-sweep shape with separate held check in optional worker; dechirp refinement offline | Partial spans and slope measurements only; CSS decoding, full duration/periodicity, alias recovery and calibrated rejection pending |
| W4 FSK-like | Phase-increment frequency-state centres, concentration, transition count and constant-envelope consistency | New experimental two-state morphology; noisy/echo controls miss; GFSK/MSK and exact FSK identity unvalidated |
| W5 PSK/OQPSK | Conjugate features exercised on BPSK/QPSK controls | Features only; BPSK/carrier cycles observed, QPSK not reliably discriminated; no order/PSK/OQPSK acceptance |
| W6 QAM | Existing generic spectral measurements | Dedicated timing/channel/constellation qualification unavailable |
| W7 FM/video | Existing generic spectral measurements | Dedicated FM/video structure unavailable |
| W8 ASK/OOK-like | Raw-copy envelope quantiles, contrast, occupancy, two-level residual and transitions | New experimental two-level morphology; tone beats/mixtures are confusable, impaired controls miss |
| B1/B2 frequency agility and RF activity | Existing per-tile regions and new transition measurements | Within-tile measurements only; cross-capture history/recurrence/qualified events pending |
| Named drone/link family and role | Unknown, insufficient evidence | Acceptance disabled; independent labelled references and release gates pending |

Every measurement is conditional on an eligible observed tile, not complete monitored-channel coverage. No new acquisition band, sample rate, dwell or scheduling is introduced. Sub-GHz/cellular/new-band links remain outside the unchanged Wi-Fi coverage.

## Cyclic estimator and support contract

Inputs must be finite complex IQ, at most 65,536 samples and a declared rate in `(0, 1 GHz]`. Two power-of-two partitions of 1,024–8,192 samples are selected, with at least 16 intervening samples. Larger inputs use the first and last eligible partitions rather than joining phase across the unobserved interior. Each partition has its own removed mean; amplitude scaling uses bounded double intermediates. Short inputs return insufficient support; absent numerical variation abstains.

For lag `l`, the ordinary statistic uses `x[n+l] * conj(x[n])`; the conjugate statistic uses `x[n+l] * x[n]`. Periodic Hann weights on each lag-product sequence and `exp(-j 2π α n/Fs)` define the convention. Squared magnitude is normalised by the two weighted input powers. Frequency resolution and searched rates are measurements, not symbol/chip-rate identity.

Four lags (`0, 1, 4, 16`) and two statistic kinds require at most eight discovery FFTs. Alpha spacing is `Fs / partition_samples`; coarse candidates exclude fewer than nine coarse cycles, DC and Nyquist-adjacent bins. Ordinary rates are positive; conjugate rates include both signs. A maximum of four peaks per kind is selected, with duplicate coarse frequencies suppressed within two bins. Each is refined over five fixed offsets (`−0.5, −0.25, 0, +0.25, +0.5` bins); at most 40 refinement evaluations occur. The selected frequency and lag are then measured on holdout without refitting.

At 20 Msps and an 8,192-sample partition, the alpha grid is about 2.44 kHz and the lowest admitted coarse rate about 22 kHz. Shorter partitions have coarser coverage. Fine-grid interpolation is not a guarantee of frequency-estimate accuracy, and short live tiles cannot establish long-period recurrence.

The development heuristic `persistent_pattern` requires both squared coherences ≥0.05. It is uncalibrated and does not correct the complete multi-candidate search. Independent direct-sum tests verify ordinary/conjugate signs, discovery/holdout normalisation and absolute source-time phase convention. Separate means and a guard prevent shared-sample fitting, but correlated real-world channel/traffic conditions do not make holdout statistically independent.

## Morphology contract

Envelope/frequency features use the raw analysis copy without mean removal. Powers are privately normalised to mean power; float ADC scaling and physical receiver SNR are not inferred. Envelope low/high levels are 20th/80th percentiles. A two-level pattern requires contrast ≥0.7, nearest-level squared residual ≤0.05 relative to level separation, high-level occupancy in 0.1–0.9 and at least eight transitions. The quantile choice limits extreme-duty signals even before those occupancy checks.

Frequency states use adjacent-sample phase increments modulo the sample rate, discarding pairs whose component power is ≤`1e-4` of mean power. A 256-bin circular histogram proposes two states separated by at least five bins; each state includes a two-bin-radius neighbourhood. A two-frequency pattern requires ≥80% valid phase-pair coverage, amplitude CV ≤0.3, combined state concentration ≥0.8, each state fraction ≥0.1 and at least eight consecutive-supported state transitions. Gaps/invalid pairs do not create transitions. Reported centres are circular means; near-Nyquist aliasing is explicit.

These are shape checks, not exact modulation classifiers. Carrier phase, modulation transitions, filtering, mixtures and multipath can change them. Thresholds and support counts are exported; unavailable values are null. Agreement between these and other features is not an independent probability vote.

## Offline candidate preparation

Existing worker-local ROI measurements provide source-region power spans, joint spans across contrasted spectral intervals, and individual interval proposals. A joint span prevents selection around just one apparent FSK state from being the only analysis. Declared-geometry-eligible proposals rank before ineligible spans, then by region energy/spectral share. Raw context is always retained and omitted proposals are counted; no source-separation claim is made.

Each proposed band includes four PSD bins of total width margin. Preparation reuses the existing private Hamming-sinc filter and integer decimator, with no automatic source-mean subtraction, padding or waveform-dependent receiver changes. At most two candidates, decimation ≤16, ≤1,023 taps and **64 million total FIR operations** are admitted. Eligibility/operation estimates are checked before filtering. Original source centres, steps, region units and discarded transients are explicit, including nested preparation after an already selected band.

Unknown usable width means no filtering. Out-of-passband or inadequate filter/budget cases retain explicit unavailable/rejection statuses. Caller-declared width is a geometric prerequisite, not calibrated analog receiver coverage. Filtering can alter morphology: the benchmark includes spread-spectrum controls that acquire a two-frequency shape after preparation, while clean/native FSK shapes can disappear. Prepared candidates therefore cannot replace raw context or become qualified identities by themselves.

## Measured validation

The grouped [breadth/control report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/waveform_breadth_final_2026-10-05/summary.json) contains 107 cases / 77 conservative groups: fresh white/coloured noise, tones/beats, analytic FSK/OOK/BPSK/QPSK/Barker-11 DSSS, fixed noise/echo impairments, periodic/discovery-only envelopes, mixtures and three received Wi-Fi crops from one session. Derivatives remain in their source groups. This is software development validation, not an independent drone test or calibrated family-accuracy report.

| Native/context outcome | Result |
|---|---|
| Forty white + eight coloured noise controls | No persistent ordinary/conjugate pattern or two-state shape |
| Six periodic envelope controls | All six have persistent ordinary cycles |
| Six discovery-only envelope controls | None persist on holdout |
| Three clean FSK controls | All three match two-frequency morphology |
| Three clean OOK controls | All three match two-level envelope morphology |
| BPSK / Barker-11 DSSS controls and their 10 dB added-noise cases | Conjugate cycles observed; this does not validate exact modulation or spreading identity |
| Three received Wi-Fi crops | No new persistent/two-state pattern in these selected crops; not proof of general Wi-Fi rejection |
| Five tones / two tone beats | Four tones have conjugate cycles; both beats have ordinary/conjugate cycles and two-level envelope shapes |
| Noisy/echo FSK/OOK controls and mixtures | Material misses and confusable shapes remain; see per-case reports |
| Legacy outputs and sources | Every pre-existing JSON field remains exactly equal for all 107 cases; source hashes remain unchanged |

No threshold was relaxed to make impairment cases match. Noise/echo settings describe software injection, not measured receiver SNR. The complete prepared pipeline is reported too; raw/control counts above must not be substituted for final searched-detector calibration.

All ten standalone suites and all 24 registered opt-in application suites pass, including scanner/lane, security, provenance, worker faults and GUI panel checks. Affected waveform/CLI suites are rerun after the final preparation/metadata changes. ASan/UBSan waveform/candidate tests pass with leak detection disabled. Both opt-in and default-off GUI variants build, with default-off scanner equivalence passing. The normal running application is not launched/replaced.

Nine [full-copy-budget process checks](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/waveform_resources_2026-10-05/summary.json) run four independent 65,536-sample tiles at 20/60/200 Msps. All complete within the unchanged two-second deadline; recorded round trips are approximately 210–343 ms on this host. These are software resource checks, not receiver performance, scan-cycle equivalence, radio coverage or soak acceptance. Child memory/CPU/file limits, request/reply sizes, drop/cancellation/restart policy and the producer copy budget are unchanged. Wire protocol is **5**; opt-in application and worker must be rebuilt together.

## Reproduce without hardware

```sh
cmake -S tools/cyclostationary -B build/cyclostationary-offline -DCMAKE_BUILD_TYPE=Release
cmake --build build/cyclostationary-offline -j 4
ctest --test-dir build/cyclostationary-offline --output-on-failure
python3 tools/cyclostationary/benchmark_waveforms.py --tool build/cyclostationary-offline/wifi_cyclo_replay --output data/wifi_drone_analysis/waveform-breadth-new-run
```

For an existing IQ file, use `analyze --samples 65536 --expand-waveforms` with its explicit format/rate and optional offset. Add `--prepare-candidates --usable-bandwidth HZ` only with a known caller declaration; it remains uncalibrated receiver metadata. Existing explicit private band-selection flags can also precede expanded analysis. Output is JSON; originals are read-only. `shadow-replay` automatically reports the new native-rate measurements from protocol-6 children; offline candidate preparation is rejected in shadow mode.

## Milestone status and next work

N1 now has a reproducible grouped breadth benchmark and this capability/support contract; broader captured negatives and unresolved dataset metadata remain open. N2 has an offline bounded preparation slice, with selection/filter confusables and misses recorded; automatic live preparation is not accepted. N3 has fine CAF refinement, separate holdout and conjugate features; broader resolution/lag/robustness work remains open. N4 has fresh software controls, not sealed receiver/field calibration. N5 expands FSK/ASK-like measurements and exercises spread/PSK feature controls, not full waveform classification. N7 adds bounded worker/display support and software equivalence, not G5/G6 live qualification. N6/N8 and the original release gates remain open.

The second expansion below adds bounded general CP timing and fixed Barker-11 compatibility. Next priorities are whole-search rejection/disambiguation, more robust frequency/envelope estimators, captured ordinary-traffic controls and wider chip/clock/channel qualification. Gap-aware activity history follows qualified measurements. Mini 2-specific tuning, ML training and Remote ID remain deferred.

## General timing and short-code discovery — second expansion

The optional worker now also runs `bounded_ofdm_barker_discovery_v1`, with a new expandable panel section. Offline `--discover-structure` adds only this measurement; `--expand-waveforms` and `--prepare-candidates` include it on their native or explicitly selected analysis copy. Automatic offline candidate preparation still measures only its original CAF/morphology features; the new structure search is not silently multiplied over every prepared candidate. None of these measurements feeds `experimental_dsp_v1` or classification. Protocol **6** requires rebuilding the opt-in application and sibling helper together; an older helper is rejected.

### Bounded OFDM/CP timing search

Two separately centred discovery/holdout partitions use the same 1,024–8,192 sample/16-sample guard contract as CAF. Linear autocorrelation uses a twice-length zero-padded FFT, not circular wraparound. Normalised lag coherence ranks at most four local maxima, suppressing candidates less than three samples apart. Useful lags range from 16 to `min(1024, partition_samples / 10)` samples.

A periodic-Hann lag-product FFT proposes symbol periods. The two strongest eligible positive bins produce rounded-period candidates at offsets 0, −1 and +1 sample; prefix ratios 1/4, 1/8, 1/16 and 1/32 supply bounded fallback proposals. At most eight unique prefixes per lag survive, each at least four samples and no longer than half the useful lag. A hypothesis requires at least eight complete periods of eligible lag pairs. The search performs at most six FFTs and 32 timing trials, retaining four by discovery CP-versus-outside contrast. This is a restricted search, not exhaustive timing coverage.

The prefix phase is fitted only on discovery. Held-out CP, outside-CP and symbol-rate cyclic coherence use that same phase in absolute tile coordinates, with both pair endpoints inside their own partition. The experimental pattern flag requires discovery and holdout CP coherence ≥0.5, both outside coherences ≤0.1, held contrast ≥0.4 and held symbol-cycle coherence ≥0.005. These thresholds are uncalibrated. Ordinary Wi-Fi and repeated chip structures can pass; a flag establishes no protocol, generation, subcarrier count, drone or vendor identity.

### Fixed Barker-11 compatibility

A separate short-code measurement tests only `[1,1,1,-1,-1,-1,1,-1,-1,1,-1]`. A discovery squared-signal lag-one moment proposes a carrier in ±Fs/4 and its Fs/2 alias. Each of the two branches tests integer chip lengths 1–16 samples, including sample phase and code phase, for 32 bounded chip/carrier hypotheses. Integrated chips are correlated against the eleven-chip sequence and normalised by chip power. Code phase groups contain non-overlapping complete words; unsupported zero-power words are skipped. The strongest competing phase is recorded rather than discarded.

Four discovery proposals survive. Their carrier, chip length and phase remain fixed on holdout. A code-compatible flag requires at least eight complete supported words per partition, code coherence ≥0.8 and a margin ≥0.5 over the strongest competing phase in both partitions. Carrier coherence is exported as a diagnostic; it is not a confidence or independent acceptance vote.

This supports an experimental measurement on compatible BPSK-spread, integer-chip timing. Other spreading codes, fractional chip timing, clock drift, differential/quadrature schemes, pulse shaping and broad captured DSSS coverage remain unqualified. It is not an 802.11b decoder or a generic DSSS classifier. In particular, Fs divided by a real chip rate need not be an integer. Chip rate `Fs / chip_samples` is conditional on the declared input rate; receiver clock/passband accuracy is not inferred.

### Second-expansion validation

The [structure benchmark](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/structure_breadth_verified_2026-10-05/summary.json) includes 159 cases in 83 conservative source groups. All 159 preserve every baseline JSON field and source hash. All 107 overlapping cases reproduce the earlier CAF/morphology and candidate-preparation outputs exactly, including impaired/filter confusables. Additional copied-prefix Gaussian controls test CP geometry, not a complete RF protocol/transmitter. Their noise derivatives, discovery-only variants and different CP geometries from the same Gaussian seed share groups. New Barker chip/carrier variants also share their underlying payload-seed group. This is development evaluation; it is not a sealed, independent device/session test.

Results include:

- All 18 clean and 18 added-noise CP-geometry controls recover the expected useful/prefix timing with a held CP-pattern match; exact recovery is separately counted in the report.
- Six discovery-only CP cases and one discovery-only Barker case have no held pattern match. The deliberately different eleven-chip code is rejected by the Barker compatibility check.
- Three clean, three added-noise and one echo Barker-spread cases are code compatible; six further clean chip/carrier-offset controls also match. These are software controls, not a real-world DSSS sensitivity claim.
- Forty white-noise, eight coloured-noise, tones/beats, ordinary unspread BPSK/QPSK, tested FSK/OOK, two long aliased chirps and the tested mixture controls have no new pattern flag. Three received Wi-Fi crops from one session do have CP patterns, as expected; their physical platform is not established.
- **Two impaired Barker-spread cases also pass CP morphology:** one added-noise case at useful/prefix 32/12 samples and the echo case at 17/5 samples. Competing matches remain visible. This is a demonstrated ambiguity, and a reason not to promote CP flags into an OFDM-only classifier or drone label.

Independent direct-sum tests check FFT linear-lag coherence, train/held CP gating, absolute-phase symbol cycles, direct chip integration, code correlation and complete-word counts. Tests also cover non-WLAN timing, chip lengths/carrier aliases, discovery-only rejection, source preservation, gains 1e−25/1e25, finite/rate/sample caps and sparse inputs with legitimately fewer code proposals. Strict IPC tests reject invalid phases, support counts, carrier ranges, nonfinite scores, invented flags and overlapping partitions; four-tile metadata remains within the unchanged 64 KiB reply ceiling.

The [full-budget worker record](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/structure_resources_verified_2026-10-05/summary.json) contains nine native-rate process checks at 20/60/200 Msps, using four independent windows / 262,144 copied samples. Round trips were about **231.5–252.8 ms**, inside the unchanged two-second deadline, 128 MiB address-space and 30-second CPU lifetime limits. These are offline noise/profile checks, not proof of live hardware non-interference or an exhaustive worst-case latency bound.

Software verification: 11 standalone CTests and 25 registered opt-in application CTests passed; default-off GUI build/scanner-lane check passed. New structure tests passed ASan/UBSan with leak detection disabled for the existing environment limitation. Both GUI configurations were compiled, without launching the monitor or opening any radio. Subsequent test-only additions were rerun in the affected suites.

Reproduce the structure report using:

```sh
python3 tools/cyclostationary/benchmark_structure.py \
  --tool build/cyclostationary-offline/wifi_cyclo_replay \
  --prior-breadth data/wifi_drone_analysis/waveform_breadth_final_2026-10-05/summary.json \
  --output data/wifi_drone_analysis/structure-breadth-new-run
```

N5 now has bounded general CP timing and a fixed short-code branch; full W1/W2 qualification is still partial. N4 whole-search rejection remains the next gate, with the demonstrated CP/spreading ambiguity included, alongside captured negatives, fractional/clock/channel impairments and frequency/envelope robustness. N6 gap-aware activity history and N8 independent family validation remain open. Mini 2-specific tuning, ML and Remote ID remain deferred.

Reproduce the worker resource profile on the retained software control:

```sh
python3 tools/cyclostationary/benchmark_resources.py \
  --tool build/cyclostationary-offline/wifi_cyclo_replay \
  --input data/wifi_drone_analysis/waveform_resources_2026-10-05/noise.cf32 \
  --output data/wifi_drone_analysis/structure-resources-new-run
```

## Rejection review and GUI summary — third expansion

`expanded_measurement_review_v2` adds a **separate bounded metadata review**, computed in the supervisor after validated worker replies. It uses existing measurements and the frozen observed-quality checks; it performs no IQ processing in the producer or GUI. It does not replace `experimental_dsp_v1`, change raw pattern flags, or enable any classifier/identity output. Wire protocol 6 is unchanged because the child sends the same measurements and the parent derives the review locally. At most 18 reviewed candidates are retained per tile.

Each review entry exposes its support checks, status and all failed reasons. States distinguish unavailable/not-requested, no pattern match, observed-quality rejection, declared-band rejection/unknown coverage, competing CP/Barker patterns, experimental CP/code structure and generic diagnostics. Coexisting CP and Barker matches prevent either from being presented as a uniquely resolved structure. Other mixtures remain unresolved; this rule is not a source-separation algorithm or proof that Barker excludes an OFDM transmitter.

Ordinary/conjugate cyclic patterns and envelope/frequency shapes remain diagnostics, even when their thresholds pass. They do not establish a specific waveform type. Quality and coverage blockers remain visible for these diagnostics too. A CP/code candidate may be an experimental structure match only after its existing discovery/holdout/support checks and observed-quality/declared-geometry checks pass without the tested CP/code ambiguity. Analog receiver calibration, modulation order, protocol, platform, drone/link family and role remain unvalidated.

### What the fresh controls showed

The first [development review run](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/review_calibration_2026-10-05/summary.json) uses 695 cases / 221 conservative groups. Stronger coloured noise (`y[n] = 0.975*y[n−1] + noise[n]`) exposed raw CAF matches in 34 of 128 stationary-noise seed groups when taking the maximum across duration/rate variants. Six coloured-noise cases also passed the original generic experimental-pattern review. This demonstrates that separate holdout plus a fixed 0.05 CAF threshold does **not** calibrate coloured-noise significance. The original review source is retained with that report; the raw DSP thresholds were not tuned to suppress these cases.

The revised v2 review separates generic diagnostics from CP/code structure. The [fresh v2 evaluation](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/review_fresh_2026-10-05/summary.json) freezes that rule before evaluating a new noise seed range offset by 500,000. It includes **695 cases / 221 groups**: the original broader controls, 512 new stationary-noise profile cases, clock/echo/DC/float-limiting impairments, pulsed-noise confusables and three received Wi-Fi crops. Duration/rate variants and shared Gaussian/payload sources stay grouped. All 695 preserve every legacy JSON field/source hash, and all 159 overlapping structure cases preserve the earlier structure measurements exactly.

| Fresh v2 outcome | Result and interpretation |
|---|---|
| White-noise profile cases | 0/256 raw pattern cases |
| Strong coloured-noise profile cases | 56/256 raw pattern cases; retained as rejected/diagnostic measurements |
| Stationary-noise seed-group maxima | 28/128 groups have some raw pattern across variants; none becomes an experimental CP/code structure result |
| Nominal 95% Wilson interval for reviewed stationary-noise structure matches | 0–2.91% for 0/128 independent generator groups; conditional software interval, **not** receiver/field/drone accuracy |
| Eight pulsed-noise controls | Cyclic diagnostics occur in all eight; none becomes a CP/code structure result |
| CP/Barker co-matches | Three ambiguous cases across the original impaired spreading controls and added echo controls; competing CP/code candidates remain blocked |
| Three clock-impaired CP controls | One retains a CP structure result; two miss. Clock robustness remains incomplete |
| Three CP echo controls | All retain structure results in the tested conditions; no general multipath guarantee |
| CP/Barker DC controls | Raw structure can persist, but observed-quality checks reject the contaminated windows |
| Known band coverage | Several valid analytic positives are conservatively blocked by their narrow caller-declared geometry; the report retains these losses |
| Three received Wi-Fi crops | Generic CP structure results persist. This is expected ordinary-traffic ambiguity, not drone evidence |

Stationary null profiles use 4,096 and 65,536 samples at declared 20/200 Msps. Each profile has 64 white or 64 coloured independent generator groups; the 128-group combined statistic takes a maximum over the four variants rather than counting them as independent. Declared rate changes on software controls do not establish real receiver performance. These are development controls with nominal binomial score intervals, not a sealed physical-unit/session test. V2 improves review specificity; it does not improve the underlying CAF estimator's measured coloured-noise false-match rate or supply a calibrated detector threshold.

### GUI access and verification

In the opt-in GUI build, open **Wi-Fi 2.4 GHz or Wi-Fi 5 GHz → Drone / link analysis (experimental cyclostationary DSP) → Enable optional DSP analysis**. The usual local `build/rf_monitor_gui` is now rebuilt with the optional panel included; no running monitor is restarted. Source CMake defaults remain OFF for fresh configurations, and runtime analysis remains OFF. The section remains collapsed and analysis disabled by default. A front summary now shows general CP timing, Barker-11, frequency/envelope shapes and ordinary/conjugate cyclic measurements, with review state and raw match count. This removes the need to scroll past worker diagnostics to reach new features.

Expand **Pattern review checks** for support/quality/coverage failures and competing explanations. Failed candidates can be shown explicitly. **General timing and short-code structure** includes useful/prefix times, symbol/chip/carrier rates, holdout contrast, competing-code-phase margin and support counts. **Expanded cyclic and waveform measurements** shows raw cyclic and morphology features. Worker/capture diagnostics and existing reference/ROI/chirp tables remain in separate expandable sections. The optional scroll pane now admits 200–480 pixels. Its contents remain collapsed until opened, and analysis stays disabled until explicitly enabled. Existing packet/identity/security content is preserved. No new receiver or analysis setting is applied automatically.

The [synthetic GUI preview](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/review_gui_2026-10-05/panel.png) is software-rendered from the actual ImGui test draw list and font atlas. It is not a recording from the monitor or a drone. Text/render tests cover the new review summary, competing patterns, unknown identity, both bands, insufficient support, disabled/waiting, known gaps, stale epochs and wrong-band results. ASan/UBSan review checks pass with the existing leak-detection limitation. All 12 standalone and 26 registered opt-in application suites passed, with affected suites rerun after v2 and final GUI changes. Both default-off and opt-in GUIs build without launching the normal application.

The [current worker resource report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/review_resources_2026-10-05/summary.json) contains nine full-budget four-tile process replays at declared 20/60/200 Msps: about **224.2–241.7 ms**, within unchanged deadlines/limits. These remain offline software checks; live receiver off/on/off performance and soak gates are pending.

Reproduce the current review evaluation:

```sh
python3 tools/cyclostationary/benchmark_review.py \
  --tool build/cyclostationary-offline/wifi_cyclo_replay \
  --prior-structure data/wifi_drone_analysis/structure_breadth_verified_2026-10-05/summary.json \
  --fresh-seed-offset 500000 \
  --output data/wifi_drone_analysis/review-new-run
```

`analyze --review-waveforms` requests the raw expanded measurements plus this review. Existing `--expand-waveforms`, `--discover-structure` and `--prepare-candidates` also include the review of the measurements requested on their native/explicitly selected context; `--discover-structure` keeps unrequested morphology marked not-requested. `shadow-replay` includes per-tile reviews, with live-equivalent unknown width unless offline callers explicitly declare one. Offline private filtering and chirp refinement remain offline-only; they were not promoted into the GUI acquisition path.

N4 now has measured whole-search software false matches, nominal grouped uncertainty and explicit diagnostic/ambiguity handling. It remains partial: coloured-noise-aware significance, captured background, profile-specific release thresholds and field exposure are still open. N7 gains a tested GUI review, not live hardware acceptance. Next work should address coloured-background robustness and clock/channel support before accepting new waveform labels, then N6's gap-aware generic RF activity. N8 independent drone/link validation, ML, Remote ID and further Mini 2-specific work remain deferred.


## Fourth expansion: general linear-sweep shape

`bounded_linear_sweep_discovery_v2` extends coverage beyond the three fixed research slopes. It examines only the first and last independent support windows of an admitted contiguous tile (each 512–8192 samples, disjoint with at least 16 samples between them; maximum input 65,536 samples). Raw copies are scaled separately for numerical stability; their means are not removed. It averages eight principal adjacent-sample phase increments before slope fitting, with the average centred at the correct sample time. Every raw increment still needs the phase/power guards. It does not unwrap phase or recover aliases. The average reduces adjacent phase-noise variation without changing the residual/coherence thresholds.

Discovery searches spans 64/128/256/512/1024/2048 samples at half-span steps. There are at most 498 base windows and twice that many overlapping-continuation checks. A proposal needs phase-pair support at every increment, both adjacent powers above 0.0001 times partition mean power, instantaneous phase frequency within ±0.45 Fs, an observed fitted sweep between 0.04 and 0.7 Fs, frequency-fit RMS at most 0.003 Fs, dechirped squared coherence at least 0.65 and amplitude CV at most 0.5. A neighbouring half-overlap window must support the same slope. This favours partial gates with timing margin rather than complete sawtooth spans ending at a reset.

At most three proposals are retained, ranked by observed sweep width and then coherence. Same-direction slopes within 5% of a previously retained slope are suppressed. This can merge close slopes; it does not count emitters. Each proposal freezes slope and span before accessing the held partition. The held search checks at most 255 positions per proposal and fits a constant carrier centre separately in each eligible window, then computes fixed-slope frequency residual and dechirped coherence. It uses the same RMS/coherence/amplitude heuristics. Held position searches and carrier nuisance fits are disclosed in JSON and the GUI; the search is not a preselected single-test significance estimate. Partition separation does not imply independent propagation or traffic sources.

The observed span is a measurement window, **not** a complete chirp or symbol duration. Limited support, low SNR, resets, sample-clock effects, nonlinear sweeps, multipath, mixtures, phase aliasing and scan gaps can cause misses. A locally linear section of nonlinear FM or other non-drone transmissions can also pass. This is not a LoRa/CSS decoder, vendor signature, link decoder or activity/flight-state detector.

`linear_sweep_review_v1` is metadata-only in the supervisor and separately reuses the frozen observed-quality/passband checks. It retains failed checks and never promotes sweeps beyond a generic shape diagnostic. Existing `expanded_measurement_review_v2`, `experimental_dsp_v1`, raw older measurements and classification stay unchanged. All named drone/link fields remain unknown. The new branch does not train a model, resume Mini 2 tuning or decode Remote ID.

Protocol **7** adds finite, bounded sweep measurements with exact partition/search counts, searched span/offset geometry, derived sweep widths, consistent flags, supported-held coordinates and duplicate/order checks. The 64 KiB reply ceiling, child 128 MiB/30 s limits and 2 s deadline stay unchanged. DSP runs only in the child. Automatic offline candidate preparation is not multiplied by this sweep search.

Offline `analyze --discover-sweeps` adds only `linear_sweep_discovery` and `linear_sweep_review`. `--expand-waveforms`, `--review-waveforms` and `--prepare-candidates` also include them on their existing analysis copy. Optional filtering reports original source coordinates and sample steps. Plain `analyze` and the legacy evidence flags retain their original outputs.

The GUI adds **Linear sweep shape** to the review summary and **General linear sweep measurements** with direction, slope, observed bandwidth/span, discovery/held coherence, fit error, held search count and rejection reasons. It explicitly identifies partial spans and held carrier refits. Both Wi-Fi tabs retain their runtime-off checkbox. The usual local GUI and matching protocol-7 helper are rebuilt together for the user's next launch; the current monitor is not restarted.

Incremental milestones:

| Milestone | State | Exit still needed |
|---|---|---|
| S1 bounded generic slope discovery and held slope checks | Implemented; software experimental | Independent captured sweeps, sensitivity/rejection qualification |
| S2 strict worker transport and GUI measurement visibility | Implemented; software tests pass | Receiver off/on/off latency, per-profile live budgets and soak |
| S3 broader confusable/impairment evaluation | Software evaluation only | Captured ordinary traffic, receiver backgrounds and full-search exposure calibration |
| S4 chirp/CSS or drone/link acceptance | Deferred | Independent references, symbol/recurrence checks and original field/family gates |

N4/N5/N7 remain partial and N6 activity context remains open. Next priorities remain coloured-background CAF significance, more robust low-SNR/clock/channel measurements and receiver-qualified negative controls before accepting any new labels.


### Sweep development evidence and limits

The initial raw-increment v1 development run is retained at [v1 results](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/sweep_breadth_verified_2026-10-05/summary.json), with exact source/script snapshots. It recovered 54/54 clean controls but 0/54 at either tested 30 or 15 dB SNR. That observed weakness motivated the v2 eight-increment average; thresholds were not relaxed. V2 reuses the development controls, so these results are **not an unseen holdout accuracy claim**.

The [v2 development report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/sweep_smoothed_2026-10-05/summary.json) covers **923 cases / 225 conservative source groups**. Every legacy JSON field and IQ checksum is unchanged, and all **695 prior v2 waveform reviews** are exactly preserved. Sweep derivatives share their three analytic-source groups; directions, bandwidths, spans and impairments are not independent physical drones.

| V2 control family | Cases | New raw sweep-shape matches |
|---|---:|---:|
| Clean repeated up/down sweeps; three partial-support lengths, three widths and random timing/carrier offsets | 54 | 54 |
| Same sweeps with 30 dB complex-noise SNR | 54 | 54 |
| Same sweeps with 15 dB complex-noise SNR | 54 | 23 |
| Same sweeps with a three-sample, amplitude-0.3 echo | 54 | 48 |
| Resampled sweeps, 2000 ppm clock stretch | 3 | 3 |
| Discovery-only sweeps / held opposite direction | 3 / 3 | 0 / 0 |
| Fresh white / AR-coloured stationary noise, shared rate/length derivatives | 256 / 256 | 0 / 0 |
| Received ordinary Wi-Fi, one receiver session | 3 | 0 |
| Sinusoidal nonlinear-FM controls | 3 | 0 |

Other tested tones, mixtures, FSK/OOK/PSK/spreading and CP controls produced no new sweep-shape matches. Two existing long sampled-chirp controls with alias resets did match local linear sections; that does not recover an unaliased physical RF sweep. Nonlinear FM or non-drone chirps can still be confusable outside this small generator set. All sweep matches remain generic shape diagnostics, often blocked by signal quality or unknown/declared coverage. Low-SNR misses remain substantial and there is no receiver/event probability calibration or family precision/recall.

Thirteen standalone and 27 registered opt-in CTests pass, including slope/residual direct oracles, noisy controls, phase/gain/support bounds, strict protocol failures and GUI checks for both Wi-Fi bands. ASan/UBSan sweep/review checks pass with leak detection disabled for the existing environment limitation. The usual GUI and matching worker are rebuilt; scanner-lane/panel checks pass. A [software-rendered ImGui preview](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/sweep_gui_2026-10-05/panel.png) was inspected. It uses a labelled synthetic GUI fixture, not a live drone capture.

The [nine full-budget noise worker runs](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/sweep_smoothed_resources_2026-10-05/summary.json) take 230.0–240.6 ms. The [six dense/three-slope full-budget runs](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/sweep_smoothed_positive_resources_2026-10-05/summary.json) take 226.6–248.7 ms at declared 20/60/200 Msps, including all three candidate slots in each of four tiles. These demonstrate software bounds on this host, not radio non-interference or live timing acceptance. No monitor/radio was launched or restarted, and no transmission was performed.

Reproduce the development report with:

```bash
python3 tools/cyclostationary/benchmark_sweeps.py \
  --tool build/cyclostationary-offline/wifi_cyclo_replay \
  --prior-review data/wifi_drone_analysis/review_fresh_2026-10-05/summary.json \
  --output /tmp/cyclo-sweep-new-results
```

The output directory must not already exist. Current code generates v2 results; historical v1 snapshots remain separate. Live receiver qualification, captured negatives, low-SNR/clock/channel breadth, recurrence/symbol checks and independent drone/link validation remain open.


## Fifth expansion: local cyclic-background support (6 October 2026)

`local_caf_background_v1` checks the already selected `bounded_caf_morphology_v1` rates/lags. It leaves that measurement, `expanded_measurement_review_v2`, the sweep branch and `experimental_dsp_v1` unchanged. A separate `cyclic_background_review_v1` exposes the additional support checks after strict child reply validation. Raw cyclic matches remain visible; no new waveform/drone/link acceptance is enabled.

The child reuses the existing disjoint first/last 1024–8192-sample support partitions and separate means. It uses the same normalized Hann-weighted lag products as the existing CAF branch. For each selected coarse cyclic bin it examines same-sign bins 4–20 bins away, excluding the existing DC/Nyquist guard. There are 17–34 reference bins for eligible peaks; the check requires at least 16. Reference coherence powers are sorted: the lower middle order statistic estimates the local median, and index floor(0.9 × (count−1)) supplies a local upper-background statistic. Selected fine-rate coherence is divided by each statistic with a fixed 1e−12 denominator floor. Ratios remain finite and bounded by 1e12; they are not probabilities or fitted noise laws.

Four contiguous portions of the same Hann-weighted partition measure demodulated lag-product phase consistency: squared magnitude of their complex sum divided by the squared sum of magnitudes. The minimum portion's weighted lag-product energy fraction measures support spread. These portions are **not independent captures, repetitions or emitters**. A consistent line needs discovery and held raw coherence ≥0.05, line/median ≥16, line/upper-background ≥8, block phase coherence ≥0.8 and minimum block product-energy fraction ≥0.02 in both partitions. These are engineering heuristics frozen before the broad evaluation, not calibrated CFAR or a false-alarm probability. No held frequency/lag is refitted.

FFT spectra are cached by lag/type within the job. At most 16 extra FFTs and eight peak diagnostics are produced per tile; allocations remain bounded. Protocol **8** validates exact partition/peak/reference/cache counts, finite ranges, derived ratios, block-energy bounds and support flags, while retaining the 64 KiB reply cap, 128 MiB child address-space limit, 30 s lifetime CPU cap, two-second deadline and existing copy/restart budgets. The supervisor performs metadata review only; the producer/GUI process no additional IQ.

Offline `analyze --check-cyclic-background` requests the existing waveform features plus the new background measurement/review. `--review-waveforms`, `--expand-waveforms` and `--prepare-candidates` include them on their existing native or explicitly filtered analysis copy. Source-coordinate and decimation-step mapping are exported. The search is not silently multiplied over automatically prepared candidate bands. Plain analysis and prior evidence flags retain their original output fields.

The native Wi-Fi panel shows **Cyclic background** counts of supported/raw ordinary and conjugate matches, followed by **Cyclic background checks** with line prominence, phase/energy support, reference counts and failed checks. Raw reviews remain intact and the new display is explicitly heuristic. Both Wi-Fi bands are covered. The usual GUI and matching protocol-8 helper are rebuilt together; source build flags and the runtime checkbox remain default-off. The current monitor is not restarted.

### Software results and qualification limits

The [development measurement report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/cyclic_background_verified_2026-10-06/summary.json) retains all 1,617 measurements and its original script snapshot. The [stationary-only aggregation](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/cyclic_background_stationary_2026-10-06/summary.json) corrects the statistical scope without rerunning DSP or changing any measurement. Negatives for another branch—such as opposite chirp slopes—can share legitimate cyclic rates and must not be counted as stationary CAF nulls. Both reports retain their provenance and input hashes.

The evaluation covers **1,617 cases / 263 conservative source identifiers**. Every baseline legacy JSON field and input checksum is unchanged. All **923 prior sweep measurements/reviews** and **695 historical waveform reviews** are exactly preserved. Rate/length/AR-strength derivatives share their source groups; analytic cycle kinds and translated-noise confusables share six seed groups. These are development controls, not independent real drones or field sessions.

| Control family | Cases | Raw cyclic-match cases | Background-supported cases |
|---|---:|---:|---:|
| Previous proper-complex white Gaussian nulls | 256 | 0 | 0 |
| Previous AR-coloured Gaussian nulls | 256 | 56 | 0 |
| Fresh proper-complex Gaussian backgrounds: rho 0/0.5/0.9/0.975/0.995, two lengths and two rate declarations | 640 | 74 | 0 |
| Targeted periodic envelope: clean / independent added noise / 2000 ppm resampling | 6 / 6 / 6 | 6 / 6 / 6 | 6 / 6 / 6 |
| Targeted conjugate BPSK line: clean / independent added noise / 2000 ppm resampling | 6 / 6 / 6 | 6 / 6 / 6 | 6 / 6 / 6 |
| Targeted discovery-only envelope / conjugate line | 6 / 6 | 0 / 0 | 0 / 0 |
| Translated real-noise confusables | 6 | 6 | 6 |
| Tone / two-tone beat confusables | 5 / 2 | 4 / 2 | 4 / 2 |
| Pulsed-noise confusables with real envelope cycles | 8 | 8 | 8 |
| Received ordinary Wi-Fi, one session | 3 | 0 | 0 |

The **1,152 stationary Gaussian cases represent 160 seed groups**: 53 groups have at least one raw match, and **0/160** pass the added background check. This is a whole-search result for the tested generator/profile mixtures, not receiver false-alarm calibration, field exposure or drone accuracy. No threshold was adjusted using the broad report. Genuine cycles in tones, pulses, translations and mixtures demonstrate why background support cannot establish a drone or protocol. Two opposite-held-slope controls retain cyclic support despite rejecting the sweep-specific slope hypothesis; the axes are separate.

Nearby cyclic lines can raise the local background and cause false rejection. Bursts limited to one portion, sparse signals, clock drift, window leakage, receiver artifacts and multipath can also fail the heuristics. A gated conjugate-line unit control is deliberately rejected on block support even though its raw line is real. Such misses require later ROI/receiver qualification; this delivery does not convert rejection to a calibrated negative drone decision.

Fourteen standalone / 28 opt-in CTests pass. Direct double-DFT order-statistic oracles, targeted cycles, full 16-FFT cache coverage, gated support, gain/finite/geometry bounds, strict transport mutations and both-band GUI checks are included. ASan/UBSan background/review checks pass with leak detection disabled for the existing environment limitation. The instrumented parent/protocol test passes using normal matched helpers: an ASan-instrumented child cannot initialize inside the unchanged 128 MiB limit, so limits were not relaxed. The default-off GUI/scanner regression and usual-build scanner/panel checks also pass.

The [nine full-budget noise process checks](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/background_resources_2026-10-06/summary.json) take 242.8–275.7 ms, and [six dense/three-slope checks](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/background_sweep_resources_2026-10-06/summary.json) take 246.5–281.9 ms at declared 20/60/200 Msps. These are offline helper timings on this host, not hardware non-interference acceptance. A [software-rendered ImGui preview](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/background_gui_2026-10-06/panel.png) is a labelled synthetic fixture. No monitor/radio was opened or restarted, transmitter used, model trained, Remote ID decoded or Mini 2 testing resumed.

Incremental milestones:

| Milestone | Delivered | Remaining exit |
|---|---|---|
| C1 local CAF spectral/phase support | Bounded measurement, numerical/control tests | Captured backgrounds, nearby-line/burst/receiver-artifact qualification |
| C2 separately visible rejection and GUI support | Supervisor metadata review and both-band panel | Operator/live usability and per-profile performance |
| C3 whole-search development exposure | 1,617 cases, 160 stationary seed groups, exact prior preservation | Receiver-calibrated thresholds, independent captured negatives and field duration |
| C4 accepted waveform or drone/link evidence | Deferred | Original reference/family/field gates; no generic cycle establishes identity |

N3/N4/N7 remain partial. Next work should qualify captured receiver backgrounds, improve sparse/burst/clock/channel support and measure live off/on/off budgets before accepting new labels or building N6 activity history. ML, Remote ID and further Mini 2-specific work remain deferred.

Reproduce the full development evaluation with a new output directory:

```bash
python3 tools/cyclostationary/benchmark_background.py \
  --tool build/cyclostationary-offline/wifi_cyclo_replay \
  --prior-sweeps data/wifi_drone_analysis/sweep_smoothed_2026-10-05/summary.json \
  --prior-review data/wifi_drone_analysis/review_fresh_2026-10-05/summary.json \
  --output /tmp/cyclo-background-new-results
```

`--reuse-measurements <previous-summary.json>` performs only aggregation, after verifying current tool and DSP source hashes, and records the source-report hash. It does not create fresh independent measurements.

## Contiguous burst expansion (8 October 2026)

`bounded_contiguous_burst_v1` addresses a middle-of-tile burst missed by the existing first/last whole-tile partitions. It selects the strongest eligible **retained** contrast-energy region, breaking equal-energy ties by earliest source offset. One region per tile and at most 16,384 samples are processed; long regions receive a central contiguous crop. Regions shorter than 2,064 samples are counted and skipped. Uniform-energy/context fallback regions do not repeat the whole-tile search. Other retained eligible regions are counted as budget-skipped; the earlier ROI report still discloses regions omitted by its eight-region cap. This is energy-biased selection, not calibrated activity detection or source separation.

The selected crop gets local 512-point PSD/quality (hop 257, at most 62 frames), ordinary/conjugate CAF and local-background checks, CP/Barker discovery, frequency/envelope morphology and general sweep measurements. Existing discovery/held partition geometry and thresholds are reused unchanged. No zero padding, burst joining, gap bridging, private frequency filtering or hardware changes are introduced. Each branch retains its original support and candidate bounds; a burst can be too short for a particular timing or cyclic rate even though it meets the routing minimum. The quality estimator internally computes one SCF alpha bin and discards its peaks; it provides PSD/quality context, while the separate CAF branch searches rates.

Whole-tile measurements, `experimental_dsp_v1`, previous reviews and classification remain separate. Live supervisor reviews use unknown receiver passband and ADC rails. Offline callers can declare geometry and int16 rails; filtered offline inputs retain the earlier whole pre-filter source rail check, which is disclosed. No acceptance is transferred from a local match to drone/link identity.

The worker protocol is **9**. Burst coordinates/counts/cropping must exactly match deterministic selection from the validated ROI context. All nested geometry, finite values, support predicates and resource counters are validated using the existing strict decoders. Local PSD fractions alone are transported as float32 (finite [0,1], sum tolerance 1e-6) to retain the unchanged 64 KiB reply ceiling; existing whole-tile values and new other metrics remain float64. Tiny float32 rounding at an exact quality boundary is possible. Four-tile stress metadata populates eight CAF peaks and all sixteen background FFTs in both scopes, three sweeps in both scopes, eight ROIs/three spectral intervals, spectra and timing/code results; the reply is **60,756 / 65,536 bytes**. That intentionally combined software metadata is a transport stress fixture, not physical waveform evidence.

The GUI adds **Burst-local waveform analysis** in both Wi-Fi bands, with source span, duration, local quality, crop/omission counts, separate pattern review, conflicts and background rejection reasons. Its immutable latest-tile/epoch/band rules and default-off checkbox remain unchanged. The usual GUI and nested helper are rebuilt together; an already-running monitor is not restarted.

### Software results and limitations

Artifact: `data/wifi_drone_analysis/burst_coverage_2026-10-08/summary.json` and its checkpoint records. The run covers **686 cases / 134 conservative source groups**, preserving every pre-existing JSON field and source hash in all 686 flag comparisons and **131** historical background measurements/reviews exactly. These are development controls, not independent field, drone or receiver accuracy.

- All **384** gated Gaussian interior controls (192 white, 192 AR rho .975; 32 base Gaussian seeds shared across kinds, lengths and declared rates) have eligible local regions; none passes local CP, Barker, sweep or background support. Raw coloured-noise cyclic confusables remain observable. Gating makes the complete capture nonstationary; this is not a stationary full-capture null or an event false-alarm estimate.
- Clean and 20 dB-noise CP 96+24 controls each recover **6/9** geometries. Their 4,096-sample cases lack eight complete periods per partition; 2,000 ppm resampled cases recover **0/9**. Sample timing robustness remains a next task.
- Clean/noisy Barker-8 controls each yield **9/9** compatible checks, but **all also match CP checks** and are reported as competing patterns. Their 2,000 ppm variants yield **0/9** code/CP matches despite retained cyclic diagnostics. No unique waveform is accepted.
- Linear sweeps pass **27/27** across clean, 20 dB-noise and 2,000 ppm variants. Two sweep cases also match generic CP timing, showing that CP checks are not unique to OFDM.
- Periodic envelope and conjugate-line controls each yield background support in **6/9** cases per impairment variant. At 4,096 samples the targeted rate falls into an excluded low-alpha bin under the unchanged CAF guard; longer support recovers it. This is a resolution limit, not a retuned threshold.
- Two-frequency shape controls pass **9/9** clean and **9/9** clock variants, but **0/9** noisy variants. This is shape compatibility; BPSK-derived confusables also produce two-frequency shapes.
- Three short controls and disjoint short bursts abstain; a discovery-only cycle fails held/background checks. A two-burst control selects one region and reports the other as skipped. All three tone controls pass cyclic background checks and remain quality-limited/confusable. Three received WLAN controls share one physical session and do not route a contrast burst; this does not establish performance on ordinary bursty WLAN.

Nine isolated full-budget controls use four independent tiles and four maximum 16,384-sample crops, at declared native 20/60/200 Msps. Long conjugate signals, three slopes, and eight eligible regions exercise bounded routing and omission counts. Round trips are **247.884–265.997 ms** under the unchanged 2,000 ms deadline and 128 MiB child cap. These are software measurements, not live scanner-latency or RF-rate acceptance. Artifact: `data/wifi_drone_analysis/burst_resources_2026-10-08/summary.json`.

Validation includes direct contiguous-crop equivalence, read-only IQ, uniform/short/constant abstention, deterministic ranking/cropping, noise controls, malformed wire coordinates/counts/PSD/support and pre-gap CLI routing. **15 standalone / 29 opt-in integration CTests** pass. ASan/UBSan burst tests and instrumented parent/protocol tests use normal capped helpers for transport; no sanitizer accommodation relaxes the live child limits. Usual GUI/helper and default-off GUI/scanner checks are recorded separately. A rendered synthetic panel preview verifies actual ImGui text/layout; no SDR or transmitter is opened.

| Milestone | Status / exit |
| --- | --- |
| BR1 — Bounded contiguous routing and local measurements | Delivered experimentally; crop/origin/support/omission controls and strict version-9 transport pass. |
| BR2 — Local GUI visibility | Delivered in both Wi-Fi views; conflicts, quality/passband uncertainty and background failures remain visible. |
| BR3 — Development breadth and resource evidence | Delivered: 686 additive controls, grouped Gaussian gates and nine capped full-budget requests; clock/noise/short-span limitations recorded. |
| BR4 — Receiver and event qualification | Pending: captured receiver backgrounds, weak/overlapping and real bursty traffic, independent acquisition conditions, live off/on/off scan/GUI measurements and soak. Drone-family acceptance stays gated separately. |

Next: qualify clock/sample-timing robustness and broader timing/code coverage offline; acquire receiver-background controls with the existing USRP when live testing is convenient. No new connection is needed for offline work. ML, RID, Mini 2 prioritisation and named-family acceptance remain deferred; N6 history/association has not been implemented by this slice.

Reproduce offline (does not open a radio):

```sh
build/cyclostationary-offline/wifi_cyclo_replay analyze --input CAPTURE --format cf32_le --sample-rate RATE --samples 65536 --analyze-bursts
python3 tools/cyclostationary/benchmark_bursts.py --tool build/cyclostationary-offline/wifi_cyclo_replay --prior-background data/wifi_drone_analysis/cyclic_background_stationary_2026-10-06/summary.json --output NEW_BURST_REPORT
python3 tools/cyclostationary/benchmark_burst_resources.py --tool build/cyclostationary-offline/wifi_cyclo_replay --output NEW_RESOURCE_REPORT
```

## Discovery-selected timing grids (8 October 2026)

`discovery_selected_timing_grid_v1` adds a separate diagnostic for sample timing drift/fractional timing in existing CP and fixed Barker-11 proposals. The original integer discovery bank, earlier reviews, measurements and classification remain unchanged. It runs once per tile: the selected contiguous burst if eligible, otherwise the whole contiguous tile. It does not multiply the search across all ROIs or candidate bands.

### Method and bounds

The first **two discovery-retained proposals per kind** seed nine fixed source-sample step grids: **−3,000, −2,000, −1,000, −500, 0, +500, +1,000, +2,000, +3,000 ppm**. The source mapping is `16 + (1 + grid_ppm*1e-6)*canonical_index`. The canonical length is fixed using the largest step and 16-sample margins at both source ends, keeping all trial mappings inside the same source. Original partition sizes are retained; fewer than two partitions plus a 16-sample guard after this shrinkage produces `insufficient_resampling_support`, with no padding or interpolation. Contexts without variation/proposals abstain.

Only the discovery partition is interpolated for all nine grids, using two adjacent source samples and separately removing its mean. Scaling depends only on the union of discovery interpolation support. CP geometry remains the seed's integer useful/prefix counts on the canonical grid; discovery phase is fitted using the earlier CP contrast procedure. Barker keeps the seed's integer chip count, fits discovery code/sample phase, and transforms/wraps the discovery-seed carrier on that grid. CP ranks discovery prefix/outside coherence contrast; code ranks discovery chosen/competing-phase contrast. Each kind retains one winner. **No held score selects a seed, grid, phase, geometry or carrier.** Only then is that winner's separate held partition interpolated, with canonical absolute origin preserved and the earlier support/coherence/contrast/cyclic guards applied.

Per tile: at most **9** discovery partitions, **18 CP trials**, **18 code trials**, **one retained CP and one retained code** explanation, and **11 × partition_samples** interpolated samples (at most **90,112**). Only first/last partitions are processed; no full retimed IQ array is created. Bursts/gaps are never phase-concatenated. CP/code winners can choose different grids; these are separate explanations, not a shared transmitter-clock estimate.

Linear interpolation can smooth or distort data and has no dedicated anti-alias filter. This private compatibility check is not promoted as an acquisition/preprocessing resampler; receiver settings, input IQ, producer processing, packet/security/identity results and capture schedule are unchanged. Near-Nyquist, wideband, arbitrary clock drift, fractional chip phases, different spreading codes, carrier errors, multipath and unproposed geometries remain limitations. The grid is neither a calibrated physical clock estimate nor a probability. Search expansion changes its null distribution; Gaussian development controls do not calibrate a receiver or an event false-alarm rate.

Supervisor review uses the **raw selected scope's PSD/quality before interpolation** and existing quality/passband/conflict checks. Live passband and ADC rails remain unknown. Raw and refined statuses are separate; no extra classifier/family or exact-modulation acceptance is enabled. The GUI's **Timing drift refinement** section shows scope, counters, selected grids, fractional source timing, train/held comparisons, ambiguity and rejection reasons in both Wi-Fi tabs.

Offline `analyze --refine-clock` follows the same burst-priority routing and exports integer source origin/step plus small fractional input-relative discovery/held coordinates. Keeping origin separate avoids inventing precision for large absolute offsets. `--analyze-bursts` remains optional output; its prior fields are preserved. Filtered inputs disclose the existing entire pre-filter source ADC-rail check. New worker protocol **10** validates the plan/counters, exact seed geometry, allowed grid/seed indices, finite normalized metrics, transformed carrier, complete support and derived flags. The maximum combined software metadata stress reply is **61,796 / 65,536 bytes**; child memory/CPU/fd caps, deadlines, copy pool and defaults remain unchanged.

### Development evidence

Artifact: `data/wifi_drone_analysis/timing_grid_2026-10-08/summary.json`, checkpoint records and the benchmark source snapshot. **1,196 cases / 161 conservative groups** preserve all earlier JSON fields and input hashes. **All 686 historical burst results** match the previous report exactly. No thresholds or grid were tuned on this evaluation. These are software development controls, not independent drone units, physical clocks, receiver calibration or field precision/recall.

Fresh positive derivatives share three software payload/noise seeds per kind. Grid controls use input warp −2,000/−500/0/+500/+2,000 ppm, 8,192/32,768 samples and whole/burst scopes; off-grid controls use ±2,500 ppm. Recovery counts below refer to raw pattern checks before quality/passband review, and expected geometry/chip counts also agree for the recovered intended patterns.

| Software control | Earlier integer checks | Refined checks | Remaining caveat |
| --- | --- | --- | --- |
| CP 96+24, fresh grid controls | 30/60 | 60/60 | Synthetic CP geometry, not protocol identity. |
| Barker-8, fresh grid controls | 20/60 | 60/60 | CP also matches in 16/60 refined cases; those are competing patterns. |
| Barker-16, fresh grid controls | 24/60 | 56/60 | Four misses remain; CP/code both match in 47/60 cases. |
| CP 96+24, ±2,500 ppm off-grid | 0/24 | 18/24 | Six misses; grid cannot establish the true clock. |
| Barker-8, ±2,500 ppm off-grid | 0/24 | 6/24 | Eighteen misses; no fractional-phase/continuous-clock bank. |
| Barker-16, ±2,500 ppm off-grid | 0/24 | 12/24 | Twelve misses; ten matching cases also have CP compatibility. |
| Earlier 2,000 ppm burst CP controls | 0/9 | 6/9 | The shortest regions still lack enough timing support. |
| Earlier 2,000 ppm burst Barker controls | 0/9 | 9/9 | Four also have refined CP matches. |

All **640 Gaussian controls** (384 earlier gated cases plus 256 fresh white/AR cases) across **48 conservative Gaussian seed groups** have no refined CP or code match. Two explicit discovery-only CP/code controls fail held checks. Both kinds remain confusable: two additional earlier linear-sweep cases acquire generic CP matches, and three received WLAN controls retain CP matches (one shared physical acquisition session). More CP compatibility does not establish OFDM, a drone link, or improved overall identification accuracy.

Direct source-pair and chip/word oracles check retimed train/held coherence and support. Replacing held data while preserving the same proposal context leaves seed/grid/phase selection unchanged and rejects the noise holdout. Read-only IQ, gain invariance, short/constant abstention, Gaussian controls and malformed plan/seed/grid/carrier/support/counter checks pass. There are **16 standalone / 30 opt-in integration CTests**, usual GUI/helper/scanner and default-off GUI/scanner checks. ASan/UBSan timing tests and instrumented-parent protocol tests pass with normal capped helpers; live child limits are not relaxed for sanitizer shadow memory.

Full-budget isolated worker exercises retain four independent tiles, 262,144 copied samples and the unchanged two-second deadline/128 MiB cap:

| Workload | Requests | Observed round trip |
| --- | --- | --- |
| Uniform software noise, declared 20/60/200 Msps | 9 | 229.574–242.281 ms |
| Dense/three-slope synthetic sweeps | 6 | 229.428–245.629 ms |
| Maximum 16,384-sample bursts / eight eligible regions | 9 | 259.123–328.406 ms |

Artifacts: `timing_noise_resources_2026-10-08`, `timing_sweep_resources_2026-10-08` and `timing_burst_resources_2026-10-08` under `data/wifi_drone_analysis/`. These measurements are software workload checks, not a comparison proving faster execution, live scan-latency acceptance or physical 200 Msps capture qualification. The synthetic actual-ImGui preview and verification record are in `timing_gui_2026-10-08`.

| Milestone | Status / exit |
| --- | --- |
| TR1 — Discovery-frozen bounded timing refinement | Delivered experimentally; independent source-level oracles, held-data replacement and strict protocol checks pass. |
| TR2 — Development breadth / earlier-output preservation | Delivered: 1,196 cases, 686 exact historical burst comparisons and recorded grid/off-grid misses/confusables. |
| TR3 — GUI and process/resource integration | Delivered in both Wi-Fi views; matched GUI/helper rebuilt, defaults/caps retained and 24 full-budget requests measured. |
| TR4 — Independent receiver / channel qualification | Pending: receiver backgrounds, real sample clocks, nearby/overlapping traffic, carrier/multipath/near-Nyquist cases, live off/on/off scan/GUI latency and soak. Family acceptance remains gated separately. |

Next work can broaden timing/code controls and off-grid/channel robustness offline. It does not need a new connection. Later receiver-background and live performance qualification can use the existing USRP when convenient; live Mini 2 testing remains deferred. N6 activity history and N8 independent drone/link validation are still open. No ML, RID, hardware acquisition or transmitter use was added.

Reproduce without opening a radio:

```sh
build/cyclostationary-offline/wifi_cyclo_replay analyze --input CAPTURE --format cf32_le --sample-rate RATE --samples 65536 --refine-clock
python3 tools/cyclostationary/benchmark_clock.py --tool build/cyclostationary-offline/wifi_cyclo_replay --prior-bursts data/wifi_drone_analysis/burst_coverage_2026-10-08/summary.json --output NEW_TIMING_REPORT
```
