# LoRa interpretation and capture/replay

Implemented 2026-09-18. This is the first interpretability milestone, not a new interoperable PHY or LoRaWAN parser.

## Live GUI

Select **LoRa (Sub-GHz)**. The packet table now distinguishes:

| Outcome | Meaning |
|---|---|
| Detected only | A preamble was found, but neither supported decoder recovered a valid header. |
| Header only | The SX-reference decoder accepted the header, but there were insufficient payload samples. |
| Payload / no CRC | Complete bytes were recovered; the header says no payload CRC is present. Integrity is unverified. |
| Payload / CRC failed | Bytes were recovered, but their CRC does not match. Do not interpret them as trusted identity data. |
| Payload / CRC valid | CRC matches under the named decoder. This does not authenticate a transmitter or prove real-radio interoperability. |

CRC is separately **Not checked**, **Absent**, **Failed**, or **Valid**. Header validity and payload completeness are independent. Header length is the declared length, including when the capture is incomplete. Hover an outcome for its explanation. Payloads display as exact hex, with printable ASCII in the tooltip; scroll horizontally to reach the payload column.

The decoder column distinguishes **SX reference (unvalidated OTA)** from the **Internal codec (nonstandard)**. The preamble-only path remains separately identified. SF/BW combinations are hypotheses; duplicate hypotheses are not unique packets or device identities. Coding rates display as 4/5 through 4/8.

Detailed synchronization failure stages remain unresolved: the existing demodulators do not expose enough information to distinguish every sync/SFD/timing failure. The internal codec still returns only complete results or detection fallback. No protocol/encryption inference is made from an undecoded packet.

## Save one recording

1. Connect the receiver and select LoRa mode; optionally lock to the desired frequency.
2. Expand **Save a LoRa IQ capture** and select a parent directory. Default: repository `data/lora_captures`.
3. Click **Save next completed capture**. This saves one completed LoRa listen buffer, including captures with no detections. It can be the buffer already in progress when clicked.
4. The status reports the absolute saved directory or an error. Each save uses a new directory. Stopping scanning cancels an unconsumed save request.

Saving happens on the scan worker and can briefly delay its next scan. It is deliberately one-shot, not continuous recording. Empty captures fail visibly; overflow recordings are saved as evidence and marked discontinuous. Files under `data/` are ignored by Git.

## Replay without a radio

Select LoRa mode, expand **Offline LoRa capture replay**, enter the saved capture directory and click **Replay capture**. The analysis runs in a background task; the GUI stays responsive. It does not require clicking Connect. Replay results are separate from live packet rows and **never feed the emitter registry, fingerprint log or persistent master list**. A corrupt/unsupported recording produces an error and clears the previous replay result.

Replay metadata includes source, sample count, actual sample rate, requested center frequency/gain/duration, host timestamp, receiver arguments, antenna and overflow warning. Short captures are flagged. Replay uses the same per-hypothesis decoder selection and integrity mapping as live reception, plus the same boxcar integer-decimation convention. Fingerprint extraction and identity matching are not replayed in this milestone.

For automation:

```sh
cmake --build build --target lora_replay
./build/lora_replay /absolute/path/to/capture-directory
```

The command never constructs a radio or modifies identity storage. Exit 0 means the recording was processed, even if no hypotheses matched; exit 1 means loading/analysis failed; exit 2 means invalid invocation.

## Recording format, version 1

Each capture directory contains:

- `manifest.json`: schema `rfmon-lora-iq`, version 1, format `cf32_le`, sample count, actual sample rate, requested settings, host capture-call Unix timestamp, overflow, source and a decoder-format revision label.
- `iq.cf32_le`: interleaved I then Q, IEEE-754 float32 little-endian, exactly eight bytes per sample.

A 64-bit FNV-1a checksum detects accidental IQ corruption. It is **not** an authenticity check, and does not authenticate metadata. The manifest is published after the IQ file finishes writing; a partial directory without a manifest cannot replay. Existing capture directories are not overwritten.

The loader bounds the manifest to 64 KiB and IQ to 32 million samples (256 MB, raised 2026-09-19 from 16 million/128 MB for Milestone 2 fixture collection - see lora_capture.cpp), checks version/format, exact file size, checksum, finite samples and metadata ranges. Rates must support an integer ratio to at least one of 125/250/500 kHz; unsupported hypotheses are skipped instead of silently rounding the sample rate.

The timestamp is host time at entry to the capture call, including subsequent setup/settling; it is not hardware sample-zero time. Center frequency and gain are requested values, not measured/read-back values. `device_args` is the configured connection string, not a verified radio serial number. Sample count/rate gives the duration of returned samples; overflow means continuity is lost. The format revision is not a Git commit identifier. Keep the source revision alongside externally shared fixtures when exact reproducibility matters.

## Validation and remaining work

`test_lora_observation` covers all integrity states, actual synthetic CRC-off/corrupt/truncated waveforms, codec provenance, silence, binary endianness, save/load/replay equivalence, unique saves and rejection of malformed/oversized/truncated/corrupted files and non-finite IQ. Existing PHY tests still pass. These waveforms use project encoders and do not prove interoperability.

`test_lora_gui` renders the production table and replay controls offscreen without a receiver. It checks rendering, not physical reception. Live capture saving still needs a hardware acceptance run once UHD discovery works.

Next: resolve the documented receiver/API blockers, collect independently verified RF fixtures, and correct PHY behavior against those fixtures. LoRaWAN parsing, firmware-aware board control, MIC verification, application decryption and improved identity provenance remain later milestones. See [TARANGMINI_ASSESSMENT.md](../TARANGMINI_ASSESSMENT.md).
