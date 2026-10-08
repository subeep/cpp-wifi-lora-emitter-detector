# Next steps for broader cyclostationary analysis

Updated: 8 October 2026. This is a planning update following the user's request to stop prioritising the Mini 2 recording and improve the general analysis. Live Mini 2 testing is deferred until the user is ready. The implementation updates below record delivered slices of this roadmap.

Priority update (8 October 2026): the user now requests a bounded route to finish the link-identification segment before later ML. Follow the [link-identification completion plan](WIFI_LINK_IDENTIFICATION_COMPLETION_PLAN.md), milestones L0–L6, for the next delivery. Start with reference/profile qualification and candidate coverage, then implement only signatures needed for a small validated catalogue. The broader N1–N8 work below remains the backlog; it does not require completing every waveform branch before the first validated link release. Software readiness and independent/live acceptance are separate checkpoints, and no validation gate is waived.

The next priority is **qualified candidate bands → stronger cyclic measurements → calibrated rejection → broader waveform measurements → observed RF activity**. Named drone/link validation comes later. ML and Remote ID remain deferred. The existing acquisition schedule, receiver settings, packet/identity/security processing and stores remain unchanged; the add-on stays optional and default-off.

The current implementation priority is broader timing/code controls and off-grid/channel robustness, followed by receiver-background qualification and gap-aware RF activity. The burst-local slice and an initial discovery-selected timing-grid diagnostic are delivered. No new connection is required for those offline controls. USRP receiver-background captures and an off/on/off performance run will be needed before live qualification; the user's live Mini 2 trial remains deferred.

## Starting point

Delivered functionality includes ordinary nonconjugate spectral correlation, five OFDM timing hypotheses with held-out prefix checks, worker-local time/frequency ROI measurements, three fixed research chirp hypotheses, and experimental quality/passband checks. A bounded child process and native Wi-Fi measurement panel already exist. An additional dechirp refinement diagnostic is now available **offline only**; it is not used by the live child or classification policy.

These are experimental waveform measurements. They do not yet establish a validated drone family, controller/aircraft role, flight state, or a calibrated probability. The ROI measurements suggest where to look; they do not separate emitters. Broader waveform coverage and gap-aware activity tracking remain unfinished. Software isolation checks have passed; live receiver performance and soak qualification remain open.

The original [integration roadmap](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_DRONE_ADDON_PLAN.md) and its G0–G7 gates still apply. The milestones below break down the remaining M1/M2/R3 analysis work before family validation. Their identifiers do not replace the original release milestones.

Implementation update following this plan: a bounded N1/N2/N3/N4/N5/N7 slice now adds ordinary/conjugate CAF with fine cyclic-rate refinement and separate holdout, experimental envelope/frequency-state measurements, offline candidate preparation and a 107-case grouped benchmark. The [coverage/qualification record](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_WAVEFORM_COVERAGE.md) distinguishes delivered features, confusables/misses and remaining exits. No milestone or full waveform/family release gate is declared complete by this slice.

Second implementation slice: N5 now has bounded general CP lag/period/prefix discovery and fixed Barker-11 chip/carrier/code-phase compatibility, with discovery-only fitting and held checks. A 159-case / 83-group report records previous feature preservation and CP/spreading ambiguities. Protocol 6 adds these measurements to the isolated worker/panel without changing acceptance policy. N4 rejection/disambiguation, captured negatives, fractional/clock/channel qualification and frequency/envelope robustness are next; N6/N8 remain open. Detailed methods and evidence are in the coverage record above.

Third implementation slice: a separate v2 metadata review and front Wi-Fi GUI summary now expose support, quality/passband blockers and CP/Barker conflicts. Fresh coloured-noise tests expose raw CAF false matches; cycles/shapes remain diagnostics, with no raw threshold tuning or waveform/drone classification. The 695-case fresh review report and GUI/resource verification are linked in the coverage ledger. N4/N7 remain partial; coloured-background significance, profile-specific rejection, captured negatives and clock/channel robustness precede new accepted labels and N6 activity context.

