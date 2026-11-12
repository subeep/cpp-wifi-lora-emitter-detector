# Universal drone classifier: plan

Date: 2026-11-12. Status: plan, nothing built yet. One plan with one checklist (section 10); who does each step is decided when it is started. Nothing here is to be committed without the
user's say.

## 1. What we are building

One classifier for every drone and controller we hold recordings of, DJI or not, that can also
answer "not a drone". It feeds the Model column of the Drones table, below the evidence that is
stronger than it.

It replaces the present 21-class network, which has no "not a drone" answer and named a drone
in 48% of no-drone intervals on the X310 (`DRONE_RF_MODEL_ON_X310_BACKGROUND_2026-11-10.md`).

### Order of evidence (fixed)

| Rank | Evidence | What the row says |
|---|---|---|
| 1 | Readable DJI DroneID | The model, "Confirmed" |
| 2 | Encrypted DroneID plus the measured link family | A short list, "Likely" |
| 3 | Link family from the measured subcarrier spacing | The family, always stated when a link is measured |
| 4 | Classifier inside that family | A model under the family; an answer from another family is discarded |
| 5 | Classifier alone | Only where no link family is measured: non-DJI drones, Wi-Fi drones, hobby controllers |
| — | Remote ID | Its own row or joined by serial; it carries no model name |

The family is a measurement and constrains the classifier. The classifier never overrides
ranks 1 to 3.

### What the classifier returns

For each analysed interval:

- **signal type**: one of `dji_ofdm` (15 kHz / 30 kHz groups), `lightbridge` (32.8 kHz),
  `wifi_drone`, `hopping_controller`, `analogue_video`, `other_drone`, `not_a_drone`;
- **model**: a ranked list of models with scores, or none;
- **accepted**: whether the top model passes the acceptance rule (section 7);
- the model file's hash, the class list and the measured accuracy for the accepted class, so the
  app can show "Classifier: X (measured accuracy on unseen aircraft: N%)".

## 2. What exists to build on

- Training and evaluation tools in `tools/drone_ml/` (corpus inventory, splits, preprocessing,
  `SpectrogramCNN`, residual model, calibration, presence experiments), the Python runtime at
  `data/drone_ml/runtime-2026-10-14` and a CUDA runtime, an RTX 3060 Ti (8 GB), 46 GB RAM.
- The app's inference worker (`tools/drone_ml/live_worker.py`, `predict.py`,
  `src/ml_inference.cpp`), which already runs a model on three 8 ms intervals per capture and
  returns the top three candidates with scores.
- The timing probe (`cp_lag_probe.py`, and its C++ port in `src/drones/symbol_timing*`), the
  DroneID decoder, the model table (`model_links`), `verdict()` and the classifier hint.
- Earlier findings that shape this plan:
  - a model trained on whole recordings learns the recording, not the link: three quarters of
    "Air 2" training windows held no Air 2 link;
  - inside one DJI family, naming an unseen aircraft reaches about 0.5 to 0.7 by three
    different methods; with the same aircraft on both sides it reaches 0.93;
  - the timing probe flags 30 kHz in 0.11% of no-drone blocks, so the family is far more
    selective than any learned score so far.

## 3. Data

| Source | Receiver, rate | What it holds | Aircraft per model |
|---|---|---|---|
| DRFF-R2 (non-locked units) | USRP, 100 MSps, 5745 MHz | Air 2, Air 2S, Mini 3 Pro, Mini 4 Pro, Mini 5 Pro, Mavic 3; environment files | several for most |
| DroneRFb-DIR | 80 MSps, 2440 MHz | Six types known by letter (A, C, D, E, F, G); codes 84, 87 (Mini 3), 88, 90, 93 | 3 |
| RFUAV | X310, 100 MSps | 37 types: Avata 2, FPV, Mavic 3 Pro, Mini 3, Mini 4 Pro, Autel Evo Nano, and about 30 hobby controllers and links (FrSky, Futaba, FlySky, Jumper, RadioMaster, Radiolink, Skydroid, SIYI, WFLY, Herelink, Yunzhuo, JR, Devention) | 1 |
| KU Leuven | X310, 100 MSps, 2.44 GHz, anechoic | Mini 2, Inspire 2, Matrice 300, Mavic 2 Enterprise Advanced, SJRC F11 Pro, five controllers | 1 |
| Tampere | 19 recordings | Inspire 2, Matrice 100/210, Phantom 4, Phantom 4 Pro+, Mavic Pro, Mavic Mini, Parrot Disco, Parrot Mambo, Yuneec Typhoon H | 1 |
| Own X310 sessions | X310, 20 MSps | Background (237 one-second captures), Air 3S sessions, live-check captures | Air 3S: 1 |
| DroneRFa | pending download (616 GB) | Expected: Matrice 200/300/30T/600 Pro, Phantom 3, Phantom 4 RTK, Avata, Mini 2 and others; list unconfirmed | unknown |
| FPV analogue video | on disk | Excluded until its format and zero-tail issues are closed | — |

