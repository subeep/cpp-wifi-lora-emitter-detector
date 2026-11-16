# Universal drone classifier: change log

Execution log for [the plan](DRONE_UNIVERSAL_CLASSIFIER_PLAN_2026-11-12.md).
Checklist items are completed only when their stated acceptance criteria are met.
No commits are authorized. Locked DRFF-R2 recordings must remain unopened.

## 2026-11-12 — A1 complete

- Inspected the native worker, telemetry decoder, packet ingestion, track store and history view.
  The workspace already contained substantial uncommitted work; it is being preserved.
- Added worker emission of height, altitude, pilot/home coordinates and horizontal ground
  speed for successfully parsed telemetry. Invalid or unset coordinate pairs and zero scalar
  values are omitted. Existing packet identity and authentication semantics remain unchanged.
- Added `motion_altitude_units_verified: false`: the vendored parser explicitly describes
  motion/altitude scaling as provisional until checked against a local flight.
- Version 3/padded records retain their existing identity-only behavior; the vendored telemetry
  parser does not support their telemetry layout. No unverified layout is inferred here.
- Worker tests now check emitted fields against the decoded bytes of both recorded fixtures,
  including omission of unset fields, and check the Air 2 pilot coordinates explicitly.
- Extracted the existing history renderer unchanged into `src/drones/drone_tracks_panel.hpp`
  so the app and GUI test exercise the same view. The GUI test replays the actual Air 2 worker
  packet through packet ingestion, detections and tracks in an isolated test store. It renders
  the history and verifies `Pilot 51.446230, 7.267104` and the unauthenticated attribution.
  The synthetic acquisition identity is confined to the test; offline data is not fed into live state.
- Validation: `drone_native_worker`, `drone_tracks`, `droneid_packet`, and `dji_addon` CTest
  cases passed. `rf_monitor_gui` built successfully. GUI verification used the existing
  offscreen ImGui/EGL test, not a live receiver or flight.
- A1 checked complete. The parser's version/layout and provisional-unit limitations above
  remain explicit; this step does not validate telemetry against physical flight measurements.

## 2026-11-12 — A2 complete

- Began a metadata-only survey of dataset paths and existing review artifacts. No locked IQ
  was opened. The old corpus review samples only some archive members, so it cannot serve
  unchanged as the requested universal inventory.
- Added `tools/drone_ml/universal_inventory.py`, producing
  `data/drone_ml/2026-11-12/universal_classifier/inventory.json`: 7,050 recording entries,
  counts per source, model, aircraft group and type, and 127 locked entries marked unreadable.
  Entries include archive members; excluded/duplicate candidates are still listed and flagged.
  Incomplete DroneRFa is recorded separately. Excluded FPV metadata remains explicitly unknown.
- SourceGuard refuses locked names, resolved aliases, hardlink aliases and unsafe archive
  members before opening. Tests mock opening to prove locked-path rejection happens first.
  The inventory reads directory listings, archive directories, recorder sidecars and existing
  audited metadata; it does not open IQ. Cached dataset metadata is checked against source
  size and modification time. No existing training scripts have been authorized to read locks.
- Included October Air 3S sessions and four original continuous Air 3S captures, November X310
  sessions, and the recorded Phantom 4 Pro captures. The latter include overflow/gap flags
  and four views below 20 MSps, excluded from candidate training. November 12 live checks
  retained no IQ, so they cannot supply new training examples.
- Findings: 33 RFUAV entries with archive/XML label conflicts; nine Mini 3 entries with
  mismatched pack names; 71 possible extracted Leuven duplicates; 60 Leuven Mavic entries
  with unresolved model naming; 13 DAUTEL entries needing alias/type review. These remain
  excluded from draft clean model candidates. Scene approval is incomplete for own-X310 data.
- Validation: metadata inventory generation succeeded; four Python tests passed (locked
  boundaries, aliases, unresolved types, and aircraft-independent reporting eligibility).

## 2026-11-12 — A3 prepared, awaiting user review

- Added `tools/drone_ml/universal_classes.py`, producing `classes.json` beside the inventory
  and [a readable review](DRONE_UNIVERSAL_CLASSIFIER_CLASS_REVIEW_2026-11-12.md).
- Proposed 60 model classes plus background: eight eligible for a multi-aircraft split,
  52 conservatively limited to unit-match reporting. Eligibility requires at least three
  explicit unit identities within one source; dataset names alone cannot establish independence.
  B4 must still implement and verify actual splits before any accuracy claim is made.
- Every class has its type reason and source counts. Types remain provisional until presence
  measurements; controller-only recordings cannot inherit OFDM positives from aircraft labels.
  All records remain `training_eligible: false` during preparation.
- Review needed: the plan's two flags do not cover the two-aircraft Mini 3 Pro case. Proposed
  `single_unit` means the conservative reporting tier here, not a claim of exactly one aircraft.
  Cross-dataset unit independence and unresolved model/type assignments remain disclosed.
- A3 is deliberately unchecked because its completion criterion includes user review.
  A4 and later steps have not started. No classifier training, app integration or sealed-test
  execution has occurred, and no commit was made by this task.

## 2026-11-12 — A3 approved; A4 and A5 complete

- User replied `continue` to the concrete class review. Marked A3 reviewed and retained the
  accepted class/type proposals, exclusions and conservative unit-match reporting tier.
- User additionally confirmed `yeah no drones were nearby` for the November 10 background
  scene. Recorded this exact statement in a local confirmation artifact; antenna position
  remains unknown. The original source `scene.json` was not rewritten or filled with guesses.
- Added `universal_background.py` and `background_split.json`: November 10 provides 237 valid
  one-second training captures (15 faulty captures excluded); the confirmed October 23
  background provides 13 held-out captures, totalling 3.25 s, covering 2.4, 5.2 and 5.8 GHz.
  Whole sessions, resolved source paths and capture hashes must not overlap between parts.
- Four split tests pass, covering session leakage, copied-capture leakage, and missing bands.
  Existing capture hashes are recorded, not described as freshly recomputed. Source sizes and
  November sidecar/session hash agreement were checked.
- A4 meets the session-split and band-coverage criterion. The held-out part is development
  data already seen in diagnostics; it is neither untouched final evaluation nor enough data
  for a <=1 false row/hour claim. C4 still needs separate calibration and untouched evaluation
  sessions of adequate duration. Current scan centres and October centres are not identical.
- A5's on-disk requirement is met by the October 23 Air 3S live-feed session and confirmed
  same-day no-drone session. Existing validation reports record their provenance and integrity.
  This does not establish generalization to flight or other aircraft.

## 2026-11-12 — B1 in progress

- Added guarded readers for DRFF-R2, DroneRFb-DIR, Leuven, RFUAV, Tampere and own-X310 IQ.
  Locked paths are rejected before reading, including aliases; excluded class-review entries
  are not processed. Recording-context labels are not treated as per-window presence labels.
- Added 8 ms / 20 MSps views using the same Kaiser-8.6 polyphase resampling family as existing
  preprocessing. Views preserve physical amplitude, use 100 us real filter support on both
  sides, and span guarded overlapping offsets. Three spaced intervals are sampled per file;
  RFUAV sampling is explicitly bounded to the first 160 ms of each archive member.
