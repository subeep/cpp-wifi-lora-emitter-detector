# Standalone IQ and FFT inference interface

Implemented 13 October 2026. This component is experimental and optional. It lives in
`tools/drone_ml/`; the C++ application does not import, launch or depend on it. No receiver,
GUI, scanner, cyclostationary, decoder, security or CMake changes are part of this work.

## Current API

```python
from tools.drone_ml.predict import InferenceEngine

engine = InferenceEngine("path/to/model.pt")  # load weights once
result = engine.predict_iq(iq, rate=100_000_000, offset=0)
# Alternative: time-by-frequency complex FFT or squared-magnitude frames.
result = engine.predict_fft(frames, metadata)
```

Each call consumes one complete contiguous interval with the checkpoint's duration and
geometry. The existing IQ file command remains supported. Numerical FFT files use a
bounded C-order NPY v1/v2 array plus a JSON sidecar:

```bash
/tmp/rf-stft-venv/bin/python -m tools.drone_ml.predict \
  --model data/drone_ml/2026-10-13/long_activity_model_verified/model.pt \
  --fft data/drone_ml/2026-10-13/fft_input_verification/frames.npy \
  --fft-metadata data/drone_ml/2026-10-13/fft_input_verification/frames.json
```

`preprocess.fft_frames(iq, source_rate, config)` is the reference producer and returns
both arrays and metadata. It performs exactly the existing source conditioning and
frequency-view preparation before computing the FFT. Producing FFTs in this helper
still costs host compute; savings require an upstream producer that already supplies
compatible FFTs. This is not a hardware FFT driver or continuous stream assembler.

## Producer contract

| Field | Required meaning |
|---|---|
| `schema` | `rfmon.drone_ml.fft.v1` |
| `preprocessing` | Exact checkpoint `STFTConfig.metadata()` contract, including duration, selected-view sample rate, nfft, hop, Hann window and preprocessing version |
| `representation` | `complex_fft` or `power_fft`; power means squared FFT magnitude before dividing by Hann energy |
| `bin_order` | `fftshift` or `natural`; always full two-sided frequency coverage |
| `forward_scale` | Positive FFT amplitude multiplier: stored complex FFT = scale × unnormalized DFT; stored power = scale² × squared DFT magnitude |
| `input_conditioning` | `source_mean_removed_unit_rms_before_view_v1` |
| `source_rate_hz` | Original IQ sample rate before view selection/resampling |
| `selected_offset_hz` | Selected view centre relative to original capture centre, following the checkpoint selector |
| `frame_start_samples` | Exact ascending frame starts `[0, hop, 2*hop, ...]`, measured at the selected-view rate relative to this interval |
| `interval_start_sample` | Nonnegative original-source sample index; zero for an isolated interval |
| `dropped_samples` | Zero; gaps must end the interval, never be silently filled |

Absolute RF centre and hardware clock/session provenance should be retained by a future
acquisition wrapper; they are not model inputs in this version. Relative sample indices
provide within-interval timing, not synchronization across devices or independent calls.

Crucially, input conditioning removes the mean and normalizes RMS over the **original
source interval**, before frequency mixing and Kaiser polyphase filtering/resampling.
A producer must reproduce this operation and the automatic selector where enabled.
Normalizing each FFT independently, or normalizing just the narrowed frequency view,
is a different input distribution. Merely setting a metadata field does not verify
that hardware performed the operation; hardware parity must be measured later.

The adapter converts both representations to the same float32 frequency-by-time
log-power tensor, then uses the same normalization, activity check, CNN and acceptance
policy as IQ inference. It rejects gaps, wrong geometry, incompatible preprocessing,
unsupported bin orders, NaNs, negative power and over-budget arrays. NPY loading is
bounded before allocation and forbids object/pickle payloads. Limits apply to one model
input allocation; they do not change radio sample rates.

An averaged spectrum, max-hold spectrum, single snapshot, screen image or arbitrary
FPGA FFT cannot be passed as a compatible sequence. Raw, unconditioned magnitude FFTs
cannot generally recover the original source normalization and filtering contract.
Retain the IQ path for cyclostationary analysis and decoding.

## Integration boundary for the application

Future integration must preserve these properties:

1. Optional worker process, disabled by default, with no radio ownership or changes to
   acquisition settings. The existing application remains usable with the worker absent.
2. Feed copied intervals through a bounded, nonblocking queue. Queue saturation drops
   optional inference work and reports a counter; acquisition never waits for inference.
3. Carry capture identifiers, frequency, sample rate, sample origin and discontinuity
   information with every job. Reject incomplete inputs. Discard stale results after
   retunes, restarts or expiry, and never concatenate across gaps.
4. Place results in a separate experimental output channel. They must not overwrite
   existing detector decisions, alter security thresholds or trigger actions implicitly.
5. Apply worker CPU/memory limits, detect crashes/timeouts and allow restart/disable.
   Qualify acquisition drops, scanner timing and GUI responsiveness with inference on/off
   before enabling the option. Process separation alone does not eliminate resource contention.
6. Maintain an independently versioned input contract, checkpoint and rejection policy.
   Validate Python/C++ or RFNoC producer parity before enabling each producer.

This boundary is documented for later integration; no live worker, IPC queue, GUI switch,
ONNX export or C++ engine is installed by this change.

## Verification and identification limits

The FFT tests cover native and downsampled IQ, explicit/automatic frequency views, 1 ms
and 8 ms context, scaling and bin-order conversion, timing/shape rejection, unsafe NPY
headers, activity rejection and policy status. All 47 standalone ML tests passed.

A previously derived 8 ms development probe was checked against the preserved preprocessing
implementation and existing long-context checkpoint. Its old IQ tensor, current IQ tensor
and FFT-input tensor were exactly equal; candidate score and decision also matched. This
reuses development material, not a new held-out evaluation. No SDA dataset read was needed.
Artifacts and implementation hashes are recorded in
`data/drone_ml/2026-10-13/fft_input_verification/verification.json`.

The checkpoint still returns `identification_unavailable`: its calibration did not support
an acceptable named-model policy. The new input path does not establish accuracy, unknown-drone
recognition, realtime throughput or USRP hardware FFT compatibility. Improving model
separation, adding trustworthy non-drone examples and validating unseen units remain required.