Fourth implementation slice: bounded generic upward/downward linear-sweep shape extends W3 beyond the fixed slope templates. Separate held checks freeze slope/span while declaring position searches and carrier nuisance fits. A separate shape review and seventh front GUI row expose support and blockers; protocol 7 keeps existing resource ceilings. This is partial waveform breadth, not CSS decoding or identity. The S1–S4 incremental milestones and software evaluation are recorded in the coverage ledger. Coloured-background significance, receiver controls and low-SNR/clock/channel support remain priorities; N6/N8, ML, Remote ID and further Mini 2-specific work remain deferred.

Fifth implementation slice: N3/N4 gain a local cyclic-background diagnostic, and N7 gains a front support summary and detailed rejection reasons. Selected rates/lags are unchanged; local spectral prominence and within-window phase/energy support are checked separately. A 1,617-case development report preserves prior outputs and rejects observed stationary Gaussian cyclic confusables while retaining targeted periodic controls. Tones/mixtures and other real cyclic signals still pass; receiver-calibrated significance remains pending. Protocol 8 retains all original process/copy/deadline caps. The C1–C4 milestones are in the coverage ledger. Captured negatives, receiver-profile qualification, clock/channel robustness and N6/N8 remain open; ML, Remote ID and further Mini 2 work remain deferred.

Sixth implementation slice (8 October): `bounded_contiguous_burst_v1` reruns bounded measurements inside one strongest eligible contrast region per tile. It keeps independent discovery/held support inside that contiguous region, never concatenates bursts, caps a central crop at 16,384 samples, and reports short/budget-skipped regions. A separate GUI review preserves whole-tile results. The 686-case additive benchmark includes 131 exact historical background comparisons and 384 gated white/AR Gaussian controls with no local CP, Barker, sweep or background-supported match. Short-window and clock failures, CP/Barker ambiguities and tones remain unresolved. Protocol 9 keeps the existing resource caps; BR1–BR4 are in the coverage ledger. N6 activity history and N8 identity remain unfinished.

Seventh implementation slice (8 October): N3/N5 now include bounded fractional timing compatibility for retained CP/Barker proposals. Nine fixed grids and two proposals per kind select one discovery winner per kind, with frozen held checks, explicit interpolation/source coordinates and a separate GUI section. The 1,196-case report preserves all earlier fields and 686 historical burst results. Fresh CP/Barker grid controls show recovery gains; ±2,500 ppm off-grid cases, chip-16 misses, CP/code conflicts and chirp/CP confusables remain visible. Protocol 10 preserves resource caps; TR1–TR4 are in the coverage ledger. This is a timing compatibility hypothesis, not a calibrated receiver clock or drone/link acceptance. Broader code banks, receiver-background controls, N6 history and N8 identity remain unfinished.

## Milestones and order

| Milestone | Deliverable | Dependency | Exit evidence | Actual drone needed? |
|---|---|---|---|---|
| N1 — Baseline and qualification contract | Versioned capability/quality ledger and reproducible benchmark | Current implementation | Source grouping, missing metadata, coordinate/continuity rules and compute costs recorded; frozen legacy baseline | No |
| N2 — Qualified candidate preparation | Bounded ROI routing and private filtering | N1 | Independent filter/coordinate checks; mixtures and edge cases; selection misses and cost reported | No; receiver calibration can follow later |
| N3 — Stronger cyclic measurements | Fine cyclic-frequency search, stability measurements and conjugate-feature evaluation | N1/N2 | Independent mathematical oracle, off-grid/impairment tests and measured value within the budget | No |
| N4 — Rejection calibration | Frozen, profile-specific null/confusable evaluation of the whole searched detector | N1; repeat for each N2/N3/N5 revision | Fresh grouped tests; false matches, misses, unknown rejection and confidence bounds reported | No for initial work; representative background RF needed for deployment claims |
| N5 — Broader waveform bank | OFDM, spread-spectrum, FSK and general chirp branches first; remaining groups evaluated | N2/N3; N4 for promotion | Each group has measured/experimental/unsupported scope, captured controls and resource evidence | No for estimator development; real reference captures needed for support claims |
| N6 — Observed RF activity | Bounded burst/recurrence/frequency-change history with explicit gaps | Useful qualified N3/N5 outputs | Timestamped synthetic/replay oracle; no invented observations or associations across gaps | No for generic RF activity; yes later for validated drone activity |
| N7 — Optional live promotion | Useful branches enter the isolated worker and transparent panel | Per-branch N2–N5 gates; N6 if history included | Legacy equivalence, failure/load bounds; later G5/G6 receiver and soak checks | No for software checks; receiver required for live checks |
| N8 — Independent drone/link validation | Frozen family catalogue and held-out evaluation | Qualified features and adequate labelled captures | Applicable G2/G2A/G7 gates; otherwise family remains experimental/unknown | Yes, or suitable independently labelled recordings |