- Each record stores source identity/signature, interval offsets, view centres, samples, output
  hashes and a version. Resume refuses changed inputs/configuration or source signatures.
  A per-class count and source-failure report are written. Dataset-reader smoke validation is
  underway before constructing the Storage-disk window store.

### B1 validation and full build started

- Eight-record smoke build covered all six eligible dataset families and both native own-IQ
  layouts: 108 views, no reader failures. Three resampling tests passed (frequency translation,
  physical amplitude/native sample preservation, and invalid-input rejection).
- Full Storage build started at
  `/media/sudeep/Storage/drone_universal_classifier/2026-11-12/views-v1` after sandbox escalation
  was approved. Expected: 6,399 recording entries, 101,175 windows, about 120.61 GiB uncompressed.
  Source recordings are not modified. B1 remains unchecked while this build is running.

### B2/B3 implementation prepared during the B1 build

- Added `universal_features.py`: 29 measured facts (named-lag ratios/values, six blind peaks,
  occupied bandwidth, flatness, burst duration/period and duty cycle), with missing-value masks.
  Timing uses exactly the first 131,072 samples, aligned with the production C++ probe.
- Added the offline `drone_symbol_timing_probe` C++ adapter and `universal_feature_parity.py`.
  Timing parity passed on 108 recorded views spanning all six sources: max absolute difference
  0.000001923, no non-boundary flag mismatches. Timing parity does not claim a C++ equivalent
  for envelope/bandwidth features; these have synthetic tests instead.
- Added `universal_presence.py`. Own-family timing may produce context-model positive labels;
  absence of that timing stays unknown, never automatically negative. Confirmed X310 background
  retains its scene label. Other background contexts need sample review before negative use.
- Non-OFDM classes remain unknown because RFUAV, Leuven and Tampere have no matched source
  no-signal controls in this inventory. Asked the user whether to keep these unlabelled,
  use explicitly provisional labels, or wait for controls; no decision is inferred from silence.
- Nineteen preparation tests pass. The eight-record/108-window presence smoke pass completed.
  No 20-per-class spectrogram review is claimed yet, and no model training has begun.

### First B2 visual review (partial)

- Generated and visually inspected 20 spectrogram windows each for Air 2, Air 2S, Mavic 3,
  Mini 3 Pro, Mini 4 Pro and Mini 5 Pro: 120 windows across six models. Saved the exact sample
  identities, timing ratios and per-class review notes in `review-drff/manifest.json`.
- Corrected the initial plot scale before review: normalized spectral power needed a -40 to
  +10 dB display range; the initial range saturated most panels. Final sheets have a shared
  labelled scale. Plotting packages were installed only in `/tmp/universal-review-plotting`.
- Positive views generally show burst/gap structure. Many unknown views also contain structure,
  so they must not be relabelled as negative. Examples with possible partial-view or late-burst
  probe misses were recorded (Mini 3 Pro 35f561:5, Mini 4 Pro 2a3c82:13 / 8769de:10,
  Mini 5 Pro 2af979:7). Visual structure alone does not establish the class's link.
- No labels were overridden from these plots. This is only six of the universal classes;
  B2 remains incomplete and no training is authorized by the visual review.

## 2026-11-13 — B1 resumed after approval-review interruption

- On November 12 the serial B1 job was deliberately stopped after more than 600 records to
  change only the scheduling to four workers. Reader, selection and resampling ASTs were
  verified unchanged; all 108 serial/parallel smoke views were bit-identical. The original
  serial implementation is retained in the local artifacts for provenance.
- The first restart was **not executed**: automatic approval review failed because the usage
  limit prevented the review from completing. This was not a determination that the action
  was unsafe. Completed files remained on Storage; B1 was not running during the interruption.
- Following the user's November 13 `continue`, the same authorized restart passed review and
  resumed. Existing source signatures/output sizes and input/configuration hashes are checked;
  the explicit scheduling-only migration is recorded beside the window store.
- B1 remains unchecked until all eligible records finish and its final report is verified.
  No interpretation of `continue` supplies missing no-signal controls or turns unknown
  windows into training labels.

### November 13: presence resume and additional visual review

- Presence/feature extraction now uses four workers and resumes completed records only when
  the view hash, extractor/decision code hash and cached feature-file hash agree. While B1 is
  running, only records whose metadata has been published are included in each snapshot.
- Updated presence/feature tests pass (eight targeted cases). A full-store snapshot pass is
  running; its report will remain explicitly partial until the complete B1 store is processed.
- Visually inspected another 100 windows: 20 each for DroneRFb A/C/D/F and Mini 3. Exact
  selections and notes are in `review-dir/manifest.json`. Total inspected so far: 220 windows
  across 11 model classes. No labels were overridden; unknown structured windows stay unknown.
- The uniform-index contact-sheet sample can repeat the same frequency-view offset. It is
  a visual spot check, not an unbiased estimate of presence share; the latter comes from the
  full per-window label counts. Spectrogram appearance alone does not authenticate a model.

- Disk throughput, rather than feature arithmetic, is limiting the concurrent passes. The
  feature pass was temporarily suspended (SIGSTOP, PID 1253935) to prioritize B1 disk I/O.
  It must be resumed with SIGCONT after B1; its execution session is still retained. This
  is a scheduling pause of a subsidiary process, not completion or cancellation of B2.

### Final B1 verification preparation

- Added `universal_verify_views.py` to check exact eligible-record/output coverage, immutable
  input-manifest hashes, source size/mtime, every output array's shape/type/size and frequency
  metadata, and agreement with the final per-class window counts. One complete persisted
  output record per class is additionally checked for finite samples and its IQ checksum.
  This verification does not reopen original IQ or claim a full re-hash of the entire store.
- Five verifier tests pass, including same-size IQ corruption, changed input manifests,
  orphan IQ output and incomplete-build rejection. B1 remains pending the real final report.

## 2026-11-13 — B1 complete (taken over by the first agent)

- The B1 build finished at 11:33: 6,399 of 6,399 eligible records, no errors. Ran
  `universal_verify_views` on the real store: passed, 101,175 windows, 129,504,819,072 bytes,
  counts per class agree with the build report, one full record per class re-hashed. Result in
  `data/drone_ml/2026-11-12/universal_classifier/views_verification.json`. No original or locked
  IQ was opened by the verifier. B1 ticked.
- The suspended presence/feature process (PID 1253935) no longer existed, so it could not be
  resumed with SIGCONT. Restarted `universal_presence` on the complete store with 8 workers; it
  reuses the 174 finished records (same view and code hashes). Log:
  `data/drone_ml/2026-11-12/universal_classifier/presence_run.log`.
- Class sizes in the store are very uneven: from 21 windows (Mavic Pro, Phantom 4, Mavic Mini,
  Parrot Disco, Matrice 100) to more than 12,000 (Air 2S, DroneRFb types). B4 and the sampler
  must account for this; classes with 21 windows come from a single Tampere file.
- Pending inventory amendment, **not applied** (it would change the input manifests the store
  is tied to): the 60 KU Leuven "Mavic" entries are a Mavic 2 Enterprise Advanced by their own
  DroneID (product code 69 in three files; `data/drone_ml/2026-11-12/kuleuven_decode_01`). To
  be applied with the next store version, together with DroneRFa (E4).

