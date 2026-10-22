# Expanded drone model: final development validation and insights

Checked: 22 October 2026 (Asia/Kolkata). Training, development evaluation and calibration diagnostics are complete. **The new residual model is not a justified replacement for the previous model, and neither is validated for live identity acceptance.**

## Completed work and integrity

- Both expanded models finished 40 epochs. The residual has 121,320 optimizer steps and 2,137,952 parameters; the compact has 179,093 parameters.
- Residual epochs 1–14 ran on CPU; epochs 15–40 continued on the RTX 3060 Ti with the original optimizer, scheduler, sampler and training settings. The CUDA RNG transition is recorded. This is a hybrid-device experiment, not a bitwise all-CPU replication.
- Both expanded checkpoint hashes match their training reports, evaluation, diagnostics and calibration audit. Final exported weights match their epoch-40 training states exactly; all model tensors are finite.
- All 960 known/unknown development spectrograms were verified byte-identical to the previous corpus, with matching labels, group identities, interval hashes, activity eligibility and selected frequency offsets. Each model uses its own training-fitted normalization.
- Reports and comparisons completed successfully; the trainer and calibration processes exited. No reserved final-test IQ was read. No dataset originals were changed.
- The new checkpoint passed the existing C++ supervisor/Python worker path using one pre-existing offline development IQ interval. Candidate, full score vector, selected offset, decision and null accepted identity match direct Python exactly. This is integration validation, not a labelled detection trial.

Checkpoint: `data/drone_ml/2026-10-20/expanded_v2/residual_model/model.pt`.
SHA-256: `c2488ef9fd0f6e16fd1bf60634d68dee6c0d10eb88ce5edd6a9496b5084e56af`.

## Same-development-data comparison

Known evaluation contains 432 windows from 36 recordings and six physical-unit groups, covering three known classes. Of these, 380 windows pass the structured-RF proxy. Group-balanced accuracy gives each unit group equal weight; recording votes average class scores across that recording. These are correlated development observations, not independent field trials.

| Metric | Previous residual | Expanded compact | New residual |
|---|---:|---:|---:|
| Known-window accuracy | 71.53% | 68.29% | 70.83% |
| Activity-eligible accuracy | 69.21% | 66.32% | 68.42% |
| Group-balanced accuracy | 68.40% | 69.21% | 67.71% |
| Recording-vote accuracy | 83.33% | 69.44% | 77.78% |

The new residual loses 0.69 percentage points of all-window accuracy and 5.56 points of recording-vote accuracy against the previous residual (28/36 versus 30/36 recordings). It improves aggregate accuracy over the expanded compact but has lower group-balanced accuracy. The automatic ranking favouring compact uses only group-balanced accuracy; it is not a deployment recommendation.

## Which classes improved or regressed

| Known class | Previous residual | Expanded compact | New residual |
|---|---:|---:|---:|
| DJI Air 2 | 146/180 (81.11%) | 152/180 (84.44%) | 134/180 (74.44%) |
| DJI Air 2S | 93/180 (51.67%) | 71/180 (39.44%) | 100/180 (55.56%) |
| DJI Mini 4 Pro | 70/72 (97.22%) | 72/72 (100.00%) | 72/72 (100.00%) |

- **Air 2 regressed:** 12 fewer correct windows. Its largest new confusion is Air 2S (25/180 windows).
- **Air 2S improved in aggregate but still fails on a separate unit:** unit 3 reaches 100/144 correct; unit 4 remains 0/36, with median maximum softmax 0.9733. Across both units, 47 windows are called Mini 4 Pro and 23 Mavic 3. High scores do not establish identity.
- **Mini 4 Pro improved to 72/72** across the two available calibration units. This small, previously inspected sample is not evidence of perfect recognition in other receivers, environments or modes.
- The new final training loss is 0.3384 versus 0.3448 previously, while generalization metrics declined. Lower training loss alone is not an improvement criterion.

## Unknown rejection: useful progress, still not qualified

The only unknown development aircraft model is DJI Mini 3 Pro, represented by two unit groups (528 windows; 480 eligible). It is absent from the 21 known output classes. There are no verified ordinary Wi-Fi/router/phone/background negatives in this assessment.

The raw maximum softmax median on unknown windows rose from **0.8717** (previous residual) to **0.9560** (new residual); expanded compact is 0.7046. If raw score ≥0.95 were treated as acceptance, the new residual would accept 257/480 eligible unknown windows (**53.54%**), compared with 132/480 (**27.50%**) previously. Setting the score threshold to 0.99 rejects every eligible known window for the new residual. A simple confidence threshold is not sufficient.

The diagnostic policy combines temperature scaling, class-prototype distance and support restricted to the three assessed classes. It is fitted using one known unit per class and unknown unit 1, then assessed on the other units. Prototypes use original eligible training embeddings. These groups were previously inspected; this is not a sealed test.

