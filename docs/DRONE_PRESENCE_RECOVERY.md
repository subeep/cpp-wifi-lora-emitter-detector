# Drone presence and Mini 2 recovery

Updated 22 October 2026. This work addresses the live observation that ordinary RF receives
drone-name candidates and the Mini 2 is poorly recognized. **No qualified presence model
has been trained or enabled by this change.** Existing acquisition, scanning, decoding,
cyclostationary processing and security functions keep their current behavior.

Follow-on work now adds [dispersed capture sampling and a separate review-gated presence
trainer](DRONE_PRESENCE_BASELINE.md). The first readiness check found no fresh recorder
sessions, so real-data presence training remains pending.

## Implemented now

- The Model Inference tab's main result is **Unclassified RF — drone presence undetermined**,
  or **Insufficient evidence**, with a separate historical state for stale observations.
  Candidate names and relative class scores are inside collapsed diagnostic details.
  Scores are no longer shown as percentage bars. The history is explicitly diagnostic.
- The Python live result adds `presence_status`, nullable `drone_present` and
  `drone_presence_probability`, and false `presence_validated` / `identity_validated` flags.
  No candidate score or activity-proxy output can become positive presence, absence,
  unknown-drone identity, or an accepted model. The C++ supervisor also withholds identity
  independently of what a worker returns. Offline research prediction APIs are unchanged.
- An optional, default-off audit recorder saves the **exact submitted 8 ms IQ**, receiver
  provenance, actual rate/tuning, available gain, checkpoint hash, preprocessing contract,
  quality diagnostics and inference result. It writes from the Python worker, not the
  acquisition callback. Each start creates a unique session. Recording stops after 64
  intervals or 256 MiB of IQ, whichever comes first; inference continues. Write failures
  disable recording and are shown in the GUI. Disk latency can delay optional inference;
  the existing bounded queue/drop behavior remains in force.
- `tools.drone_ml.input_audit` replays a saved session with IQ integrity checks and compares
  candidate, status, selected frequency view and all scores for the same checkpoint hash
  (score tolerance 1e-6). It can also sample disjoint intervals across a CF32 recording and
  compare five guarded frequency views without changing production preprocessing.
- Session context is an operator observation, not a verified interval label. All recordings
  start with `training_eligible=false` and `window_truth_verified=false`. Nothing is
  automatically added to training, and no model or acceptance policy is promoted.

## Findings from existing local recordings

Sources are the two 25 MSps CF32 files in
`/home/sudeep/Pictures/Drone_recorder./reports/throughput-2026-09-24/`.
Their full SHA-256 hashes match `field-captures.json`. That manifest confirms Mini 2/controller
linked for one recording and both switched off for the other. It does not establish absence
of other drones or emitter truth for every interval. Both belong to the same development
session. Their sidecars also report aggregate decoder continuity breaks without mapping them
to saved IQ intervals; exact temporal continuity is not independently established here.

Twelve disjoint 8 ms intervals, spread across each 2-second recording, were replayed through
each model. This examines 96 ms per recording (4.8%); it is not continuous detection testing.

| Recording / check | Previous residual (GUI default) | Expanded residual |
|---|---:|---:|
| Linked Mini 2 context: Mini 2 top candidate | 1 / 12 | 0 / 12 |
| Linked context: insufficient evidence | 1 / 12 | 1 / 12 |
| Linked context: intervals with any Mini 2 top candidate across six view configurations | 1 / 12 | 0 / 12 |
| Linked context: candidate changes in at least one fixed view | 5 / 12 | 11 / 12 |
| Target off: intervals producing some drone-name candidate | 12 / 12 | 12 / 12 |

The six configurations are the checkpoint's normal automatic view plus five fixed offsets;
they are correlated diagnostic views, not independent trials. At 25 MSps those fixed offsets
are -1.25, -0.625, 0, +0.625 and +1.25 MHz, with a 20 MSps model view. These small shifts do
not cover another Wi-Fi channel or a hop outside the original recording's band.

The previous model most often ranked Mini 5 Pro in the linked recording (5 intervals);
the expanded model also most often ranked Mini 5 Pro (4). In the target-off recording,
the previous model ranked Mini 5 Pro six times and Mini 4 Pro four times; the expanded model
ranked Mini 4 Pro five times and Air 2S three times. These are **candidate counts**, not
accuracy, verified false detections, or calibrated probabilities. The actual emitters remain
unverified. The quality proxy observed a component peak as high as 0.999634 in the linked
recording; proximity to full scale is a flag for review, not proof of clipping.

This reproduces candidate instability on local receiver data and gives no basis to promote
the expanded checkpoint. Frequency-view changes alone did not restore reliable Mini 2
ranking. It does not prove which combination of signal selection, label contamination,
receiver differences, link modes and model generalization causes the failure.

Reports: `data/drone_ml/2026-10-22/presence_input_audit/` (generated local data):
`source_integrity.json`, `previous_residual_mini2_context.json`,
`expanded_residual_mini2_context.json`, `previous_residual_target_off_context.json`,
`expanded_residual_target_off_context.json`.

## Collect the next evidence using the GUI

1. Use the existing receiver controls to connect and observe the relevant band/channel.
   Start Inference does not connect, retune or start RF transmission. Check actual rate,
   selected model view and capture/skip counters in the tab. First verify that the view
   contains the signal of interest; merely having a drone nearby does not do that.
2. In Model Inference, enable **Record exact inference inputs for review**. The default output
   is `data/drone_ml/live_audits`. Choose a context and enter location, session/date, mode,
   aircraft/controller state, approximate distance and surrounding transmitter activity.