### November 13: visual review of 20 more classes (first agent)

Generated sheets for all 50 classes not yet reviewed (`review-rest/`) and inspected 20 of
them, 20 windows each: Mini 2, Matrice 300, Inspire 2, Avata 2, Air 3S, FPV, Mavic 3 Pro,
Mavic Pro, Matrice 100, Matrice 210, Phantom 4, Phantom 4 Pro+, not_a_drone, SJRC F11 Pro,
Futaba T14SG, RadioMaster TX16S, Herelink Hx4, Parrot Disco, Yuneec Typhoon H, Mavic Mini.
Total inspected: 31 of 61 classes. No label was overridden. Findings:

1. **Positives look right where the link is inside the view.** Mini 2, Matrice 300, Inspire 2,
   Avata 2, Air 3S, Mavic 3 Pro and Phantom 4 `signal_present` windows show the expected
   wideband bursts or blocks.
2. **Many `unknown` windows hold the link.** Avata 2 especially: at least four of 13 unknown
   windows show the same burst train as the positives. Short O4 bursts fill little of the
   6.5 ms timing block, so the ratio stays under the threshold. They stay unknown; a
   burst-aware positive rule would recover training data.
3. **Edge-only positives.** Matrice 210 and Phantom 4 Pro+ (Tampere) positives show only thin
   strips at the top and bottom edges of the view: the link sits between two view offsets and
   only its skirts are inside. They are poor training examples. Matrice 100 has no positive at
   all for the same reason. The next store version should add a view centred on the measured
   signal for single-file classes.
4. **One positive with nothing visible** (FPV 0ed1a0:20). The timing test can fire below the
   visible noise level; kept, but noted as unverifiable by eye.