| Assessment metric | Previous residual | Expanded compact | New residual |
|---|---:|---:|---:|
| Correct known acceptance (99 windows) | 17.17% | 19.19% | 31.31% |
| Known abstention/rejection | 82.83% | 80.81% | 68.69% |
| Wrong known acceptance | 0.00% | 0.00% | 0.00% |
| Unknown false acceptance (372 windows) | 1.34% | 3.23% | 1.88% |

The new diagnostic policy correctly accepts **31/99** known assessment windows versus **17/99** previously, but falsely accepts **7/372** unknown windows versus **5/372**. It still abstains on 68/99 known windows. This is a trade-off, not a proven superiority at an equal false-acceptance rate. The ≤1% unknown false-accept target used during fitting does not hold on the other unknown unit (1.88%). Zero wrong known acceptances in 99 windows does not imply zero error generally.

New diagnostic parameters: temperature 1.37618, score threshold 0.85396 and maximum prototype distance 0.02405. These are retained as diagnostic evidence only. The checkpoint has no acceptance policy, the GUI default remains the previous residual, and candidate scores remain uncalibrated known-class scores rather than drone-presence probabilities.

## Inference timing and compatibility

A separate CPU measurement used PyTorch 2.14.1+cpu, two threads, batch size one, three warmups and 30 repeated forwards on one prepared calibration STFT. It excludes IQ preprocessing, acquisition, queues, IPC and display. Background machine load was not controlled, so this is a small timing sample rather than a throughput guarantee.

| Model | Mean forward | Median | Sample p95 |
|---|---:|---:|---:|
| previous residual | 49.81 ms | 49.58 ms | 51.08 ms |
| expanded compact | 1.89 ms | 1.87 ms | 1.99 ms |
| expanded residual | 55.84 ms | 55.74 ms | 58.83 ms |

The new model's one production-worker IQ probe measured **135.23 ms** for preprocessing plus model execution. This excludes process startup, acquisition and full end-to-end scheduling. It returned a candidate with `accepted_class=null`. No live USRP measurement or on/off performance-impact test was performed.

## Coverage that remains unvalidated

Only Air 2, Air 2S and Mini 4 Pro have independent cross-aircraft calibration groups. The other 18 output classes are training-only coverage:

DJI Avata 2, DJI FPV, DJI Inspire 2, DJI Matrice 100, DJI Matrice 210, DJI Matrice 300, DJI Mavic 3, DJI Mavic 3 Pro, DJI Mavic Mini, DJI Mavic Pro, DJI Mini 2, DJI Mini 5 Pro, DJI Phantom 4, DJI Phantom 4 Pro Plus, Parrot Disco, Parrot Mambo, SJRC F11 Pro, Yuneec Typhoon H.

The classifier cannot yet substantiate router-versus-phone-versus-drone decisions or an “unknown drone” claim from an unfamiliar RF signal. Publisher recording labels establish context, not verified emitter/window identity. Final held-out tests, broader unknown models, ordinary-RF negatives and live USRP/Mini 2 qualification remain pending. Mini 2 live work remains postponed as requested.

## Next justified work

1. Keep the current default and acceptance disabled. Retain the new checkpoint as a completed development candidate; do not treat another larger network or more epochs as the immediate fix.
2. Audit the Air 2S unit-4 failure using development recordings only: check activity/window truth, modes, signal occupancy, frequency-view selection and receiver/session differences. Receiver or dataset shortcuts are hypotheses to test, not established causes.
3. Add independent units/sessions for the 18 training-only classes, several entirely withheld unknown models, and verified Wi-Fi/router/phone/noise/interference negatives. This evidence gap cannot be repaired by more augmented windows from the same recordings.
4. Once window truth is improved, run controlled preprocessing/augmentation/loss ablations and repeat seeds with fixed group splits. Choose a model using per-unit generalization and matched rejection operating points, then freeze the model and policy before final held-out testing.
5. Later qualify live USRP captures, including Mini 2, and measure encounter-level misses, false alerts per hour, decision stability and inference-on/off resource impact.

## Evidence

- `data/drone_ml/2026-10-22/final_validation/verification.json`: checkpoint/hash checks, byte-identical development comparison and timing samples.
- `data/drone_ml/2026-10-22/final_validation/new-model-ipc-parity.json`: production inference parity.
- `data/drone_ml/2026-10-20/expanded_v2/comparison.json`: expanded model comparison.
- Each expanded model directory contains `training_report.json`, `development_evaluation.json`, `diagnostics.json` and `calibration_audit.json`.
- `data/drone_ml/2026-10-20/integration/previous_model_{calibration,diagnostics}.json`: earlier-model audit evidence.
- `docs/DRONE_ML_GPU_MIGRATION.md`: GPU migration details and repeatability limits.