The locked DRFF-R2 test units (Air 2 units 5–8, Air 2S 5–7, Mini 4 Pro 5, mavic3C, mavic3S)
are never opened in this work. They are used once, at the end, on the user's instruction.

Two facts about this data decide what can be claimed:

1. **Most classes have one aircraft and one recording session.** For those, accuracy on an
   unseen aircraft cannot be measured. They are trained and reported as "matches the recorded
   unit", a weaker claim than the multi-aircraft classes.
2. **Almost none of it comes from the deployment receiver setting** (X310 at 20 MSps, Wi-Fi
   channel centres, real 2.4 GHz traffic). Transfer to that setting is the main risk.

## 4. Classes

Two levels, trained together.

**Level 1, signal type (7 classes).** Chosen so that the types differ physically and each
has several models and sources:

| Type | Examples | Sources |
|---|---|---|
| `dji_ofdm` | Air 2, Mini 2, Mini 4 Pro, Avata 2, Matrice 300 | DRFF-R2, DIR, RFUAV, KU Leuven, Tampere, X310 |
| `lightbridge` | Inspire 2, Matrice 100/210, Phantom 4 | Tampere, KU Leuven, live Phantom 4 |
| `wifi_drone` | Mavic Mini, Parrot Disco/Mambo, Yuneec, SJRC, Autel Evo Nano (to be confirmed by measurement) | Tampere, KU Leuven, RFUAV |
| `hopping_controller` | FrSky, Futaba, FlySky, Spektrum, Radiolink | RFUAV, KU Leuven |
| `analogue_video` | held back until the FPV set is usable | — |
| `other_drone` | drone-related signals that fit no type above | assigned during the inventory |
| `not_a_drone` | Wi-Fi, Bluetooth, noise, other ISM traffic | X310 background, DRFF environment files, dataset no-drone files |

**Level 2, model.** One class per model. Each class carries a flag:

- `multi_aircraft`: two or more aircraft in training and at least one never trained on, so
  unseen-aircraft accuracy is measured;
- `single_unit`: one aircraft; reported as unit match only.

The six DroneRFb-DIR letters stay separate classes where their product code is unnamed, and
merge with a named model where the code is known (87 is Mini 3).

Models we know only from the table (no recordings) are not classes. They stay reachable through
the link family and DroneID.

## 5. Inputs

The network sees what the app can give it: intervals from one capture at 20 MSps.

1. **Views.** Every dataset recording (80–100 MSps) is cut into 20 MSps views at several
   offsets from the signal, as the scanner's Wi-Fi-centred captures would see it, including
   views that hold only part of the signal. Rate conversion uses the same resampler everywhere.
2. **Presence labels, per window.** A window counts as a positive for its class only if the
   signal is in it:
   - OFDM links: the timing probe flags the class's own timing in that window;
   - hopping controllers and Wi-Fi drones: a burst detector on the power envelope, checked
     against a matching no-signal file of the same source;
   - windows from a drone recording that hold no drone signal become `not_a_drone` candidates
     only after review of a sample, because they may hold the controller.
   This is the correction for the recording-context problem.
3. **Two inputs per window.**
   - the spectrogram, as now (8 ms, existing preprocessing);
   - a short vector of measured facts: the lag profile at the listed timings and its strongest
     blind peaks, occupied bandwidth, burst length and period from the envelope, duty cycle,
     spectral flatness. These are the quantities the family measurement already uses, so the
     network and the family rule see consistent evidence.
4. **Longer context.** Per capture, the three interval outputs are combined; per row, the
   captures that carry the link are combined (as the hint does today).

## 6. Training

- **Network:** the compact `SpectrogramCNN` trunk plus a small network on the measured facts,
  joined before two output heads (type, model). The model head is trained with a mask so each
  example only competes with models of its own type. Small on purpose: the data, not the
  capacity, is the limit.
- **Sampling:** balanced by type, then model, then aircraft, then source, so that large
  datasets and long recordings do not dominate.
- **Augmentation, aimed at receiver transfer:**
  - gain and noise-floor changes, small frequency shifts, time shifts;
  - **mixing with real X310 background** at random power ratios: a dataset link laid over
    captured 2.4 GHz Wi-Fi and Bluetooth is the closest we can get to deployment data without
    flying every model;
  - channel filtering (mild frequency-selective fading), random DC and IQ imbalance.