5. **Receiver artefacts identify the source.** KU Leuven views carry a constant narrow line at
   -5 MHz or 0 MHz from view centre (the recording's own centre spur); several Tampere files
   contain all-zero stretches. A network can learn these as "this dataset". Needs a notch or
   augmentation before training, and a per-source confusion check afterwards.
6. **RFUAV is not a clean room.** The Futaba T14SG sheet shows 20 MHz Wi-Fi-like bursts,
   Bluetooth-like hops and chirp trains beside the controller's own narrow bursts. Energy
   alone therefore cannot label RFUAV controller windows. KU Leuven is anechoic by its README,
   and its controller windows show only narrow bursts and noise, so an energy rule is
   defensible there.
7. **Two type assignments look wrong on the data.** SJRC F11 Pro (typed `wifi_drone`) shows
   narrow hopping bursts in these 2.44 GHz recordings: the control link, i.e.
   `hopping_controller` for this source. Herelink Hx4 (typed `hopping_controller`) shows
   wideband OFDM-like bursts about 13 MHz wide with a comb structure: not a hopper. Types
   were declared provisional until this step; both to be corrected with the next class file.
8. **Tampere Wi-Fi drones are weak at 20 MSps.** Parrot Disco and Mavic Mini windows are
   mostly noise with faint bursts; 21 windows each from one file.

## 2026-11-13 — B2 and B3 complete for the first-classifier scope

- **User decision:** "KU Leuven now, rest later". Non-OFDM classes recorded in the KU Leuven
  anechoic chamber are labelled by energy; RFUAV and Tampere non-OFDM classes stay unknown and
  are out of the first classifier.
- Presence/feature pass finished on the complete store: 6,399 records, 101,175 windows
  (`presence-v1/report.json`).
- Added `tools/drone_ml/universal_presence_energy.py` (overlay, reads cached facts only) and
  two tests. Rule: duty cycle > 0 gives `signal_present`; otherwise spectral flatness >= 0.75
  gives `source_noise`; otherwise `unknown`. On the 20 reviewed SJRC windows it agreed with the
  spectrograms in all 20 (6 with bursts, 14 noise). Result in `presence_energy_overlay.json`:

  | Class | Present | Source noise | Unknown |
  |---|---|---|---|
  | Nine Eagles | 520 | 551 | 0 |
  | SJRC F11 Pro | 279 | 792 | 0 |
  | Spektrum DX4e | 198 | 1,293 | 0 |
  | Taranis Q X7 | 243 | 824 | 4 |
  | WLtoys | 791 | 274 | 6 |
  | Q205 | 0 | 1,071 | 0 |

  Q205 has no window with a signal in the sampled intervals, so it cannot be a class.
- Presence share by timing for the OFDM classes (windows present / total): Air 2 242/1,554,
  Air 2S 359/12,648, Air 3S 28/108, Avata 2 294/924, DroneRFb A 196/13,740, C 1,565/12,090,
  D 295/12,135, F 272/10,125, FPV 369/798, Matrice 300 271/2,982, Mavic 3 279/903, Mavic 3
  Pro 117/378, Mavic Pro 6/21, Mini 2 526/2,982, Mini 3 1,770/10,383, Mini 3 Pro 182/966,
  Mini 4 Pro 442/1,617, Mini 5 Pro 155/903; Lightbridge: Inspire 2 764/3,069, Matrice 100
  0/21, Matrice 210 8/87, Phantom 4 3/21, Phantom 4 Pro 2/63, Phantom 4 Pro+ 14/87.
  Background: 750 confirmed X310 windows; 1,014 dataset background windows remain unknown.
- Reviewed six more sheets (Q205, WLtoys, Nine Eagles, Taranis Q X7, Spektrum DX4e, Phantom 4
  Pro): 37 of 61 classes inspected, which covers every class in the first-classifier scope.
  The KU Leuven energy labels agree with the visible bursts; one faint Taranis burst was
  labelled noise.
- Open findings for B4 and C1:
  - Five Lightbridge classes have 0 to 14 positive windows; only Inspire 2 has enough to train.
  - The user's own Phantom 4 Pro recordings show a wideband signal in most windows but the
    32.8 kHz test fires in 2 of 63; the recordings have gaps and overflows. The positive rule
    for Lightbridge on X310 data needs a separate look.
  - Air 2S (3%) and DroneRFb A, D, F (1–3%) have very low presence shares for their size.
- B2 ticked for the first-classifier scope; the 24 deferred classes are neither labelled nor
  reviewed. B3 ticked: timing parity with the C++ probe passed on 108 views (other agent's run,
  `feature_parity.json`); bandwidth and envelope facts have synthetic tests only.

## 2026-11-13 — B4 splits and C1 type-only baseline

User decision: "Yes, go ahead" (B4 and C1). Nothing committed.

**B4.** `tools/drone_ml/universal_splits.py` (+ test) writes `splits.json`: 14,674 windows —
train 8,018, val 1,257, test 1,742 (aircraft never trained on), unit_test 2,119 (new recordings
of a single training unit), cross_test 189 (same model, other dataset), open_test 1,310 (models
never trained on: Air 3S, Mini 3 Pro, Taranis Q X7, Mavic Pro, Matrice 100/210, Phantom 4 /
4 Pro / 4 Pro+), bg_test 39 (held-out X310 background). Q205 dropped (no signal). The script
refuses any aircraft or recording on both sides. B4 ticked.

**C1.** `tools/drone_ml/universal_cache.py` (128 x 156 pooled spectrogram + 29 facts, cache on
the Storage disk) and `tools/drone_ml/universal_train.py`. Four types: dji_ofdm, lightbridge,
hopping_controller, not_a_drone. CPU, 14 epochs, best epoch on val balanced accuracy.
Outputs: `data/drone_ml/2026-11-13/universal_c1/{full,spec_only,facts_only}/` (development
only, not deployable).

Balanced type accuracy, range over seeds 1–3:

| Partition | Full | Spectrogram only | Facts only |
|---|---|---|---|
| val | 0.990–0.997 | 0.980–0.986 | 0.983–0.984 |
| test (unseen aircraft) | 1.00 | 0.993–0.997 | 0.997–0.998 |
| unit_test | 0.990–0.999 | 0.979–0.987 | 0.983–0.987 |
| cross_test | 0.989 | 0.968–0.995 | 0.979–0.989 |
| open_test (unseen models) | 0.658–0.687 | 0.613–0.705 | 0.785–0.844 |
| bg_test (39 windows) | 0.974 | 0.590–0.769 | 0.974 |

Per-model results on the held-out partitions (seeds 1/2/3):

| Model (partition, windows) | Full | Spectrogram only | Facts only |
|---|---|---|---|
| Air 3S (open, 28, own X310) | .96/.93/.96 | .54/.54/.46 | .96/.96/.96 |
| Mini 3 Pro (open, 182) | .99/1.0/1.0 | .99/.99/.99 | .99/.98/.98 |
| Mavic Pro (open, 6) | 1.0 all | .33/1.0/1.0 | .67/1.0/1.0 |
| Taranis Q X7 (open, 243) | .71/.76/.64 | .81/.89/.58 | .70/.63/.46 |
| Matrice 210, Phantom 4, 4 Pro (open, 8+3+2, Lightbridge) | 0 | 0 | 1.0 |
| Phantom 4 Pro+ (open, 14, Lightbridge) | 0 | 0 | .43 |
| x310_background (bg_test, 39) | .97 all | .59/.72/.77 | .97 all |

Findings:
1. **The DJI OFDM result is not only circular.** The spectrogram-only model never sees the
   timing ratio the labels came from and still scores 0.99 on unseen aircraft. On public data
   the spectrogram carries the DJI signal shape.
2. **The spectrogram does not transfer to our receiver.** Spectrogram-only gets Air 3S (own
   X310) right in about half the windows and calls 23–41% of X310 background windows drones
   (mostly dji_ofdm). 27 of the 39 background windows hold bursts (Wi-Fi/Bluetooth-like), so
   this is the false-alarm case that matters live, not empty noise. The facts keep it at 1/39.
3. **Lightbridge is circular.** Only the facts-only model recognises unseen Lightbridge models
   (Tampere and own X310), because the label and the 32.8 kHz timing fact are the same
   measurement. The full model ignores it and follows the spectrogram, which learned Inspire 2
   from one source and calls Tampere Lightbridge dji_ofdm or not_a_drone. This supports the
   agreed order: link family from timing is a deterministic step before the classifier, and
   the classifier must not overrule it.
4. **Unseen hopping controllers are weak:** Taranis Q X7 0.46–0.89, partly called
   not_a_drone. Only four hopping classes from one chamber are in training.
5. **No per-hour false-alarm figure is possible:** the held-out background is 3.25 s. C1
   stays open for that row.

Next (agreed with the user): record 30–60 min of no-drone X310 background across the
monitored bands for a real bg_test; then C2 with the family mask, augmentation against the
dataset fingerprints (KU Leuven spur line, Tampere zero stretches, gain and frequency offset),
mixing drone bursts onto X310 background, and more non-drone burst negatives.

## 2026-11-13 — no-drone X310 background recording

User: "ok go ahead" (record background). 60 min receive-only, no drone powered (user confirmed),
gain 20 dB as the live app. 556 clean 0.5 s captures across 4 slots at 2.4 GHz and 7 at 5.8 GHz
(~34,750 windows, 22 GB on the Storage disk); 65 captures lost to radio-open errors. 2410 and
2430 MHz clip on a strong local Wi-Fi signal in most captures (flagged). Details:
`data/drone_ml/2026-11-13/background_x310/README.md`. Next: cut into windows, run presence and
facts, and score the C1 models on it for the per-hour false-alarm row.

## 2026-11-13 — C1 models scored on the 60-minute X310 background

User: "Yes, score it". `data/drone_ml/2026-11-13/background_x310/score.py` cuts the 556 clean
captures into 34,472 back-to-back 8 ms views (275.8 s of air; 13,531 views with bursts; 5,642
clipped) and runs all nine C1 models. Result: `c1_scores.json`. Any type other than
not_a_drone counts as a false alarm; no threshold or aggregation (that is C4).

| Model (seeds 1/2/3) | False views | Views with bursts | Captures with any false view |
|---|---|---|---|
| Full | 9.1% / 7.0% / 10.4% | 20% / 16% / 22% | 389 / 306 / 315 of 556 |
| Spectrogram only | 22% / 16% / 16% (almost all dji_ofdm) | 49% / 36% / 35% | 357 / 295 / 238 |
| Facts only | 10% / 12% / 11% (hopping, Lightbridge, DJI) | 21% / 26% / 24% | 399 / 415 / 412 |

- 2.4 GHz is the problem: full-model false rates 2–42% per slot at 2.4 GHz (2430 MHz worst,
  36–42%), 0.6–3.5% at 5.8 GHz. Clipped views are false about 2.5x as often as unclipped.
- The 39-window bg_test (0.974) badly understated this. **C1 fails the proposed gate (<=1 false
  row per hour) by orders of magnitude**: at ~7% of views, a raw per-view classifier would flag
  thousands of views per hour of 2.4 GHz air.
- Deterministic timing on the same views (ratio > 4): DJI hypotheses 15 kHz 45 views (0.13%,
  42 at 5735 MHz), 30 kHz 13, 7.5 kHz 20; **Lightbridge 32.8 kHz 1,508 views (4.4%)**, 1,148
  of them at 2430 MHz; Wi-Fi 312.5 kHz 4,103. The DJI OFDM family step is close to clean on
  this background; the 32.8 kHz test is not, so a Lightbridge family from timing alone is not
  safe at 2.4 GHz (possibly linked to clipping; not yet checked).
- Consequences for C2/C4: (1) train with this site's 2.4 GHz Wi-Fi/Bluetooth bursts as
  not_a_drone, using a time-split of this recording (first ~40 min train/val, last ~20 min
  test) plus a second recording later for a clean test; (2) gate the classifier on the timing
  family (a DJI type only where 15/30 kHz timing fired) instead of letting it type every
  burst; (3) C4 must define a row as several consistent views over time, not a single view;
  (4) look at the 32.8 kHz false hits and at receive gain at 2.4 GHz.
- C1 stays open: the per-hour row now exists as a per-view rate, but the "row" aggregation is
  defined only in C4.

## 2026-11-13 — C2 two-level model with family gate, site background and mixing

User: "Yes, start C2". Nothing committed.

**Data** (`tools/drone_ml/universal_c2_data.py`, output on the Storage disk
`2026-11-13/c2-data-v1/`): the X310 no-drone hour split by time: minutes 0–32 train (17,422
views), 32–40 val (4,650), 40–60 test (12,400; never used for fitting or epoch choice). Drone
views mixed at IQ level onto background from the same time split, drone-to-background power
-6 to +12 dB, facts recomputed; a mix keeps its label only if its own timing still fires
(hopping: ratio >= 0 dB). Kept: 7,016 train, 1,747 val, 1,762 test mixes.

**Model** (`tools/drone_ml/universal_c2_train.py`, test `tests/test_drone_ml_universal_c2.py`,
5 tests pass): spectrogram CNN + facts MLP, a type head and a model head trained only against
models of its own type; noise-floor, frequency-band and time-mask augmentation; 20 epochs;
epoch chosen on val + mix_val + bg_site_val. **Family gate** after the network, not learned:
dji_ofdm only where 15 or 30 kHz timing > 4, lightbridge only where 32.8 kHz > 4,
hopping_controller only where a burst exists; DJI models limited to the timing that fired.
Outputs in `data/drone_ml/2026-11-13/universal_c2/` incl. per-view probabilities for C4.

Scorecard (seeds 1/2/3, gated unless marked):

| Measure | C1 full | C2 |
|---|---|---|
| Type, unseen aircraft (test) | 1.00 | 1.00 / 1.00 / 1.00 |
| Type, unit_test | 0.99–1.00 | 0.999 all |
| Type, cross_test | 0.989 | 0.995 / 0.995 / 1.00 |
| Type, open_test (unseen models) | 0.66–0.69 | 0.77 / 0.74 / 0.70 |
| Type, Air 3S (own X310, unseen model) | 0.93–0.96 | 0.96 all |
| Type, drones mixed on site background (mix_test) | — | 0.999 / 0.998 / 0.998 |
| **False views, site background test (20 min)** | 7.0–10.4% (whole hour) | **1.53% / 1.83% / 1.17%** |
| Background captures with any false view | 306–389 of 556 | 43 / 49 / 35 of 200 |
| Model top-1 / top-3 in type, unseen aircraft | — | 0.56/0.99, 0.53/0.99, 0.48/0.99 |
| Model top-1 / top-3, same unit (unit_test) | — | 0.95/0.99 all seeds |
| Model top-1 / top-3, cross-dataset (Mini 3, Mini 4 Pro) | — | 0.15–0.20 / 0.60 |
| Model top-1 / top-3, mixed on site background | — | 0.62–0.63 / 0.94–0.95 |

Findings:
1. **False views on the site background fell about 6x** (7–10% to 1.2–1.8%). What remains is
   mostly hopping_controller (94–175 views, 2470 MHz worst at 7–8%, spread over ~35 captures:
   likely Bluetooth) and 43–44 dji_ofdm.
2. **42 of the 44 DJI false views are one 0.5 s capture** (5735 MHz, 10:26:21 UTC) where the
   15 kHz timing fires in 42 of 62 views: a real 15 kHz OFDM signal for at least that capture,
   absent from the 5735 captures 66 s before and after. The DroneID decoder found no packet in
   it (0.5 s can miss a DroneID burst), so it is unidentified: a passing DJI drone outside the
   room, or another 15 kHz OFDM source. Not removed from the test.
3. **Lightbridge still fails on unseen models** (Matrice 210, Phantom 4/4 Pro/4 Pro+: 0–0.29).
   One training source (KU Leuven Inspire 2) is the cause; mixing did not fix it. Needs more
   Lightbridge sources (DroneRFa may help, E4).
4. **Taranis Q X7 (unseen hopping controller) rose to 0.80–0.96** from 0.46–0.89.
5. **Model naming does not transfer across datasets** (top-1 0.15–0.20 for the same model
   from another dataset), while unseen aircraft of the same dataset get 0.48–0.56. The model
   head is learning recording conditions as much as aircraft. Model names from the classifier
   must stay "Likely, top-3" and are not yet good enough for the app.
6. Per-hour row: per view the gated false rate is ~1.5%; a per-row figure needs the C4 row
   rule (consistent views across captures). Unknown-model handling also needs C4 thresholds.

C2 ticked: every section 8 row now exists over three seeds except the two that need the C4
decision rule (false rows per hour, accepted-as-wrong-model rate).

## 2026-11-13 — C4 decision rule and thresholds

User: "Yes, do C4". `tools/drone_ml/universal_c4.py` (row-rule tests added to
`tests/test_drone_ml_universal_c2.py`, 7 pass). Decision files written next to each model:
`data/drone_ml/2026-11-13/universal_c2/c2_seed{1,2,3}_decision.json`; report `c4_report.json`.

Rule: 3 intervals per capture (start, middle, end, as `select_intervals` in the app); capture
type score = mean gated type probability; per-type threshold set on bg_site_val only (minutes
32–40, 75 captures cut into 1,575 three-interval pseudo-captures): lowest value above every val
score, at least 0.5. Model threshold set on val + mix_val for 95% right names. A row needs two
typed captures of one type on one band slot within 200 s; a model is named for a row only if two
captures name it. Measured on data not used to set anything:

| Measure (seeds 1/2/3) | Result | Proposed gate |
|---|---|---|
| Thresholds dji / lightbridge / hopping | 0.50 / 0.50 / 0.58, 0.53, 0.62 | — |
| Background test, 200 captures, 20 min: typed captures | 3 / 3 / 3 (2 hopping, 1 DJI = the 5735 MHz event) | — |
| **False typed rows per hour** | **0 in 0.33 h, all seeds** | <= 1 |
| False named rows per hour | 0 in 0.33 h | <= 1 |
| Dataset chamber noise typed (525 captures) | 0 | — |
| Capture type rate: unseen aircraft / unit / cross-dataset / mixed on site bg | 1.00 / 1.00 / 1.00 / 1.00 | >= 0.95 |
| Capture type rate, unseen models | 0.91 / 0.90 / 0.79 (Lightbridge and part of Taranis missed) | — |
| Model named right (captures), unseen aircraft | 0.58 / 0.55 / 0.49 | reported |
| Model named right, same unit | 0.98 / 0.98 / 0.99 | reported |
| Model named right, cross-dataset | 0.19 / 0.09 / 0.27 | reported |
| **Unseen model accepted as a wrong model (captures)** | **0.82 / 0.83 / 0.73** | <= 0.05 |

Findings:
1. **Type with the row rule passes on this background**: no false row in 20 minutes, while every
   drone partition of known types is typed in every capture. The single-capture DJI event at
   5735 MHz does not open a row.
2. **The per-hour number is not yet strong evidence.** Zero rows in 0.33 h only bounds the rate
   below ~9 per hour (95%). Supporting "<= 1 per hour" needs about 3 h of no-drone X310 time
   with no false row, ideally on other days and places, since thresholds and test come from
   the same day and site.
3. **Model naming fails and should stay off.** The 95% precision set on val does not hold on
   new aircraft (0.49–0.58), other datasets (0.09–0.27), or unseen models (73–83% accepted as
   a wrong model, against <= 5%). The val partition shares aircraft/sessions with training, so
   its confidence is optimistic. Recommendation for C5: the classifier contributes type only;
   model names come from DroneID and the link-family table.
4. Lightbridge on unseen models remains the type-level gap (0 of 5 recordings).

C4 ticked: decision files exist next to the models and the false rates are measured on
background not used to set them. Open for C5: the user's gates; more background hours.

## 2026-11-13 — model naming: open-set training and C4 v2

User decision: "No it should give model name too" (overruling my type-only recommendation),
then "yes go ahead" with: hidden-model rotation, an unknown-model outcome with a distance
check, naming thresholds set on aircraft not in training, a top-3 list.

Changes:
- `universal_c2_train.py`: open-set rotation (default; `--closed-set` = first C2). In every
  batch ~25% of the models of each type with 3+ models are hidden; their views are trained to
  give no visible model a lead. Per-model mean embeddings and pooled covariance saved in the
  checkpoint. Models: `data/drone_ml/2026-11-13/universal_c2b/` (type results unchanged:
  test 1.00, unit 0.999, cross 1.00, site background 1.3–1.7% false views per view).
- `universal_c4.py` v2: a capture names models (ranked list of up to 3 in the family) only if
  its top score passes a score threshold and its embedding is within a Mahalanobis distance of
  that model's training views; else "unknown <type> model". Distance: 95th percentile on val
  captures of aircraft not in training. Score: lowest value at which simulated unknowns (val
  captures scored with their own model hidden) are named in <= 5%. Tests: 8 pass.

Results (seeds 1/2/3; `universal_c2b/c4_report.json`):

| Measure | First C4 | C4 v2 |
|---|---|---|
| Score threshold | ~0.5 | 1.00 (names nothing) / 0.51 / 0.77 |
| Unseen aircraft: captures named | 0.88–0.92 | 0 / 0.04 / 0.005 |
| ... top-1 right when named | 0.49–0.58 | — / 0.88 / 1.00 |
| Same unit: captures named | 0.97 | 0.16 / 0.82 / 0.70 |
| ... top-1 right when named | 0.98–0.99 | 1.00 / 0.99 / 1.00 |
| Cross-dataset: named, top-1 right | 0.59, 0.09–0.27 | 0 / 0.53 (0.23) / 0 |
| **Unseen models named as a known model** | **0.73–0.83** | **0 / 0.19 / 0.10** |
| False rows / named rows on site background (20 min) | 0 / 0 | 0 / 0 |

Per model (seeds 2/3): Mini 3 Pro, Mavic Pro and all Lightbridge unseen models are now refused
(Mini 3 Pro was misnamed in 51 of 58 captures before). Still misnamed: **Air 3S 9 of 10
captures** (the 30 kHz family has only Mini 4 Pro and Avata 2 in training, so there is little
to compare against) and Taranis Q X7 6–20 of 70.

Findings:
1. Rejection works where the family has several trained models; it fails where the family is
   nearly empty (30 kHz: 2 models) or for hopping controllers (4 models).
2. Keeping unknowns low costs nearly all naming of new aircraft of known models: the network
   cannot yet tell "another unit of model X" from "a model it never saw". With two training
   aircraft per multi-aircraft model (DRFF-R2) and unnamed DroneRFb codes, this is a data limit,
   not a threshold choice. One seed (1) refuses every name at the 5% target.
3. Where it names, it is right: unit match 0.99–1.00, unseen aircraft 0.88–1.00 (few captures).
4. What would move this: more aircraft per model and more models per family (DroneRFa, E4);
   the user's own drones recorded on the X310 and put into training (Air 3S is held out today
   as the unseen-model test).

