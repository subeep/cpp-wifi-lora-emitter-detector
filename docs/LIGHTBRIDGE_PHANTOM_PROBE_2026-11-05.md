# Phantom recording investigation — 5 November 2026

No Lightbridge IQ-to-telemetry decoder has been implemented or validated.
This report records actual read-only waveform measurements and an existing
DroneID decoder baseline, not Lightbridge decoding success.

## Experiment

Added `tools/lightbridge_probe.py`, a reproducible bounded offline probe.
Read all 50 existing Phantom 4 files from `docs/lightbridge_input_inventory.json`:
three disjoint 20 ms intervals per recording, at beginning, middle and end.
Total inspected IQ is 3 seconds out of approximately 100 recorded seconds.
No original files were altered. Each interval retains source offset and SHA-256.

Features use the first 262,144 samples of each interval (about 4.37 ms), with
4096-point averaged spectra and linear real autocorrelation up to lag 8192.
Full 20 ms intervals are checked for nonfinite samples. Spectral power spans
include noise/interference and are not occupied bandwidth estimates for a
separated transmitter. Correlation peaks are not recovered symbol timings.

The existing `build/drone_native_worker` was replayed on all 30 sampled
intervals from the 10 clean-labelled recordings. Each invocation reset context
and received the complete 20 ms interval at its original sample rate. It exited
successfully without worker errors in all 30 cases and emitted **zero CRC-valid
DroneID packets**. This covers only 0.6 seconds and uses the existing strongest-
channel selection. It neither establishes absence of Lightbridge telemetry nor
validates a Lightbridge decoder. No pilot map, coded bits or telemetry fields
were recovered in this experiment.

Results and executable/result hashes:

- `data/validation/lightbridge-2026-11-05/phantom_probe.jsonl`
- `data/validation/lightbridge-2026-11-05/summary.json`

## Metadata correction and limitations

The author's acquisition description supports nominal 60 million complex
samples/second, center 2.4375 GHz, 28 MHz receiver bandwidth and interleaved
floating-point I/Q. See Figure 19 and its accompanying discussion:
https://repository.essex.ac.uk/34531/1/1900395_SWINNEY_Thesis_Submission.pdf

The separate local model reader declares 34 MHz usable bandwidth. That is not
the author's 28 MHz acquisition setting. The probe uses cf32 little-endian and
60 MSps, consistent with the existing reader and plausible numeric samples;
it does not assume 34 MHz usable passband. Neither source metadata nor finite
samples certify absence of dropped samples or clipping in every recording.

Clean is a dataset condition label, not packet-level proof of an isolated drone.
The available inventory lacks clean/Blue flying Phantom recordings. Mixed
Wi-Fi/Bluetooth recordings must not be used to infer drone-specific parameters
without separating their emitters. Short autocorrelation lags can result from
receiver filtering or interference, so none are promoted to Lightbridge markers.

## Next unresolved work

Full-recording time/frequency localization and channel-specific synchronization
are needed before recovering Lightbridge pilot allocation, scrambling,
interleaving, error-correction and checksums. Payload semantics require
independent telemetry evidence. Current data can support research but cannot
turn a guessed framing scheme into a verified decoder.

The probe's synthetic checks cover nonfinite/constant input, known-tone frequency,
correlation harmonics and preservation of complex64/complex128 inputs. They
verify measurement plumbing, not compatibility with the Lightbridge protocol.
No live acquisition, transmission, training, or changes to the native decoder
were performed. These sampled intervals are protocol-development observations;
they should not be represented as untouched final validation data later.