- **`not_a_drone`:** X310 background captures at every scanned centre, split by session so the
  evaluation background is never trained on.
- **Open set:** a share of whole classes is left out of training in rotation and must come out
  as its type with no accepted model.
- **Runs:** three seeds per configuration; every reported number is a range over seeds.

## 7. Decision rule

1. Per interval: type scores and model scores.
2. Per capture: mean of the interval scores of intervals that pass the activity gate.
3. **Family mask:** if the row has a measured link family, models outside it are removed
   before ranking. If the type head disagrees with the measured family (for example it says
   `hopping_controller` on a 30 kHz link), the classifier gives no model and the disagreement
   is logged.
4. **Acceptance:** a model is accepted for a row only when
   - at least a named number of captures of the row agree on it, and
   - its combined score is above a threshold chosen on held-out background data for a stated
     false-name rate (section 8). The threshold is per type, since score distributions differ.
5. **Classifier-only rows** (no link, no DroneID, no Remote ID): opened only for an accepted
   type other than `not_a_drone` that persists on nearby frequencies across sweeps. They carry
   the tier "Classifier" and the measured accuracy, and raise the lowest alert severity.

## 8. Scorecard and gates

Reported for every trained model, on data never used for training or threshold setting:

| Measure | Data | Gate before integration |
|---|---|---|
| Type accuracy, unseen aircraft | held-out aircraft of multi-aircraft classes | proposed ≥ 0.95 |
| Type accuracy, other receiver | X310 recordings of our own drones | proposed ≥ 0.90 |
| False named rows per hour | held-out X310 background sessions | the user sets it; proposed ≤ 1 per hour |
| False typed rows per hour (classifier-only) | same | proposed ≤ 1 per hour |
| Model top-1 and top-3 inside the family, unseen aircraft | multi-aircraft classes | reported, no gate; expected 0.5–0.7 top-1 |
| Model accuracy, same unit | single-unit classes, held-out time segments | reported as unit match |
| Unknown model handling | left-out classes | accepted-as-wrong-model rate, proposed ≤ 5% |
| Cross-dataset pairs | Inspire 2 (Tampere vs KU Leuven), Mini 4 Pro (DRFF-R2 vs RFUAV), Mini 2 (KU Leuven vs X310 if recorded), Air 3S (unseen class, X310) | reported |

Gates are proposals. The user decides them after seeing the first scorecard. If the background
gate fails, the classifier stays out of the app and only the scorecard is delivered.

The sealed DRFF-R2 test is run once, after the gates pass and the user says so.

## 9. Integration

- **Worker:** the existing inference worker loads the new model file. Its reply gains the type,
  the acceptance flag and the per-class accuracy. The class list and thresholds travel inside
  the model file; nothing is hard-coded in C++.
- **`src/ml_inference.cpp`:** parse the new fields; old model files keep working.
- **`classifier_hint.hpp`:** replaced by the decision rule of section 7 (family mask,
  agreement across captures, acceptance).
- **`model_verdict.hpp`:** rank 4 becomes "family, with classifier model under it"; a new
  "Classifier" tier for rank 5.
- **Rows and tracks:** `detections()` accepts classifier-only sightings as a fourth evidence
  source; tracks and alerts treat them at the lowest severity.
- **Live check tool:** prints classifier types, accepted models and rejections.

## 10. Checklist

One list, in order. A step is ticked only when its "done when" is met. Steps marked (gate)
stop the work until the user decides.

### Stage A: preparation

- [ ] **A1. Decoder worker emits the remaining DroneID fields** (height, altitude, pilot, home,
      speed). Done when: the worker test checks them on the recorded fixtures and the tracks
      view shows a pilot position from a fixture replay.
- [ ] **A2. Corpus inventory.** Every recording listed with source, receiver, rate, centre,
      aircraft, signal type, model and flags; locked DRFF-R2 units listed as unreadable.
      Done when: one manifest file exists, with counts per class, aircraft and source, and a
      script that refuses locked paths.
- [ ] **A3. Class list.** Signal types and models fixed from A2, each model flagged
      `multi_aircraft` or `single_unit`. Done when: the list is written with the reason for
      every type assignment and reviewed by the user.
- [ ] **A4. Background set.** X310 no-drone sessions split by session into training and
      held-out. Done when: both parts exist and the held-out part covers every scanned band.
- [ ] **A5. Own-drone recordings on the X310** (user flies; any drones available). Done when:
      at least one drone session and one same-day no-drone session are on disk. The plan
      continues without them, but the "other receiver" row of the scorecard stays empty.

### Stage B: data for training