## 2026-11-14 — Air 3S moved into training (splits v2, C2c)

User decision: "ohk go ahead" (move Air 3S into training; Mini 3 Pro stays the unseen DJI model).
Nothing committed.

- `universal_splits.py` v2: Air 3S removed from `OPEN_MODELS`; new `SESSION_PARTITION` splits it
  by recording session (files of one session are near copies): `air3s10` + `air3s40` train
  (19 views), `air3s20` val (2), the separate `air3s_o4` session unit_test (7 views, 3 captures).
  Any other Air 3S source is refused. Test added. Only those 28 views changed partition.
- Rebuilt: splits `data/drone_ml/2026-11-14/universal_c2c/splits_v2.json`; cache
  `Storage/.../2026-11-14/cache-v2`; mixes `Storage/.../2026-11-14/c2-data-v2` (same background
  time split). Open-set C2 seeds 1–3 and C4 v2 in `data/drone_ml/2026-11-14/universal_c2c/`
  (`run.sh`, `run.log`, `c4_report.json`).

Type results are unchanged (test 1.00, unit 0.999, cross 0.995–1.00, open 0.64–0.70, site
background false views 1.2–1.9%, 0 false rows in 0.33 h on all seeds).

Naming (C4 v2 rule), previous run (Air 3S unseen) → this run:

