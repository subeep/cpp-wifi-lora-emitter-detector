# Expanded model training run — 14 October 2026

The user authorized training after the pretraining handoff. The larger residual CNN has
started optimization. This remains a standalone offline experiment, separate from the C++
application. Its inputs are the prepared workspace tensor shards; the job does not read
original SDA IQ, open a radio or access reserved final-test IQ.

Run directory: `data/drone_ml/2026-10-14/expanded_training_01/`.

- `job_status.json`: which architecture is running, completion or failure.
- `residual_model/progress.json`: epoch, batch, optimizer steps and training loss.
- `residual_model/training_state.pt`: resumable state, saved atomically after each completed epoch.
- `training.log`: completed-epoch losses and any errors.
- `launch.json`: original recipe, code hashes and launch provenance.
- `implementation/`: frozen executable code, so later working-tree edits cannot alter this run.

The original managed execution session stopped after completing epoch 24, with its last
progress record at epoch 25, batch 50. The temporary Python environment was subsequently
unavailable. Recovery uses `resume_job.py` in the run directory, the unchanged frozen code,
and the exact pinned package versions restored under `data/drone_ml/runtime-2026-10-14/`.
The recovery launcher uses a host process independent of the chat execution session and
an exclusive file lock to prevent duplicate recovery jobs. `process.json` records its host
PID once recovery starts; `process_before_recovery.json` preserves the original session
metadata. Check live progress as well as process metadata to establish that it is running.
The saved epoch-24 state is copied to `recovery_epoch24.pt` before continuing. Model,
optimizer, scheduler and random-generator states are restored, so epoch 25 is replayed
from its beginning. The baseline and development comparison remain queued afterward.

## Sequence

1. Residual CNN: 2,137,952 parameters, 21 classes, 40 fixed epochs.
2. Compact baseline: 179,093 parameters, the same corpus, seed, sampler and 40 epochs.
3. Save development-calibration reports and `comparison.json`.

The prepared recipe is unchanged: batch 8, seed 17, CPU with two Torch threads, AdamW
learning rate 0.0003, cosine decay, label smoothing and cross-unit contrastive loss.
The only trainer additions are atomic epoch checkpoints, resumability, progress logging
and checks that loss/gradients remain finite. A synthetic interrupted-run test verified
that resuming an epoch checkpoint produces exactly the same CPU weights as uninterrupted
training. The process runs with reduced scheduling and I/O priority.

At completion each model folder will contain an inference checkpoint `model.pt`. These
files do not exist merely because training has started. Training loss is not accuracy.
Development evaluation covers only the three known classes with calibration groups and
Mini 3 Pro unknown examples; it cannot establish accuracy for all 21 classes. No acceptance
policy is automatically enabled and no unknown-drone claim is made.

If a run fails, inspect `job_status.json` and its traceback. A completed-epoch state can be
resumed with the frozen snapshot's `expanded_training` CLI using the same corpus, review,
output directory, epochs, architecture and device, plus `--start-training --resume`.
Do not start a second optimizer against an active output directory. The recovery launcher
continues both architectures and the comparison in the original order; existing model
directories require a valid epoch checkpoint, while absent ones begin fresh training.
