# Residual training: CPU-to-CUDA continuation

The host has an NVIDIA GeForce RTX 3060 Ti with 8 GiB VRAM and a working NVIDIA
595.99.02 driver. The original training environment contains PyTorch 2.14.1+cpu;
it cannot use CUDA. GPU visibility also requires execution outside the restricted
sandbox. The first sandbox-only `nvidia-smi` failure did not indicate a host driver
failure.

An isolated CUDA environment is installed at
`data/drone_ml/runtime-cuda-2026-10-21/`, using the matching PyTorch 2.14.1+cu130
package from the official PyTorch wheel index. Neither the GUI's CPU inference
runtime nor the existing DJI decoder runtime is changed.

The continuation was launched from 14 completed CPU epochs. The previous CPU
process was stopped after the GPU probe and recovery tests passed. Its checkpoint,
configuration and process records are backed up under
`data/drone_ml/2026-10-21/gpu_migration/cpu_checkpoint_backup/`. Current process
metadata is in `data/drone_ml/2026-10-20/expanded_v2/process.json`; implementation
and hashes are frozen in that experiment's `cuda_implementation/` directory.

The Linuxbrew Python runtime needs an explicit driver-library search path. The
CUDA process sets `LD_LIBRARY_PATH` to its workspace `driver-libs/` directory,
which contains symlinks to installed NVIDIA driver libraries only. Adding all
system libraries caused a glibc mismatch and is not used. No system driver or
global library configuration was changed.

## Continuation contract

- Resume the last atomic completed-epoch checkpoint, including weights, AdamW
  optimizer, cosine learning-rate scheduler, CPU RNG and weighted-sampler RNG.
- Preserve the original 40-epoch target, batch size 8, float32 precision, corpus,
  training partitions, augmentation and loss. No mixed precision or batch-size
  tuning is introduced during this experiment.
- Preserve `run_config.json` as the original launch configuration. The actual
  device is recorded in `runtime.json`, `progress.json` and future saved states.
- Require explicit `--allow-device-change`; other configuration/hash changes
  still fail. Record checkpoint hash, transition epoch, runtime versions and the
  newly initialized CUDA dropout stream in `device_transitions.json`.
- A CPU dropout RNG state cannot be mapped to the CUDA stream. The continuation
  seeds CUDA with 17 plus the completed epoch count, then preserves CUDA RNG
  states on subsequent checkpoints. This is a documented hybrid-device run,
  not a bitwise reproduction of an uninterrupted CPU experiment.
- Retain a CPU checkpoint backup before the handover. The continuation runner
  takes the same exclusive experiment lock as the CPU job, preventing two writers.
- Read prepared workspace tensors only. Source datasets and reserved final-test
  partitions are not accessed by the continuation runner.

The training code retains strict deterministic algorithms, disables cuDNN
benchmarking and TF32 for CUDA, and sets the cuBLAS workspace configuration.
The fixed 16×40 final feature map uses an equivalent 8×10 non-overlapping average
pool on CUDA to produce the existing 2×4 output. Adaptive average-pooling backward
is unsupported by strict deterministic CUDA mode. CPU execution retains the
original operation; model parameters, checkpoint keys and feature regions are unchanged.
These controls improve repeatability on the same stack; they do not guarantee
CPU/GPU equivalence. See [PyTorch reproducibility guidance](https://docs.pytorch.org/docs/2.14/notes/randomness.html).

## Checks before handover

`tools.drone_ml.gpu_probe` loads a snapshot of the existing checkpoint and one
training-only batch. It checks CPU/GPU forward agreement, optimizer restoration,
finite loss/gradients and bounded GPU memory, then measures ten warm training
steps. It never saves the probe's updated weights. Fixed-batch speed excludes
disk loading and full-run overhead; estimate remaining runtime from the resumed
job's actual throughput instead.

The recovery tests additionally compare uninterrupted CUDA continuation with an
interrupted-and-resumed CUDA continuation from the same CPU checkpoint. This
tests preservation of CUDA RNG, optimizer, scheduler and sampler state.

All four recovery/pooling tests passed in the CUDA environment. The CPU ML suite
passed 71 tests, with the CUDA-only test skipped there. The actual-checkpoint probe
measured 82.9 ms per warm training step (batch 8), 1.33 GiB peak reserved memory,
finite loss/gradients and a maximum CPU/GPU logit difference of approximately
1.43e-6. These are probe measurements, not final model accuracy or sustained
throughput guarantees.

The actual continuation was observed advancing through epoch 15 at approximately
88 ms per batch including data loading, with 95% GPU utilization and about
1,604 MiB reported process VRAM use. Early throughput implies roughly two hours
of remaining training at handover, excluding evaluation and calibration. This
estimate will change with load and later measurements; the hourly monitor follows
the new process through `process.json`.

Evidence is stored under `data/drone_ml/2026-10-21/gpu_migration/`. GPU setup and
handover status must be checked there and in the experiment's process metadata;
this document alone is not proof that CUDA training has started.

After training, the existing development evaluation and calibration continue.
GPU acceleration does not supply missing independent aircraft, unknown-class or
non-drone evidence. No model promotion or live identity acceptance is automatic.