| | c2b seeds 1/2/3 | c2c seeds 1/2/3 |
|---|---|---|
| Air 3S | named as another model 0/9/9 of 10 captures | held-out session: named 3/3/2 of 3, top-1 right 2/2/2, top-3 right in every named capture |
| Mini 3 Pro (unseen DJI) | named 0/0/0 of 58 | named 0/0/0 of 58 |
| Unseen models named as known (all) | 0 / 0.19 / 0.10 | 0.15 / 0.14 / 0.11 — all Taranis Q X7 (hopping), 16–21 of 70 captures |
| Unit match: named / top-1 right when named | 0.16–0.82 / 0.99–1.00 | 0.78–0.85 / 0.99 |
| Unseen aircraft of a known model named | 0–4% | 0.5–3.6% |
| Score threshold | 1.0 / … (seed 1 named nothing) | 0.57 / 0.59 / 0.51 (all seeds name) |

Reading: Air 3S now gets its own name, and the right one is always in the Likely top-3. The
held-out session is only 3 captures from the same unit and day, so this is a unit-match result,
not a general Air 3S claim. The remaining naming risk is the hopping-controller family
(Taranis Q X7 named as a known controller); DJI unseen models are refused.

## 2026-11-14 — Per-type naming limits (C4 v3)

User decision: "Yes, tighten it" (unknown controllers being named). Nothing committed.

Why Taranis Q X7 was named: the score check gives no protection for hopping controllers, because
simulated unknowns (a known controller scored with its own model hidden) only reach ~0.36, while
the real unknown Taranis scores 0.75–0.89 at the median. Distance does separate them: clean known
controllers sit at 4–10 from their training views, Taranis at 12–30. The v2 distance limit was
one pooled value (15–22), set almost entirely by unseen DJI aircraft.

`universal_c4.py` v3 (`universal_c4_decision_v3`): naming limits are set per signal type,
validation data only. Distance: 95th percentile on unseen-aircraft captures of the type (val +
mix_val) when it has at least 20; otherwise (hopping controllers, Lightbridge: single units) on
clean val captures of the known units. Score: simulated unknowns of that type; with fewer than 20,
the pooled value. Decision files now carry `naming.by_type`. Test added (limits apply only to
their own type; a type with no limits is never named). Rerun on the C2c models (no retraining);
v2 outputs kept in `data/drone_ml/2026-11-14/universal_c2c/c4_v2/`.

