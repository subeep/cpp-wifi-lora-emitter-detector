# Temporal drone model candidate

23 October 2026, Asia/Kolkata. This is a separate, untrained offline candidate.
The existing application, receiver, GUI model and checkpoints were not changed.
The NIST download and its existing dependent verification job continue separately.

## Architecture implemented

`tools/drone_ml/temporal_model.py` defines `TemporalDroneModel`.

1. Each numerical 256 x 625 STFT tile represents 8 ms at 20 MSps, with a 256-point
   periodic Hann FFT and 256-sample hop. No image resizing or color conversion.
2. Shared compact CNN: convolution channels 16, 32, 64, 96; kernels 5, 3, 3, 3;
   stride 2; GroupNorm and SiLU. Adaptive 4 x 4 pooling feeds a 64-dimensional
   feature layer. This uses the existing compact architecture, newly initialized,
   rather than importing the identity checkpoint's weights.
3. Six residual causal temporal blocks, each with a 64-channel convolution,
   kernel length 3 and dilations 1, 2, 4, 8, 16, 32. Left padding only. LayerNorm
   operates on channels at each time step; normalizing across time would leak
   future inputs into earlier outputs.
4. Separate two-logit presence and 21-logit identity heads, plus a normalized
   64-dimensional embedding for later rejection-method comparisons.

The candidate has **254,103 parameters**. Its maximum receptive field spans 127
tiles (1.016 seconds); actual evidence cannot exceed the supplied context. The
initial 16-tile configuration supplies 128 ms, not 1.016 seconds of evidence.
Short context left padding is architectural padding, not invented observed RF.
No calibrated probabilities, learned unknown detector or acceptance thresholds
are available. Embedding distances/energy/max-logit rejection still need fitting
and comparison on reviewed held-out groups.

The masked joint-loss helper permits presence labels on background/drone rows,
but identity loss only on verified known-drone rows. Background, unidentified
drone and unlabelled rows cannot train a known identity. A label value of -1
means unavailable. This helper does not override protected dataset partitions.

No real-data training driver, trained temporal checkpoint or live streaming
integration is included in this increment. Existing checkpoint loaders and the
GUI remain unchanged. Tests perform a synthetic forward/backward pass only.

## Input preparation implemented

`tools/drone_ml/temporal_inputs.py` prepares a bounded sequence from one explicit
train/development source record. It verifies source hashes, preserves source
file-stat tuples, records sample offsets and maintains one fixed frequency view
throughout the context. Reserved/unspecified partitions fail before IQ access.
Short recordings fail instead of padding or joining unrelated captures. Outputs
must be outside source/manifest directories.

Each 8 ms tile is exactly equal to the existing fixed-view numerical STFT
preprocessor. It retains existing per-tile mean removal and unit-RMS treatment;
a training-set normalization has not been fitted. The tensor shape is
`[time_tiles, 256, 625]`; callers add batch/channel dimensions for the model.

Two 128 ms previews were prepared under:
`data/drone_ml/2026-10-23/temporal_candidate_01/`:

- `tampere_mavic_pro/`: first 128 ms of the previously reviewed Tampere Mavic Pro
  training source, fixed offset +22 MHz within the source's 120 MSps recording.
- `local_mini2_context/`: first 128 ms of the user's linked Mini 2 development
  recording, fixed offset -1.25 MHz at source rate 25 MSps.

Each tensor is 10.24 MB before `.npy` header overhead. Metadata records source
hash, grouping, provenance, view and all sixteen native sample offsets. Both
previews have `training_eligible=false`, no verified presence/identity labels,
and no live qualification. The local recording's unresolved acquisition
continuity remains explicit. Adjacent array offsets cannot prove gap-free RF
acquisition; the preview does not certify continuity or label every tile drone.

## Initial CPU measurement

`tools/drone_ml/benchmark_temporal.py` measured newly initialized weights on
synthetic numerical inputs. CPU, two PyTorch threads, batch one, two warmups and
ten measurements per context, PyTorch 2.14.1+cpu. Host load was uncontrolled.

| Context | Tiles | Mean forward | Sample p95 |
|---|---:|---:|---:|
| 64 ms | 8 | 27.19 ms | 28.87 ms |
| 128 ms | 16 | 84.47 ms | 86.65 ms |
| 256 ms | 32 | 183.49 ms | 186.59 ms |
| 512 ms | 64 | 382.19 ms | 385.15 ms |

These timings exclude IQ selection/resampling/STFT, acquisition, queueing, IPC
and display. Observation duration adds latency. They are not an end-to-end
real-time qualification or an accuracy result. Overlapping contexts currently
recompute the CNN; later live engineering should reuse features and clear
history on view/source changes and gaps. No GPU benchmark was run here.

Full timing metadata and model-source hash:
`data/drone_ml/2026-10-23/temporal_candidate_01/benchmark.json`.

## Verification and next steps

Six additional tests cover causal prefix invariance, absence of future leakage,
masked identity gradients, finite forward/backward computation, exact agreement
with the existing STFT, fixed-view offsets, short/out-of-band input rejection,
reserved-file exclusion and source preservation. Together with the seven
public-source/timing tests, all 13 targeted checks passed.

Next: finish and verify the NIST transfer; review representative exact frequency
views; collect independent background/drone groups from several sources; add a
reviewed temporal corpus and training/evaluation driver; compare this candidate
against the separate presence baseline at matched false acceptance. Do not use
randomly split neighboring NIST timestamps as independent validation sessions.
Select context using measured generalization and end-to-end cost, rather than
the shortest forward time or a paper's reported accuracy. Public-data review,
unknown-model holdouts, final tests and local USRP qualification remain pending.