3. Collect separate sessions for controlled background, controller only, linked Mini 2 idle,
   and linked Mini 2 video. For background, include active ordinary router/phone traffic,
   quieter conditions and other relevant RF environments. An off-target session alone is
   not proof of a drone-free scene. Do not switch context while inference is running:
   Stop Inference, select the new context, then Start Inference to create another session.
4. Repeat on independent days/locations and settings; retain aircraft/controller/receiver
   identifiers. Pair on/off conditions where practical. Do not label every interval in an
   on-session as a drone transmission. Mixed or unattributable windows need review or exclusion.
5. Stop before offline replay. Review saved path, count and any recorder errors. Disk budget
   is per start, not a permanent quota. The recorder saves only submitted intervals and ends
   at its cap. Long continuous recordings with coverage metadata will still be needed to
   measure false alerts per hour, encounter misses and detection latency.

Example replay from the repository root (replace `SESSION_DIRECTORY`; use a new report path):

```bash
data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.input_audit \
  --model data/drone_ml/2026-10-14/expanded_training_01/residual_model/model.pt \
  --session SESSION_DIRECTORY \
  --output /tmp/drone-session-replay.json
```

For an existing CF32 recording, use `--iq PATH --sample-rate-hz ACTUAL_RATE`, optionally
`--intervals 12 --sweep-views --expected-context-label DJI_Mini_2`, instead of `--session`.
Rate must come from receiver metadata. Expected context is for diagnostics, never a label
verification shortcut. Inputs are opened read-only/no-atime and reports refuse overwrite.

## Remaining milestones and training design

1. **Evidence review and disjoint splits.** Review positive windows from the existing broad
   drone corpus and new local recordings, and controlled negative/background windows.
   Record label evidence, source hashes, same-session grouping, receiver and operating mode.
   Keep all intervals, views, augmentations and on/off recordings from a collection session
   together. A new recorder start does not by itself create an independent session. Reserve
   whole sessions/aircraft for calibration and final testing, and entirely different unknown
   models for unknown calibration versus final unknown testing. The SDA drive was not mounted
   at its earlier path during this change; no extraction or dataset modification was attempted.
2. **Presence baseline.** Train a separate small STFT model first, using the existing numerical
   preprocessing contract. Reuse the compact CNN architecture with a binary presence head,
   but fit normalization only on training data. Train on reviewed drone/background windows;
   balance by collection group rather than by huge counts of correlated windows. Quiet noise
   and structured ordinary RF both need negative coverage. Do not treat unlabelled RF as
   negative. A detector trained only on Mini 2 cannot substantiate broad unknown-drone claims.
   Separate models allow us to measure presence errors without silently changing identity.
3. **Selection and joint evidence.** Compare the current first-interval/power-selected view
   with bounded multiple time intervals and activity/channel proposals on reviewed captures.
   Evaluate misses and compute cost before altering the live observer. Cyclostationary features
   may support presence classification; its current candidate outputs are not trusted labels
   and must not become a hard gate that suppresses unseen waveforms without validation.
4. **Identity and unknown rejection.** Refit the existing identity model on verified positive
   intervals, with Mini 2 receiver/session diversity. Calibrate prototype/embedding distance
   and score criteria on independent known and unseen-drone groups. A positive qualified
   presence decision plus identity rejection may yield unknown drone; unfamiliar RF without
   positive presence evidence remains unclassified RF. Background and poor-quality inputs
   must not acquire an aircraft model. Shared encoders can be evaluated later if beneficial.
5. **Qualification and UI enablement.** Before enabling presence/identity decisions, freeze
   preprocessing, model and thresholds. Report false drone alerts/hour, encounter recall and
   latency, per-session/aircraft Mini 2 performance, wrong-model acceptance, unknown false
   acceptance and known rejection, with coverage and uncertainty. Evaluate gain, channel,
   receiver and mode shifts, and compare inference on/off resource impact. Add scoped temporal
   voting with expiry only after individual decisions are useful; repeated errors do not
   become evidence. Display no drone detected only for qualified, adequately observed coverage,
   never as a claim that no drone exists outside the observed time and band.

The missing prerequisite for presence training is reviewed background and positive interval
evidence with independent evaluation sessions. The new recorder/audit tools make that evidence
collectable and reproducible; they do not replace it with invented labels.

## Verification

- C++ GUI, inference regression target and offline probe build successfully.
- Seven Python tests cover score/presence separation, exact IQ and provenance recording,
  per-session byte limits, failure isolation, source preservation, temporal/view audit,
  live/direct/replay parity and tampered-IQ rejection.
- Five C++ regression suites pass: ML inference, scanner Wi-Fi lane, scanner LoRa security,
  GUI shell and serial lane. The ML fixture deliberately sends an unqualified identity at
  a 0.99999 score; the supervisor withholds it and the display stays undetermined.
- The actual C++ supervisor/Python worker recorded one interval from a temporary copy of
  the existing Mini 2 file. Offline replay matched the recorded result with the identical
  checkpoint hash; `drone_present` and `accepted_class` remained null. Evidence is saved as
  `ipc_probe.json` and `ipc_replay.json` alongside the input-audit reports. This probe is
  explicitly unlabelled/offline and is not part of the fresh-session training data.
- IPC tests require a local Unix socket pair, which the sandbox blocks; they run outside it.
  Synthetic tests and offline replay do not establish field detection performance.
- No live hardware was accessed and no existing model weights or source recordings were changed.