Resulting limits (seeds 1/2/3): DJI distance 22.4 / 20.4 / 14.9 (unchanged), score 0.62 / 0.64 /
0.57; Lightbridge distance 7.5 / 6.9 / 8.0; hopping distance 9.6 / 10.5 / 9.9 (score 0, the
distance alone already refuses simulated unknowns).

| (seeds 1/2/3) | v2 | v3 |
|---|---|---|
| Unseen models named as known | 0.15 / 0.14 / 0.11 | 0 / 0 / 0 |
| Taranis Q X7 named | 21 / 20 / 16 of 70 | 0 / 0 / 0 |
| Mini 3 Pro named | 0 of 58 | 0 of 58 |
| Known controllers (unit_test) named / right | 150–153 of 154 / all but 1–2 | 136–138 of 154 / all but 1 |
| Air 3S held-out session named / top-1 right | 3 / 3 / 2, right 2 / 2 / 2 | unchanged |
| Unit match named / top-1 right when named | 0.78–0.85 / 0.99 | 0.71–0.76 / 0.99 |
| Unseen aircraft of a known model named | 0.5–3.6% | 0.5–1.6% |
| Background false rows | 0 in 0.33 h | 0 in 0.33 h |

Caveat: the limit values come from validation only, but the change was designed after seeing
Taranis fail, so Taranis is no longer a clean test of it. A second never-seen controller (e.g.
from DroneRFa) is needed to confirm the 0%.

## 2026-11-14 — C3 ablations

User decision: "Yes, do C3". Nothing committed.

Each ablation is the C2c model (Air 3S trained, open-set) with one part removed, 3 seeds, the same
validation/epoch choice, the same test partitions and the C4 v3 naming rule. Code: switches
`--no-mix`, `--no-facts`, `--train-cache` in `universal_c2_train.py`; `--no-presence-filter` in
`universal_splits.py` (every window of a drone recording labelled as that drone: 95,130 windows
instead of 14,674, cache `Storage/.../2026-11-14/cache-v2-nofilter`, 3.7 GB). Tests added. Outputs
in `data/drone_ml/2026-11-14/universal_c3/` (`run.sh`, logs, `c3_table.py`, `c3_table.md`).

- no mixing: drone views mixed onto X310 background removed from training (site background kept).
- no facts: the network sees the spectrogram only; the family gate still uses the facts.
- no presence filter: only the real training windows are unfiltered; the mixes, validation and
  tests stay filtered, and the family gate still runs, so this tests labelling, not the gate.

| (range over seeds 1–3) | full (C2c) | no mixing | no facts | no presence filter |
|---|---|---|---|---|
| type: unseen aircraft (test) | 1.000 | 1.000 | 0.990–0.999 | 1.000 |
| type: unit_test | 0.999 | 0.998–1.000 | 0.987–0.993 | 1.000 |
| type: cross-dataset | 0.995–1.000 | 0.984–1.000 | 0.926–0.974 | 0.995 |
| type: unseen models | 0.635–0.702 | 0.709–0.733 | 0.690–0.716 | 0.738–0.762 |
| type: mixed onto X310 bg | 0.998–0.999 | 0.982–0.997 | 0.950–0.975 | 0.994–0.997 |
| site bg views falsely typed | 1.2%–1.9% | 0.8%–1.3% | 1.4%–2.1% | 0.9%–1.1% |
| bg captures typed (of 200) | 3 | 1 | 2–3 | 3 |
| bg false rows (0.33 h) | 0 | 0 | 0 | 0 |
| model top-1 per view: unit_test | 0.906–0.920 | 0.896–0.899 | 0.901–0.916 | 0.898–0.908 |
| model top-1 per view: cross | 0.234–0.307 | 0.042–0.235 | 0.148–0.196 | 0.069–0.101 |
| captures named: unit_test | 0.714–0.760 | 0.656–0.688 | 0.676–0.731 | 0.663–0.722 |
| top-1 right when named: unit_test | 0.993–0.994 | 0.996–1.000 | 0.997–1.000 | 0.993–0.997 |
| unseen models named as known | 0.000 | 0.000 | 0.007–0.071 | 0.021–0.071 |
| Taranis named (of 70) | 0 | 0 | 1–10 | 3–10 |
| Air 3S named / right (of 3) | 3·2/3·2/2·2 | 1·1/2·2/2·2 | 1·1/2·2/0·0 | 2·2/2·2/2·2 |

(Air 3S: named·top-1 right per seed.)

What each part earns:

- **Measured facts as network input: earn their place.** Without them, type accuracy drops on
  other datasets (cross 0.93–0.97 vs 0.995–1.00) and on drones mixed into X310 background
  (0.95–0.98 vs 0.998), and unknown models start being named (Taranis 1–10 of 70; one seed loses
  Air 3S entirely).
- **Presence filtering: earns its place for naming, not for type.** Unfiltered labels give the
  same or slightly better type results (unseen models 0.74–0.76, site background 0.9–1.1%), but
  model naming across datasets collapses (top-1 0.07–0.10 vs 0.23–0.31) and unknown models are
  named again (Taranis 3–10 of 70). The gate still protects type: windows with no measured signal
  are blocked at run time either way.
- **Background mixing: mixed result.** It helps low-SNR and naming (mixed-test type 0.998 vs
  0.98–1.00, more captures named 0.71–0.76 vs 0.66–0.69, cross-dataset model top-1 0.23–0.31 vs
  0.04–0.24, Air 3S named more often), but without it fewer site-background views are falsely
  typed (0.8–1.3% vs 1.2–1.9%) and unseen-model type is a little higher (0.71–0.73 vs 0.64–0.70).
  Kept: the false-typing difference is on views and never reached a false row (0 in 0.33 h for
  every variant); naming and weak signals are what mixing is for.

Limits: 0.33 h of held-out background cannot separate false-row rates (0 for all); the 3-hour
no-drone recording is still needed. Unseen-model type (open_test) is 0.64–0.76 for every variant.

## 2026-11-14 — C5 decision and Stage D integration (D1–D6)

User decision (C5): record the ~3 h no-drone background later; "for now go ahead and work next
things safely". Recorded as a conditional go: gates as proposed, strict naming point (C4 v3),
classifier-only rows built but off by default. Scorecard: `DRONE_UNIVERSAL_CLASSIFIER_SCORECARD_2026-11-14.md`.
Nothing committed.

**D1. Package and worker.**
- `tools/drone_ml/universal_package.py` builds one model file:
  `data/drone_ml/2026-11-14/universal_package/universal_v1.pt` (sha256 `f8406cb0…`). It holds
  the C2c network, normalisation, model list and timing; the C4 v3 decision file; per-model
  embedding means and precision; measured accuracy per model; and provenance. Seed 2 was chosen
  on validation (selection 0.984).
- `tools/drone_ml/universal_worker.py` uses the same socket protocol as `live_worker.py`, at
  20 MSps only. Each interval returns gated type scores. The capture's last interval also returns
  `universal.capture` (type, ranked models, named or refused, and why), computed by the same code
  as the evaluation (`universal_c4.Views.capture` + new `explain`).
- Test `tests/test_drone_ml_universal_worker.py` covers handshake, capture grouping, STFT
  preview, refusal of other rates, and parity. Worker spectrogram and facts for real Air 3S views
  are bit-identical to the training cache, and the capture decision matches the evaluation.