N4 starts early and continues alongside the other milestones. A new waveform branch can be developed without waiting for every other branch, but it must pass its own correctness/calibration/resource checks before live promotion. N8 does not block general DSP progress. No calendar release date is implied; benchmark results and available references determine the scope of each delivery.

## N1 — Establish the baseline and honest output meanings

1. Record each available measurement, its applicable sample rates, minimum duration/cycles, passband needs, search size, known confusables and current limitations. Distinguish a waveform group from a drone-link family and a physical device model.
2. Keep separate states for input invalidity, measured quality failure, insufficient support, unknown receiver qualification, no pattern match and experimental pattern match. A valid waveform measurement can remain useful when drone attribution is unknown.
3. Freeze the existing legacy features and classification outputs for comparison. New measurements must be additive, explicitly versioned and reproducible. Record executable/source hashes, parameters, input hashes and source sample coordinates.
4. Resolve source format/rate/passband metadata before physical timing claims. In particular, the local DroneDetect sample-rate declaration remains provisional; directory names and drone captions do not supply protocol truth.
5. Split by source recording, session and device where known before extracting windows. Keep derivatives and overlapping windows together; missing independence remains visible. The local DroneDetect interference conditions are not drone-free controls.
6. Establish per-branch timing, total request latency, memory and rejection/candidate counts at representative and worst supported rates. Compare native analysis, private preparation, exhaustive offline replay and unchanged live-budget replay separately.

**Deliverable:** a benchmark manifest, capability ledger and baseline report. **Exit:** a reviewer can reproduce the results and determine which claims each input can support without guessing its metadata or counting correlated windows as independent trials.

## N2 — Analyse qualified regions inside each copied tile

Use the existing time/PSD measurements to propose a small bounded set of candidate bands inside already admitted tiles. Retain a context path for diffuse, continuous and unfamiliar signals; selecting only obvious cyclic peaks would bias the later detector's measured recall.

- Prepare candidates on private worker/offline copies: explicit downmix, anti-alias filtering and only admissible decimation. Preserve original sample centres, filter support and discarded transients. Never concatenate discontinuous tiles or change source files.
- Record native versus prepared measurements and preprocessing decisions. Evaluate source-mean removal as a declared option with ablations; avoid universal notches or automatic removal that erases a legitimate signal component.
- Qualify digital edge occupancy, known ADC rails, clipping indicators, tones, DC, duration and continuity. Unknown float ADC scaling and analog passband stay unknown. A sample rate or digitally narrow PSD cannot certify analog capture coverage.
- Distinguish partial-band observations from complete waveform coverage. Band clipping must not generate a confident bandwidth/timing or family claim.
- Evaluate two-emitter overlaps, adjacent-channel traffic, wideband background, intermittent bursts and a strong tone masking a weak signal. A band proposal is not a separated transmitter; unresolved overlap must remain explicit.
- Bound candidate count, filter operations, scratch memory and dropped proposals before live use. Allocate within the current request budget rather than enlarging the capture pool or scanner dwell.