- [ ] **B1. 20 MSps views.** Every recording cut into deployment-like views at several
      offsets. Done when: the window store is on the Storage disk with a count per class.
- [ ] **B2. Presence labels.** Each window marked signal-present or not, by the timing probe
      for OFDM links and a burst detector for the rest. Done when: presence share per class is
      reported and 20 windows per class have been checked by eye against their spectrograms.
- [ ] **B3. Measured-fact vector.** Lag profile, bandwidth, burst timing, duty cycle,
      flatness. Done when: the Python extractor agrees with the C++ timing code on a set of
      test captures.
- [ ] **B4. Splits.** By aircraft and by session; left-out classes chosen for the unknown-model
      test. Done when: no aircraft or session appears on both sides, checked by script.

### Stage C: models

- [ ] **C1. Type-only baseline.** Done when: scorecard rows for type accuracy and false typed
      rows per hour exist, over three seeds.
- [ ] **C2. Two-level model** with the family mask, augmentation and background mixing.
      Done when: the full scorecard of section 8 exists, over three seeds.
- [ ] **C3. Ablations.** Without background mixing, without the measured facts, without
      presence filtering. Done when: each is in the scorecard, so we know what earns its place.
- [ ] **C4. Thresholds and acceptance rule** set on held-out background only. Done when: the
      decision file is inside the model package and the false-name rate is measured on a
      background session not used to set it.
- [ ] **C5. Scorecard review (gate).** The user sets the gates of section 8 and decides
      go or no-go. No-go keeps the classifier out of the app.

### Stage D: integration (only after a go at C5)

- [ ] **D1. Worker reply format** frozen and implemented: type, ranked models, acceptance,
      per-class accuracy, model hash. Done when: old model files still load and a test covers
      the new fields.
- [ ] **D2. App parsing** in `src/ml_inference.cpp`. Done when: the new fields reach the Drones
      view code.
- [ ] **D3. Decision rule in the app** (family mask, agreement across captures, acceptance),
      replacing the present hint. Done when: tests cover a masked-out answer, a type/family
      disagreement, too few captures, and an accepted model.
- [ ] **D4. Verdict tiers:** family with a classifier model under it; a "Classifier" tier for
      classifier-only rows. Done when: the verdict tests cover both.
- [ ] **D5. Classifier-only rows** in the detection rows, tracks and alerts, at the lowest
      severity. Done when: tests cover open, persist, expire and no alert storm.
- [ ] **D6. Live check tool** prints types, accepted models and rejections.

### Stage E: proof

- [ ] **E1. Offline replay** of the held-out X310 sessions through the app path. Done when:
      false rows per hour match the scorecard within its range.
- [ ] **E2. Live run** with the user's drones and a no-drone hour. Done when: results are
      written up with what was and was not seen.
- [ ] **E3. Sealed DRFF-R2 test (gate).** Run once, only on the user's instruction, after E2.
- [ ] **E4. DroneRFa added** when its download completes: inventory, views, retrain, scorecard
      compared with C2.

### Running alongside, not blocking

- [ ] An in-app "record this session" switch for coordinated mode, saving every capture with
      a typed label. It makes A5 and later replay tests easy.

## 11. Risks and how each is checked

| Risk | Check |
|---|---|
| The model learns the dataset or receiver, not the signal | Cross-dataset pairs; background mixing; per-source confusion table |
| Background still gets drone names | Held-out X310 background sessions; false rows per hour is a gate |
| Single-unit classes look better than they are | Reported separately as unit match; never mixed into unseen-aircraft numbers |
| Presence labels are wrong (the probe misses links under strong tones) | Review a sample of windows per class by spectrogram; count probe misses |
| 20 MSps partial views lose what separates models | Compare accuracy on full and partial views; report both |
| Family mask hides a classifier that is wrong about the type | Type-versus-family disagreements are counted and logged |
| The threshold drifts with gain | Gain augmentation; thresholds checked at two receiver gains on the X310 |
| Locked test data leaks | The inventory lists locked files as unreadable; scripts refuse their paths |

## 12. What is needed from the user

1. Recordings on the X310 of the drones available (Phantom 4; the Mini 2 if it is the user's;
   the Air 3S when available), a few minutes each, with a no-drone session from the same
   place and antenna on the same day.
2. Any non-DJI drone or hobby controller that can be brought near the antenna.
3. The gates of section 8, once the first scorecard exists.
4. The instruction to run the sealed test.

## 13. Not in this plan

Training on the locked units; Bluetooth Remote ID; the UI redesign (`~/Documents/upgraded_ui`);
calibration of the link detector's own threshold (kept as a separate, later task); 10 GbE
capture.