**Bug found and fixed (facts scaling).**
- `universal_train.Norm.compress` decided which facts to rescale (×1e6 before log) from the
  largest value in the current batch. In training and evaluation (batches of 128–512) the result
  was always the same two features (`burst_length_s`, `burst_period_s`), so the scorecard is
  unaffected. A single live view, though, would rescale any feature that happened to be small,
  which produced garbage and a NaN.
- The mask is now fixed from training data (`Norm.scale_mask`, saved in `state()`). The package
  stores it, and the build checks it reproduces the evaluated outputs on all 1,259 validation views.
- Also fixed: when only not_a_drone was allowed and its raw score underflowed to 0, the gate
  divided 0 by 0. The answer is now not_a_drone. Rerunning C4 on the C2c models gives a report
  identical to the scorecard's (0 differences).

**D2.** `src/ml_inference.{hpp,cpp}` parses the handshake (types, model types, accuracy) and
per-result `universal` fields, as `Result::capture_decision`. Bad scores or schemas fail the
worker, and old model files load unchanged. `test_ml_inference` covers both, via the fixture
worker's universal mode.

**D3–D5.** `src/drones/universal_claim.hpp`:
- Family check: the type must match the row's measured link (15/30 kHz → DJI OFDM, 32.8 kHz →
  Lightbridge), otherwise it counts as a disagreement and gives no model. Named models whose
  measured link group does not fit the row are dropped.
- Agreement: at least 2 named captures; models ranked by mean score.
- DroneID-confirmed rows are untouched, and the old hint path is skipped when a universal model
  is loaded.
- Classifier-only rows (`ClassifierSightings`) use the C4 row rule (second typed capture within
  200 s on one slot, merge within 300 s), expire 60 s after the last sighting, and give way to a
  nearby link row.

**D4.** `model_verdict.hpp`:
- A Family row with an accepted list reads "DJI O4 family: likely Mini 4 Pro, Air 3S or Avata 2",
  with the capture count and measured accuracy.
- Typed but not named reads "model unknown (named in n of m captures; needs 2)". Disagreements
  are stated.
- New weakest tier `Classifier` for classifier-only rows; encrypted DroneID rows show the
  classifier's ranking as detail.
- Tracks, evidence lists and the detection log carry "Classifier".

**D5 switch.** Off by default in the Drones view (`classifier_only_rows`); live check tool
`--classifier-rows`. Tracks accept classifier-only rows, giving one lower-severity alert per row
and none on refresh.

**D6.** `coordinated_live_check --ml` uses the universal package and prints per-capture types,
named counts, refusal reasons and each row's verdict; the JSON summary carries them too.

**App wiring** (`src/main.cpp`, a few lines): the Drones classifier starts the universal
package; rows get claims and classifier-only rows after the existing hints; one switch.

**Checks.** 92/92 C++ tests pass (new `test_drone_universal_claim`). The Python universal tests
pass. End to end with no radio, the real C++ client, worker and package ran on held-out Air 3S
captures:
- 2452 MHz: DJI OFDM (0.99). Air 3S ranked first at 0.58 but was refused at the strict limit of
  0.64, so the row would show the family with "model unknown".
- 5745 MHz: not a drone (that recording has no measured signal).
- 22–30 ms per interval.

Not done: E1 replay (needs the background recording), live run (E2).

## 2026-11-14 — Phantom in the "no-drone" background; C2d; package v2

User: "there was a phantom in yesterday's no drone recording ig", plus Phantom 4 Pro recordings
in `~/Music/lb_decode/outputs/p4p/priority1`. Nothing committed.

**The background contained a Lightbridge link.**
- In the 13 Nov hour, the 32.8 kHz timing fires on 17–18% of 2.4 GHz views from minute 18 on.
  Before minute 18 it fires on 0–0.1%, and at 5.8 GHz never.
- The hits sit at 2430 MHz (35% of views), sometimes 2450/2470, persist through the hour, and
  fill 18–20 MHz.
- Labelled "not a drone", these trained the network to dismiss a Lightbridge link at this site
  (3,596 train and 1,612 validation views). Thresholds were not affected: the Lightbridge type
  threshold stayed at its 0.5 floor.

**Why the app misses this Phantom.**
- The user's Phantom 4 Pro recordings (X310, 12 Nov, switched on, stationary, motors off; ten
  10 s captures at 5785 MHz and five at 2437.5 MHz, gain 30) fire none of the six timing checks.
- The packaged classifier calls every capture "not a drone": the family gate opened Lightbridge
  in 1 of 60 intervals.
- `lb_decode` gives this Phantom's Lightbridge a 4096-point FFT at 44.8 MSps, i.e. a 10.9375 kHz
  subcarrier spacing. The probe's 32.8 kHz check is 3x that and sees only part of the signal.
- A 10.9375 kHz check, using the same ratio method, fires on:
  - the Phantom at 2437.5 MHz: 85–95% of views; at 5785 MHz (weak): 10–20%;
  - the background's 2430 MHz link period: 64%;
  - clean background (399 captures, 2,793 views): 0, with the 99.9th percentile ratio at 2.78
    against a threshold of 4.
- This also confirms the user's report: the background link has the Phantom's symbol timing.
- Not yet added to the probe or classifier; waiting for the user's decision.
- Note: the 2437.5 MHz captures at gain 30 are hard-clipped (peak |x| = 1.414).

**C2d.** `universal_c2_data.py` v2 excludes 2.4 GHz captures from minute 17 on from every
background split and mix (`EXCLUDE_24_FROM_MIN`); the recordings are untouched. Retrained
open-set seeds 1–3 and ran C4 v3 (`data/drone_ml/2026-11-14/universal_c2d/`, data `c2-data-v3`).

| (seeds 1–3) | C2c | C2d |
|---|---|---|
| Type: never-seen models | 0.64–0.70 | 0.89 |
| Site background views falsely typed | 1.2–1.9% | 0.5–0.6% |
| Background captures typed / false rows | 3 of 200 / 0 | 1 of 127 / 0 |
| Never-seen models named as known; Taranis | 0; 0 | 0; 0 |
| Known units named / right when named | 0.71–0.76 / 0.99 | 0.69–0.74 / 0.997 |
| Cross-dataset model top-1 (views) | 0.23–0.31 | 0.17–0.28 |

- Never-seen Lightbridge models are now partly typed Lightbridge (Matrice 210, Phantom 4,
  Phantom 4 Pro+), where before they were dismissed.
- Caveat: the held-out background test (minutes 40–60) now has no 2.4 GHz captures. 2.4 GHz
  false rows rest on the coming 3 h recording.

**Package v2.** `data/drone_ml/2026-11-14/universal_package/universal_v2.pt` (C2d, seed 3 on
validation; sha256 `2d29cb1b…`; the scale-mask check passes). The app, the live check and the
worker tests use it; v1 is kept. Worker tests pass and 92/92 C++ tests pass. End to end, the
held-out Air 3S capture (2452 MHz) is now named Air 3S (0.85, limit 0.65; v1 refused it). The
Phantom capture is still "not a drone" (the gate needs the 10.9375 kHz check).