**Deliverable:** candidate preparation and routing report. **Exit:** correct source coordinates/passbands, independent filter checks, measured selection coverage and no regression of legacy outputs. Include candidate misses; improved scores on selected examples alone do not pass this milestone.

## N3 — Improve the cyclostationary core

The current coarse cyclic-frequency grid and ordinary SCF should remain the comparison baseline. Evaluate the following additions individually, then retain those with measured benefit:

1. **Bounded fine cyclic-frequency refinement.** Use a coarse search followed by a small search around candidate peaks or candidate symbol/chip rates. Report searched ranges, bin spacing, observation duration and meaningful resolution. A dense grid does not create information absent from a short observation.
2. **Two useful resolutions.** Compare short-window measurements for burst localisation with longer contiguous observations for rate discrimination. Allocate a fixed compute budget; do not introduce a full unbounded SCF image search. Long offline captures do not justify assuming longer live tiles.
3. **Conjugate cyclic features.** Add a separately specified research estimator with explicit conjugation, phase and frequency conventions, independent direct-sum checks and CFO sensitivity tests. Evaluate its additional separability on broad waveform controls before claiming support for particular modulations.
4. **Peak stability and support.** Record magnitude stability across disjoint eligible partitions, number of observed cycles, available samples and sensitivity to parameter changes. Choose hypotheses on a discovery partition and check them on another when duration permits. Too little support returns unavailable rather than a strong result.
5. **Safer normalisation.** Check denominator/power floors and masks so weak spectral bins or filtered edges cannot manufacture large coherence. Report the support behind a peak instead of only the maximum score.

Ordinary SCF, conjugate features, CP checks and PSD all reuse the same signal. Their agreement is useful consistency evidence, but must not be multiplied as independent probabilities or counted as independent votes.

**Deliverable:** estimator comparison with independent oracles, off-grid rates, CFO/clock drift, gain scaling, burst alignment and null controls. **Exit:** correct conventions/coordinates, declared failure modes and a demonstrated sensitivity or discrimination improvement at an acceptable cost. If a feature adds no measured value, leave it out of live processing.

## N4 — Calibrate false matches without building a model

Keep the current v1 heuristic policy frozen as the baseline. Develop new deterministic rules on development data, freeze them, then evaluate fresh seeds and recordings with group separation. This is statistical detector validation, not ML training.

- Test the **maximum produced by the complete search**: candidate selection, cyclic frequencies, spectral bins, timing/lag hypotheses, chirp slopes, positions and preprocessing alternatives. Calibrating one prespecified bin does not calibrate a blind search over thousands of opportunities.
- Use white and coloured noise, tones/two-tone beats, receiver DC/IQ artifacts, amplitude envelopes, bursty noise, ordinary Wi-Fi/Bluetooth/spread-spectrum traffic, repeated non-OFDM symbols, other chirps and overlapping signals. Finite bursts and envelopes can produce structure without establishing a drone link.
- Evaluate sample gaps, CFO, clock drift, multipath, changing SNR, partial passband, clipping, weak/strong overlaps and insufficient duration. Keep physically invalid inputs separate from eligible low-SNR misses.
- Calibrate only within explicitly described rate/duration/passband/search profiles. An untested profile remains experimental. Noise calibration establishes pattern false-match behaviour under those nulls; it does not establish the probability that a transmitter is a drone.
- Report matches/misses by source group and condition, conditional coverage, unknown rejection, confidence bounds and cost. Avoid a single pooled accuracy number dominated by easy noise windows. Captured deployment background and independent devices/sessions remain necessary for release claims.
- Record changes as new policy versions. Re-evaluate selection and final acceptance together; a tighter verifier can improve precision while discarding valid candidates.

**Deliverable:** frozen rejection policy plus development and fresh evaluation reports. **Exit:** false-match/miss tradeoffs and uncertainty are reviewable; no unsupported probability or named-family acceptance is introduced.

## N5 — Expand waveform coverage in two waves

Each branch must expose its measured structure and competing explanations. Generic waveform detection remains separate from drone/link identification.

| Group | Next measurement work | Conditions for useful output |
|---|---|---|
| W1 — OFDM | General bounded lag/symbol/CP search beyond the five existing WLAN timings; held-out prefix and cyclic-rate consistency | Enough symbols, appropriate passband and fresh timing controls; generic OFDM does not imply Wi-Fi generation or a drone vendor |
| W2 — DSSS/chip-spread | Candidate chip-rate cycles, spreading-related correlations and spectral-shape consistency | Captured chip-spread controls and noise/OFDM confusables; a peak alone cannot identify a code or protocol |
| W4 — FSK/GFSK/MSK-like | Frequency-state/deviation statistics, rate cycles and continuous-phase consistency | Account for noise, phase wrapping, CFO and channel distortion; preserve ambiguous labels |
| W3 — Linear chirp/CSS-like | General slope/duration/direction proposals, dechirp consistency and within-observation repetition where available | Include non-DJI chirps and mismatched templates; the existing three-template refinement remains an offline diagnostic pending calibration |
| W7 — FM/video-like structure | Frequency/envelope periodicity and documented structural timing where captures support it | Analyse RF structure only; no video reconstruction or inference of drone use from an FM signal |
| W5 — PSK/OQPSK-like | Conjugate/ordinary cyclic structure and phase statistics with explicit synchronisation assumptions | Enough support and channel/CFO checks; modulation order stays unknown unless separately validated |
| W6 — QAM-like | Applicable cyclic/rate features and amplitude statistics after qualified timing/channel handling | Do not assign constellation order from raw distorted IQ; mark unsupported where prerequisites are absent |
| W8 — ASK/OOK-like | Envelope levels, burst timing and rate/cycle consistency | Compare against AGC changes, pulsed interference and bursty noise |

Develop OFDM, DSSS/chip-spread and FSK first to diversify the present OFDM/chirp focus. Continue general chirp work as one branch, then FM/video structure. Evaluate PSK/OQPSK, QAM and ASK/OOK as the second wave. Gaps in real reference data should produce a partial/experimental status, not invented support.

Frequency agility/hopping and burst recurrence are behavioural attributes, not separate modulation classes. They belong in N6 and can accompany multiple waveform groups. Frequencies outside the current Wi-Fi receiver coverage remain outside coverage; this plan does not add sub-GHz, cellular or new scan bands.

**Deliverable:** per-group feature/oracle/control reports and a W1–W8 support matrix. **Exit per branch:** correct estimates, known alternatives, realistic impairment results, captured-reference scope and measured cost. No target link count is met by splitting one family into multiple aliases.

## N6 — Add gap-aware radio activity context

Build bounded descriptor history from accepted observations, using source timestamps/coordinates and session, epoch, band and receiver-profile identifiers. Define maximum records, retention and expiry before live use.

- Measure observed burst durations/duty within actual coverage, same-channel recurrence and within-observation frequency changes.
- Estimate periodicity only when several eligible observations provide adequate support. Keep scanner revisit intervals separate from transmitter recurrence. Short live tiles may not measure a long repetition period at all.
- Treat possible cross-channel associations as uncertain unless independent evidence supports them; similar bandwidth/rate is not a unique transmitter identifier. Do not infer a hopping emitter solely from successive swept-channel detections.
- Show observed/re-observed/stale states and coverage gaps. Band switches, worker loss, queue drops and reconnects invalidate unsupported continuity. Never join IQ phases across captures or infer disappearance/flight state from scan blind time.
- Emit generic RF activity until a family independently passes the drone/link gates. Drone-related activity and controller/aircraft roles require their own labels and evaluation.

**Deliverable:** replayable RF observation timeline and fixed event definitions. **Exit:** timestamped oracle cases survive gaps, switches, drops and overlapping sources without creating false continuity; history remains bounded and existing identity/security associations are unchanged.

## N7 — Promote useful work within the existing isolation boundary

Offline evaluation precedes live promotion. Keep new DSP in the child, with bounded/versioned replies and immutable GUI snapshots. The capture producer and GUI receive no new IQ processing.

Retain the current ceilings: eight 2 MiB queue slots; at most four independent tiles and 262,144 copied complex samples per capture; child 128 MiB address-space limit, 30 s CPU lifetime cap and two-second request deadline. The lifetime cap and deadline are containment limits, not a desired per-request cost. Keep cancellation, restart/recycle limits and drop-on-overload behaviour. Any new history/scratch allocations must fit the measured total budget.

Benchmark worst-rate inputs, mixtures and maximum search sizes. Under pressure, omit expensive optional refinements, report incomplete analysis and preserve capture/legacy work. Do not solve overload by increasing receiver dwell, sample rate, buffering or changing packet/security rules.

Software exits include disabled/healthy/overloaded/crashed/hung/malformed-reply equivalence, epoch/stale handling and bounded memory. Later receiver off/on/off runs and the existing G5/G6 performance/72-hour soak gates qualify advertised live profiles. A USRP may help those receive-only checks when the user is ready; actual drones are unnecessary for software containment tests.

**Deliverable:** opt-in worker/panel support for proven branches, with quality, support, alternatives and coverage shown. **Exit:** applicable G1/G3/G4 software gates followed by G5/G6 before a live performance claim.

## N8 — Return to named drone/link validation later

Park the supplied Mini 2 pilot. Retain its provenance and current results as development evidence; do not tune thresholds to force it to match or treat nearby sequential target-off traffic as a synchronised control experiment.

When the user is ready, record operating mode, unit, firmware, receiver profile, target/controller states, duration and independent observation logs. Collect ordinary confusables and repeated sessions. One Mini 2 validates a local pilot; independent-unit family support needs additional devices/captures and held-out evaluation. The original G2/G2A/G7 precision, coverage and field-exposure requirements remain unchanged.

Keep waveform, link family, drone assessment and role as separate outputs. A qualified generic waveform may be reported while the other axes remain unknown. Use no RID evidence or ML model in these milestones.

## First implementation batch after this plan

1. Finish N1: versioned support/quality ledger, reproducible cost baseline and a grouped broad-waveform/control manifest using existing recordings and independent synthetic oracles.
2. Implement N2 offline: bounded qualified-band preparation with raw/context comparison and source-coordinate checks.
3. Implement the first N3 slice: bounded fine cyclic-frequency refinement and held-out peak stability; evaluate conjugate measurements separately.
4. Run N4 fresh null/confusable evaluation on that exact pipeline, then start N5's general OFDM, DSSS and FSK branches. Promote only proven useful pieces through N7.

This batch can proceed using existing IQ and software-generated controls without waiting for the Mini 2. Obtaining captured ordinary-traffic controls and resolving source metadata can run alongside it. Synthetic controls establish estimator correctness; they do not replace captured-reference or field validation.

## Research basis and interpretation

Firecrawl was used to check primary literature for this update. SCF signal-identification research supports exploring cyclic features, but the evaluated cellular/CNN system does not validate our drone detector or supply transferable accuracy thresholds. Our bounded refinement and stability experiments are engineering proposals. [Spectrum Sensing and Signal Identification with Deep Learning Based on Spectral Correlation Function](https://arxiv.org/abs/2003.08359).

Published cyclic-correlation significance testing distinguishes ordinary and conjugate statistics. The cited multi-antenna method assumes a selected cyclic frequency, stationary Gaussian noise and a channel constant during sensing; its threshold is not automatically applicable to our blind single-channel search. N4 therefore evaluates the complete searched pipeline under its own declared conditions. [Multiple Antenna Cyclostationary Spectrum Sensing Based on the Cyclic Correlation Significance Test](https://arxiv.org/abs/1211.0313).

The three current chirp templates originate in research on particular commercial-UAV emissions. They justify an experimental feature branch, not universal DJI/OcuSync identity or guaranteed detection of the supplied Mini 2 capture. [Lightweight Detection of Inserted Chirp Symbols in Radio Transmission from Commercial UAVs](https://doi.org/10.3390/s25154552).
