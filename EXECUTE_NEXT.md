# Execute next: Milestone 2 - verified real-radio LoRa PHY decoding

Status: implementation and validation plan; not an implementation-completion report.
Prepared: 2026-09-18. Repository: `newrocktest-cpp`.

## 1. Objective and completion boundary

Recover the correct on-air PHY payload bytes from independently verified real-radio IQ recordings, repeatably, for explicitly listed operating modes. Preserve partial evidence and explain failures in the application.

Do not equate a visible burst, a valid header, readable text, a successful UART write, or an internal encoder/decoder round trip with interoperability. No finite test suite guarantees an error-free decoder. This plan instead requires reproducible fixtures, independent expected results, negative tests and a measured support boundary.

Milestone 2 has two gates:

- **M2A, first mode:** repeatable, exact decoding of one real-radio configuration on both development and withheld captures.
- **M2B, declared support:** the same evidence for every additional mode advertised as supported, plus robustness, negative-input and live-integration checks.

M2A is useful progress but is not completion of broad support. If equipment only supplies one configuration, report that limitation and the remaining M2B work rather than advertising all SF/BW combinations.

In scope: capture correctness, synchronization, symbol recovery, PHY header/payload processing, CRC, diagnostics, reproducible replay tests and honest GUI reporting.

Deferred: LoRaWAN MAC interpretation, device-specific sensor payload codecs, application decryption, MIC verification, universal device identification, collisions/multiuser decoding, Wi-Fi changes and the separate Wi-Fi bench. An encrypted on-air payload can still be recovered as bytes; application encryption is not a sufficient explanation for header or PHY CRC failures.

## 2. Latest context: preserve the now-working transmitter

**The user reports transmission is working again.** The exact fix, current board identity, firmware and command sequence have not yet been recorded. Earlier hardware/API failures are historical evidence, not proof that the current setup is still broken.

Before touching radio settings, record the working setup with the user:

1. Exact command/script and invocation currently producing transmissions; save its contents or hash, not just its filename.
2. Module hardware and firmware identity, USB adapter serial/path, UART baud and API/transparent mode.
3. What changed to restore operation: board, firmware, operating mode, script, wiring, antenna, frequency or another setting. Unknown is an acceptable recorded answer.
4. Carrier frequency, SF, bandwidth, coding rate, header mode, CRC presence, sync word, preamble length and LDRO state. Mark unknown values as unknown; the existing TarangNet API does not expose all of these.
5. Confirmation source for each value: setting readback, documented fixed firmware behavior, independent RF measurement or inference.
6. Exact UART payload bytes, complete command frame, response and timestamp for a successful attempt. Record the real RF observation separately.

Do not replace the working script with the recently guarded sweep merely to standardize tooling. Preserve it, inspect it, and use the proven path for fixtures once its behavior is understood. Do not bypass the new firmware guard or invent LW-S201 commands. No firmware upgrade, defaults reset or forced flash write is a prerequisite for this decoder milestone.

The earlier queried module reported `TarangConnect_LW_S201_LoRaWAN_CA_v1_0_4`; the supplied API document targets `TarangNet_TN_STD_WL_v0_0_4`. Two USB adapter identities appeared. Neither fact establishes today's configuration. Firmware software-version text does not establish a LoRaWAN specification version.

**Entry gate:** the working transmitter configuration is documented and reproducible, and a matching RF signal is observed at the receiver. If serial automation remains unclear, use the user's working transmitter procedure and proceed with receive-only capture/replay work.

## 3. Current implementation map

| File / component | Current behavior | Milestone 2 action |
|---|---|---|
| `src/lora_phy_std.cpp/.hpp` | SX-reference codec; fixed sync requirement, `ppm = sf`, integer-bin CFO path; returns valid-header partial results on insufficient payload samples | Add stage diagnostics; fix only demonstrated PHY failures against independent evidence |
| `src/lora_phy.cpp/.hpp` | Internal, nonstandard codec plus burst detection | Preserve regression behavior; prevent fallback success from being counted as standard interoperability |
| `src/lora_observation.cpp/.hpp` | Shared per-hypothesis reporting; standard result takes precedence when header is valid | Expose candidate provenance, failure stage and competing attempts; test selection rather than hiding a stronger result behind a partial one |
| `src/scanner.cpp` | Captures at profile rate, tests integer-decimated SF/BW hypotheses, records live fingerprints | Use validated shared analysis; surface skipped-rate hypotheses and capture quality; preserve identity behavior |
| `src/config.hpp` | 2-second LoRa windows; SF5-12 and BW125/250/500 hypotheses | Make duration/configuration suitable for measured packet airtime; search range does not mean verified support |
| `src/lora_capture.cpp/.hpp` | Version-1 manifest and little-endian complex float32 IQ; limits and checksum | Preserve old recordings; add session/reference evidence in sidecars or explicitly version extensions |
| `src/sdr_capture.cpp/.hpp` | Returns IQ, actual sample rate and overflow; can return a short capture | Improve observable transport/continuity metadata if needed without changing unrelated scan behavior |
| `src/lora_gui.cpp/.hpp` | Explicit integrity labels, raw hex, tooltips and offline replay | Add stage explanations and verified-mode provenance; retain absent/failed/not-checked distinction |
| `tools/lora_replay.cpp` | Offline text output; exit 0 even for no matches | Add proposed machine-readable diagnostic output and a fixture checker with real pass/fail semantics |
| `tools/tarangnet_api.py`, `tools/tarangmini_sf_bw_sweep.py` | Exact-version TarangNet guard, role-aware TX and checked responses | Preserve guards; do not assume these tools drive the now-working transmitter |
| `tests/test_lora_observation.cpp` | Reporting, synthetic waveforms and capture-file validation | Keep it; add independent fixtures rather than substituting new circular tests |

See [HANDOVER.md](HANDOVER.md), [TARANGMINI_ASSESSMENT.md](TARANGMINI_ASSESSMENT.md), [capture/replay documentation](docs/LORA_CAPTURE_REPLAY.md), and [sweep documentation](docs/TARANGMINI_SWEEP.md).

## 4. Establish a reproducible software baseline

The workspace already contains uncommitted LoRa changes, PDFs and unrelated bench work. Do not reset, clean, overwrite or stage unrelated files. Record both the commit and the working-tree differences; a commit SHA alone does not identify the tested code.

Run from the repository root. These targets currently exist:

```sh
git status --short
git rev-parse HEAD
cmake -S . -B build
cmake --build build --target rf_monitor_gui lora_replay test_lora_observation test_lora_phy_std test_lora_phy test_lora_master -j2
./build/test_lora_observation
./build/test_lora_phy_std
./build/test_lora_phy
./build/test_lora_master
python3 -m unittest discover -s tests -p test_tarangmini_tools.py -v
```

Where EGL dependencies are available, the offscreen target also exists:

```sh
cmake --build build --target test_lora_gui -j2
LIBGL_ALWAYS_SOFTWARE=1 ./build/test_lora_gui /tmp/lora-m2-baseline.ppm
```

Save build/test output, compiler/UHD versions, source-file SHA-256 hashes and the relevant patch in the session evidence directory. Include untracked LoRa source/test files in the snapshot; `git diff` alone omits them. Do not assume `ctest` discovers these targets: check registration before relying on it.

**Gate:** existing relevant tests pass, or a pre-existing failure is separately documented before any PHY correction. Baseline passes establish regression health, not interoperability.

## 5. Capture session procedure

### 5.1 Receiver readiness

Use the current working radio connection. Do not probe a receiver concurrently with a GUI or another process that owns it. If disconnected, verify UHD connectivity before testing RF; ICMP ping alone is insufficient.

Record device identity, connection string, antenna port, requested gain/AGC, requested and actual sample rate, requested center frequency, host timestamp and any available actual tuning/readback. The current manifest stores requested frequency/gain and host capture-call time, not hardware sample-zero time or confirmed device serial. Do not relabel these as measurements.

Use the actual transmitter frequency. An old emitter row elsewhere in the band does not confirm today's carrier. For the initial single-transmitter test, keep the signal comfortably inside the sampled passband, use a stable manual receiver gain where appropriate, and lower gain or change physical spacing if samples saturate. If using a conducted connection, use a correctly rated attenuated setup rather than a direct transmitter-to-receiver cable.

At the current 500 ksps capture rate, a nominal 500 kHz signal has little frequency-offset/filter margin. Begin with BW125 if supported. Wider modes may need a higher verified capture rate and proper channel filtering. Record the actual rate rather than relying on a GUI request.

### 5.2 Start with one known mode

Prefer SF7/BW125 and a short payload **only if the working setup supports and verifies them**. Otherwise start with its proven fixed mode. Do not change an unknown firmware's configuration just to match this preference.

Keep frequency, PHY settings, transmitter identity and receiver gain constant for the first comparison set. Use one transmitter and separate packet transmissions enough to identify complete bursts and avoid collisions. Let documented transmitter/network timing constraints govern the interval; do not assume a fixed small gap fits every SF or firmware state.

For a firmware that accepts arbitrary binary application data, collect:

- 16 bytes of `00`.
- 16 bytes of `FF`.
- Bytes `00` through `0F`.
- A short ASCII marker plus a sequence number, with exact encoding recorded.
- Then lengths 1, 2, 3, 7, 8, 15, 16, 31 and 32 bytes, where accepted.

Use the actual supported lengths and record rejections. An API may reject an empty payload; empty-payload software tests do not prove a radio supports it. Do not pad or silently modify the input just to get a passing test.

If the firmware encrypts or wraps application data, these patterns describe **UART input**, not necessarily expected on-air PHY bytes. Counters, nonces and framing may make every transmitted PHY packet different. Store expectations per captured packet.

### 5.3 Ensure the whole packet is captured

Current LoRa listen duration is **2.0 seconds**. It must not be assumed sufficient for every slow packet, long preamble, payload or header/LDRO configuration.

1. Obtain airtime from verified settings and a validated calculation, or measure the complete burst in a longer reference recording.
2. Arrange reception before packet onset; include preamble and trailing margin after the packet.
3. Use the GUI's **Save next completed capture** control for early fixtures. It can save a buffer already in progress when clicked; it is not a synchronized transmitter trigger.
4. Inspect the recording boundaries. If a packet starts before the buffer or ends after it, label it truncated, retain it as a negative fixture and capture again.
5. If GUI windows cannot reliably contain the packet, first implement configurable listen duration or a dedicated finite receive-only capture tool using the same capture format. Enforce memory limits and keep it separate from Wi-Fi scheduling.
6. For long-term streaming, explicitly design overlap/carry-over and packet deduplication; do not concatenate discontinuous scan windows and pretend they are continuous IQ.

The loader currently permits at most 16 million complex samples (128 MB of IQ). Respect this before allocating longer recordings. Account for DSP working memory as well as the raw file.

### 5.4 Qualify every recording

For each capture, record:

- `N / actual_sample_rate` duration and whether it meets the requested duration.
- Overflow, timeout/short-read evidence, malformed metadata or missing samples.
- Finite-sample check; sample magnitude distribution, peak/RMS and any observed saturation plateau. A float value near 1 alone is not universal proof of clipping; document the receiver scaling/criterion used.
- Packet start/end relative to capture boundaries, expected carrier position and independent evidence of a complete burst.
- A noise-only capture at the same receiver settings.

Do not classify transport loss as a PHY algorithm failure. Do not discard it silently either: retain and report exclusion reasons. Overflow IQ may contain concatenated segments with gaps, so it is unsuitable as a clean correctness fixture.

**Capture gate:** complete, unclipped, continuous recordings exist for the first known mode, and at least one independently verifiable packet is available. If the RF conditions are uncertain, fix acquisition before changing coding tables.

## 6. Evidence layout and expected results

Suggested new directory layout; these paths are proposals, not existing files or tools:

```text
data/lora_m2/<session-id>/                 # raw captures; ignored by Git
  session.json
  transmitter.txt
  source-snapshot/
  captures/<capture-id>/
    manifest.json                        # current capture format
    iq.cf32_le
    evidence.json                        # new sidecar
    reference-output.json                # preserved external result
    baseline-replay.txt
    diagnostic-trace.json
  results.json
  exclusions.json

tests/fixtures/lora_m2/                   # small redistributable verified fixtures
  README.md
  <fixture-id>/...
docs/LORA_M2_RESULTS.md                   # planned validation report
```

Prefer a small representative tracked regression corpus; raw `data/` files are ignored and can be lost from a fresh checkout. For external large fixtures, provide an immutable artifact location, SHA-256, acquisition provenance and a reproducible download procedure. A required acceptance job must fail clearly if its corpus is missing rather than passing with zero tests.

Example sidecar template; `null` means unknown and must not be replaced with a guess:

```json
{
  "fixture_id": "session-a-packet-001",
  "origin": "hardware",
  "split": "development",
  "iq_sha256": null,
  "manifest_sha256": null,
  "transmitter_firmware": null,
  "uart_payload_hex": null,
  "uart_command_hex": null,
  "uart_response_hex": null,
  "phy": {
    "sf": null, "bandwidth_hz": null, "coding_rate": null,
    "header_mode": null, "crc_present": null, "sync_word": null,
    "preamble_symbols": null, "ldro": null
  },
  "expected_packets": [
    {
      "start_sample_range": null,
      "end_sample_range": null,
      "phy_payload_hex": null,
      "payload_crc_valid": null,
      "oracle": null,
      "oracle_revision": null
    }
  ],
  "capture_quality": {
    "overflow": false, "complete_packet": null, "clipping_assessment": null
  },
  "eligible_for_exact_byte_acceptance": false,
  "notes": "Template only; not a verified fixture"
}
```

Define whether oracle output includes/removes the PHY header or CRC. Compare the same byte layer with the same byte order. Preserve any conversion script, original source IQ and its hash. If a reference requires resampling, record filter coefficients, rate ratio, delay and sample-index conversion.

Separate three sources of evidence:

| Evidence | What it proves |
|---|---|
| UART write or module ACK | Host/module command handling; not SDR reception |
| Independent receiver/decoder with matching captured event | Candidate on-air bytes, timing and supported integrity checks |
| Our decoder matching those bytes on the same IQ | Interoperability for that recorded configuration |

A CRC match alone is not an independent byte oracle. If no oracle can recover a packet, label it unresolved; never populate the expected bytes from our decoder's output and call that independent validation.

## 7. Set up an independent reference

Candidate primary sources, reviewed 2026-09-18:

- [EPFL gr-lora_sdr](https://github.com/tapparelj/gr-lora_sdr): a separate GNU Radio implementation with configurable PHY parameters and receive synchronization/decoding. Its README documents limitations for SF5/SF6; inspect the pinned version's actual hardware compatibility before using those modes as an oracle.
- [LoRaPHY](https://github.com/jkadbear/LoRaPHY): a separate MATLAB PHY implementation, useful as another comparison path where its environment and selected mode are supported.

These are candidates, not guaranteed answers for this module. Pin a commit, record configuration and dependencies, and verify the reference first on a known physical or independently generated vector. Keep the reference separate from the production decoder. If reusing source, review its license and attribution requirements before incorporating it; using it as an external test oracle does not require importing its code into this app.

Reference procedure:

1. Feed the **same saved IQ** to the reference; preserve its input conversion exactly.
2. Explicitly set or independently determine required SF/BW/header/LDRO/sync options. Do not brute-force unbounded parameters until a coincidental CRC succeeds.
3. Save output bytes, packet boundaries, decoded header and CRC status; retain failures too.
4. Repeat on more than one capture and payload pattern.
5. Cross-check one or more disputed cases using another implementation or a documented hardware receive result matched to that packet.
6. If references disagree, quarantine the fixture from acceptance until the byte layer/configuration discrepancy is resolved.

Do not rerun an SDR transmit/receive example merely to decode a file; use a file-input receive-only flowgraph. This task's primary requirement is an independent receive comparison.

## 8. Add diagnostic contracts before fixing algorithms

Introduce a structured attempt result, preferably in `lora_phy_std.hpp` with supporting types in a new diagnostic header if needed. Exact type names below are proposed. Do not claim these fields already exist.

Each attempt should retain:

- Candidate ID, decoder implementation/revision and original-capture sample offsets.
- SF/BW hypothesis, actual processing rate and configuration provenance.
- Last completed stage and a reason code; distinguish not attempted, insufficient samples, unsupported mode and failed validation.
- Preamble start/length, CFO estimate with units, fractional timing estimate and uncertainty/quality measures where meaningful.
- Observed sync-symbol bins, inferred/expected sync interpretation and SFD location.
- Raw header symbols, transformed symbol values, deinterleaved codewords, header nibbles/fields and checksum comparison.
- Declared payload length, CR, CRC-present flag and LDRO assumption/source.
- Required versus available symbols/samples, recovered bytes and calculated/received CRC when applicable.

Suggested reasons include `no_preamble`, `unsupported_rate`, `sync_mismatch`, `sfd_unresolved`, `header_truncated`, `header_checksum_failed`, `header_fields_unsupported`, `payload_truncated`, `fec_unrecoverable`, `payload_crc_failed`, and `complete`. Emit a reason only when the decoder has evidence for it; do not invent FEC failure detection if the current routine merely chooses a nearest codeword.

Make trace collection optional and bounded. Normal live scanning must not allocate unlimited per-symbol logs. Provide deterministic JSON export for offline diagnosis; keep exceptions/input errors separate from ordinary no-packet results.

**Diagnostic gate:** a failing capture identifies the first measurable divergence from the independent trace or expected stage output. Logging alone must not change recovered bytes/statuses on existing fixtures.

## 9. Fix the receive chain in dependency order

For every correction: retain the pre-fix failure, state the evidence, implement the smallest coherent change, add an independent test, rerun affected fixtures, then record the before/after trace. Do not change synchronization, interleaving and CRC together and infer which fix mattered.

### 9.1 Sampling and channel conditioning

Audit actual-rate handling, sample-index mapping, IQ orientation and decimation. Current integer-ratio checks can skip hypotheses silently; surface this as unsupported processing rather than reporting simply no packets.

The existing boxcar decimator is shared in convention but duplicated between live/replay code. Test parity and consolidate the validated operation when changing it. If introducing a proper channel filter/resampler, document passband, stopband, delay, normalization and filter state across chunks. Preserve enough oversampling for the chosen synchronization design rather than prematurely forcing one sample per bandwidth interval.

Tests: known tones/chirps around passband edges, exact and incompatible rates, constant input, IQ conjugation diagnostic, group-delay/sample-offset accounting, and live/replay parity. Do not silently conjugate every input until one candidate passes without recording the selected orientation.

### 9.2 Preamble, CFO, timing and SFD

The current path uses an integer-bin estimate and a symbol-grid search. Diagnose whether the actual packet starts between those grid positions. Establish a consistent frequency-sign convention and separate timing-offset effects from CFO using the relevant chirp observations.

Test fractional CFO, integer CFO and arbitrary arrival offsets independently before combining them. Validate sync symbols and SFD at the recovered timing; do not assume a fixed preamble length. Record the original sample-zero mapping through resampling/decimation.

Acceptance: the independently located preamble/SFD and first data symbol align within a documented tolerance, and the corrected symbol sequence agrees on clean fixtures. The tolerance must be justified by sample rate and reference precision, not adjusted per failing packet.

### 9.3 Sync-word handling

Audit the hard-coded `SYNC_WORD_DEFAULT = 0x12` comparison and the mapping from observed symbol bins to network sync values. Use reference/hardware evidence for the selected mode. Do not replace the constant with another value and claim general interoperability.

Expose observed sync separately from a configured filter. Bound any candidate set and record which interpretation was used. Sync recognition is not authentication or device identity.

### 9.4 Explicit header and first block

Priority audit: `ppm = sf` is used throughout the current SX-reference implementation. Verify first-block effective width/reduced-rate mapping, symbol offsets, Gray transform direction, diagonal interleaving, nibble ordering, header FEC and checksum against independent vectors.

Determine which first-block bits carry header versus payload under the selected format. Distinguish a bad checksum from a checksum-valid but unsupported field combination. Header validity must not imply payload availability.

Tests: multiple independent lengths/CR/CRC flags, first-block boundary cases, truncated header, deliberate header corruption and malformed lengths. Check bounds before indexing or allocating from recovered fields. Do not expand the current 250-byte bound without tracing all length/CRC/buffer arithmetic and documenting the supported radio limits.

### 9.5 Payload, FEC, whitening and CRC

Use the independently verified symbol stream to test each transform separately before testing end-to-end IQ. Verify block dimensions, codeword bit order, correction limits, padding removal, whitening seed/order/reset, payload length and CRC coverage/byte order.

Do not manufacture a mask, reverse bytes selectively or skip failing bytes just to match one capture. One payload pattern is insufficient to validate whitening or CRC. Check repeated patterns and length boundaries with fixed external expected bytes.

Preserve these distinct results: complete bytes with valid CRC, complete bytes with failed CRC, complete bytes with CRC absent, and incomplete bytes with CRC not checked. CRC-absent positive fixtures require an independent exact-byte oracle.

### 9.6 LDRO and header modes

LDRO must be an explicit, provenance-bearing input or a bounded documented inference supported by the selected radio behavior. Audit its effects on symbol mapping/interleaving and capacity calculations. Never treat the lack of an exposed UART setting as proof of LDRO off or implicit-header mode.

Explicit mode is the preferred first implementation target when confirmed. Implicit mode needs external payload length, CR and CRC-presence information; keep unvalidated guesses out of the successful-decode path. Treat SF5/SF6 as separate compatibility work, not ordinary extrapolations of SF7 tests.

### 9.7 Candidate iteration and selection

The current per-hypothesis decoder returns a single candidate, and the observation layer returns immediately on an SX-reference valid header. A false early candidate or partial header can hide later data or another decoder result.

For the first controlled fixtures, deliberately capture one complete packet per analysis window. For general live acceptance, implement bounded packet iteration with forward progress, tested end offsets and overlap handling. Preserve competing attempts for diagnostics and prefer evidence-supported results using an explicit rule. Keep internal/nonstandard results separately labeled.

Deduplicate only when candidates refer to the same physical sample interval with compatible evidence. Equal payload bytes or matching frequency alone are not proof of duplicate reception. Measure packet-level detection once per expected physical event, not once per SF/BW row.

## 10. Regression corpus and validation matrix

The following are project acceptance targets, not standards-mandated performance guarantees. Freeze them before examining the withheld set. If they are impractical, document a revised scope and reason before claiming completion.

### 10.1 First-mode corpus

Collect at least 30 distinct clean, complete, independently byte-verified physical packets for the chosen initial mode, across at least three payload patterns and two acquisition sessions.

- Use 20 for diagnosis/development and hold back at least 10 for acceptance.
- Different crops/resamplings of one packet stay in the same split. They are not new physical packets.
- Record all acquisition attempts, exclusions and unresolved packets. Do not count only packets our decoder already likes.
- Freeze fixture hashes and expected results before the final test run. Do not tune on the held-out failures and continue calling that set held out; create a fresh holdout afterward.

M2A pass: every eligible clean packet in both sets produces exact expected PHY bytes and the expected CRC state, with no incorrect additional validated packets. Any unresolved failure blocks the corresponding first-mode claim.

### 10.2 Expanded mode matrix

| Axis | Required coverage before advertising that support |
|---|---|
| SF/BW | Start with one verified pair; add SF8-12 and BW250/500 only where actually available; list every tested pair |
| Coding rate | Test each advertised CR with independent vectors or hardware captures; unknown/uncontrollable settings remain unverified |
| LDRO | Cover known enabled and disabled cases relevant to advertised modes; otherwise narrow support |
| CRC | Present/valid, corrupted/failed, absent and incomplete/not-checked |
| Payload length | Short, representative, interleaver-block transitions, supported maximum and malformed over-limit |
| Preamble/sync | Measured preamble variants and each advertised sync interpretation |
| Header mode | Explicit first; implicit only with independent known parameters and tests |
| Packet location | Start/middle/end of window, deliberately truncated boundaries, multiple packets and repeated identical payloads |
| Acquisition | At least two sessions per claimed hardware mode; persist actual receiver settings |

For each newly advertised physical mode, require at least 10 clean independently verified physical packets including multiple payload patterns and a later-session check. Report counts; a single passing packet is a smoke test, not a support certification. When the module cannot generate a mode, use a documented independent source or mark the mode unverified.

### 10.3 Controlled impairment tests

Derive deterministic variants from immutable clean fixtures using a separate transformation utility with recorded seed and parameters. Retain original files. Evaluate these independently and then in combinations:

- Arrival shifts covering every integer sample offset within one symbol on a representative fixture; additional fractional-delay cases with a documented interpolation filter.
- Positive/negative CFO: proposed test points 0, 0.1, 0.25, 0.5, 1 and 2 symbol FFT-bin spacings. Convert to Hz using the actual mode. These are characterization points, not a promised operating range.
- Sample-rate error: proposed 0, plus/minus 10 and 20 ppm; record the interpolation model and its limitations.
- Seeded added noise over a sweep of explicitly defined SNR values. State signal/noise power windows and estimator; do not compare unlike SNR definitions.
- Gain scaling, clipping, interferers and deliberate missing samples, kept separately labeled.

Report exact-byte success/total versus impairment and compare with the reference on the same IQ. Failures outside the declared envelope should degrade to a failed/partial result rather than incorrect validated bytes. Do not require success at arbitrary severe noise/clipping levels or rewrite expectations to match them.

### 10.4 Negative and malformed-input corpus

Include at least 1,000 deterministic no-packet software windows spanning silence, seeded noise, tones and non-LoRa signals, plus recorded transmitter-off RF backgrounds from the test setup. Report total windows, total duration, seeds and any uncertainty about ambient traffic.

Require no falsely validated standard packets in the labeled negative set. Preamble-only false candidates are tracked separately. A zero count in a finite corpus is not proof of zero false-positive probability.

Also test truncated files, wrong checksum/size/version, NaN/Inf IQ, incompatible rates, invalid SF/CR, integer overflow lengths, header corruption, payload corruption, negative/overflowing indices, multiple candidates and cancellation/shutdown behavior. Run memory/undefined-behavior checks on the decoder and fixture loader; do not rely solely on GUI execution.

## 11. Build a fixture assertion runner

Proposed new targets/files, to be implemented during this milestone:

- `tests/test_lora_interop.cpp`: load independent fixture expectations and assert bytes/fields/statuses.
- A fixture-validation script: schema, hashes, provenance, split and quality checks.
- Machine-readable replay/diagnostic output with stable schema and decoder/source revision.
- A result report generator that lists every fixture, expected packet count, matches, misses, extra results and exclusions.

The current `lora_replay` exit code is not an acceptance criterion: it returns 0 when a recording loads and yields no packets. The new assertion runner must return nonzero for missing required fixtures, wrong bytes, missing expected packets, unexpected validated packets or incorrect integrity states. Tests must actually run in Release builds; avoid relying on disabled C `assert` checks for acceptance.

Compare packet boundaries within documented tolerances and bytes exactly. A header-only result is never a full-payload pass. Never skip a failed fixture because our decoder did not detect it.

For sanitizer validation, use an isolated build directory, select the affected targets, and save command/output. Example once the new target exists:

```sh
cmake -S . -B build/lora-m2-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/lora-m2-sanitize --target test_lora_observation test_lora_interop -j2
```

`test_lora_interop` does not exist yet. Define and document its fixture-path invocation when implemented; do not fabricate a runnable command now. An unavailable sanitizer/toolchain must be reported as an unperformed check, not a pass.

## 12. Integrate verified behavior into live scanning and GUI

Use one shared receive-analysis implementation for live and replay paths. Keep transport, analysis, GUI formatting and persistent identity updates separate. Preserve all earlier integrity states.

Acceptance checks:

1. A saved live buffer and replay of that exact buffer produce the same normalized packet outcomes and bytes. Ignore only explicitly nondeterministic presentation fields such as wall-clock display time.
2. Unsupported sample rates/modes and overflow/short captures have visible explanations; they must not look like a clean scan with no traffic.
3. The GUI shows the first failed stage, decoder provenance and verified-mode scope. Do not change the label to universally "standards-compliant" after one configuration passes.
4. CRC-failed/absent packets never appear as authenticated; MIC/application semantics remain unimplemented here.
5. Replay never writes emitter/master/fingerprint records. Validate in an isolated storage environment and compare state before/after replay.
6. Live fingerprint behavior and existing LoRa master tests remain intact. If candidate iteration changes the number of fingerprint observations, test and document deduplication to prevent inflated identities/counts.
7. Replay stays responsive, bounded and independent of SDR ownership. Measure wall time and peak memory on short and maximum-supported fixture sizes; declare acceptable budgets before performance acceptance. Add cancellation or narrower limits if the UI otherwise waits excessively.
8. Verify normal GUI tables, narrow windows, long error messages, empty results and multiple packets. Save screenshots of representative partial and successful outcomes.

The current shared SDR capture wrapper also serves Wi-Fi/bench paths. If changed, run affected transport/build tests and verify that the existing callers retain their contract. Do not refactor Wi-Fi algorithms as part of a LoRa repair.

## 13. Live acceptance run

After offline M2A passes, repeat a controlled hardware run with the recorded working transmitter:

1. Save current settings and establish receiver ownership.
2. Capture at least 20 fresh transmission opportunities over a complete receive window or explicitly synchronized windows. Avoid retuning during the test.
3. Independently establish which physical packets were fully captured. Separate UART send attempts, acknowledged requests, packets actually observed and packets decoded.
4. Save IQ, UART logs and live results. Replay the exact IQ and compare with the live normalized outcomes and independent oracle.
5. Report misses, truncated windows, transport loss, CRC failures and extra candidates separately. Restore any deliberately changed settings and verify readback using the matching supported API.

Use clear denominators:

- **Capture coverage:** complete independently observed packets in recorded windows / independently established transmitted packets, only when that transmitted count is known.
- **Decoder success:** exact-byte correct packets / eligible independently verified complete captured packets.
- **False validated results:** unexpected validated packets per labeled negative window and per recorded duration.

A GUI that scans intermittently cannot establish end-to-end packet error rate merely by dividing rows by UART send attempts. If the true transmit count is unknown, state that and report capture-conditioned decoder success instead.

## 14. Failure triage and stop conditions

| Symptom | First investigation | Avoid |
|---|---|---|
| No energy at receiver | Working transmitter, frequency, antenna/port, actual samples and ownership | Editing whitening/FEC |
| Energy but no preamble | Rate/BW hypothesis, clipping, timing, modulation mismatch | Lowering thresholds until random matches appear |
| Preamble but bad sync/SFD | CFO/STO, symbol offset, sync mapping and preamble length | Assuming private/public sync from module name |
| Stable symbols but bad header | First-block mapping, bit order, FEC/checksum and actual header mode | Treating encryption as the cause |
| Valid header but truncated payload | Complete airtime/window and length interpretation | Marking header-only as decoded |
| Complete payload but bad CRC | Symbol errors, FEC/whitening, payload boundaries and CRC convention | Ignoring CRC or editing expected bytes |
| Reference also fails | Reference configuration, capture quality, unsupported mode or unknown framing | Using our output as ground truth |
| Replay/live differ | Resampling/configuration parity, offsets, candidate selection and state | Tuning separate implementations independently |
| Tests pass but no new hardware result | Test independence and capture provenance | Claiming interoperability from self-roundtrips |

Stop promotion of a correction if a clean baseline fixture regresses, expected data was derived from the implementation under test, the working transmitter configuration changed without a new session record, or a mode's parameters cannot be established. Continue useful diagnostics and other verified modes; report the blocked claim precisely.

If a source change must be reverted, revert only that isolated change after inspecting the diff. Do not use destructive workspace resets to recover a test failure. Keep the baseline fixture corpus immutable throughout.

## 15. Delivery checklist and evidence report

Produce `docs/LORA_M2_RESULTS.md` containing:

- Exact tested source revision plus working-tree/source hashes, tools and reference commits.
- Current working transmitter/receiver setup and any changes made during the milestone.
- Fixture inventory with independent expectations, hashes, split and exclusions.
- Each proven defect, its first divergent stage and the minimal correction.
- Before/after traces and exact-byte results; clean and impaired results separated.
- Per-mode support table: verified, failed, unsupported or not tested.
- Negative-test counts/duration, sanitizer results, runtime/memory measurements and GUI/live-replay comparison.
- Remaining uncertainty and a reproducible command sequence to rerun the acceptance suite.

Completion checklist:

- [ ] Working transmission setup recorded without assuming the previous failure persists.
- [ ] Baseline source snapshot and existing regression results saved.
- [ ] Complete clean captures, UART evidence and independent bytes obtained.
- [ ] Diagnostic stage results implemented with bounded trace export.
- [ ] First demonstrated PHY failures corrected with independent regression cases.
- [ ] M2A development and held-out sets pass exact-byte and integrity assertions.
- [ ] Every advertised additional mode meets the declared M2B evidence requirements.
- [ ] Negative/malformed inputs, timing/CFO/noise characterization and memory checks completed.
- [ ] Live/replay equality, GUI truthfulness and identity-storage isolation verified.
- [ ] Fresh live acceptance run completed and denominators reported honestly.
- [ ] Results/support matrix and HANDOVER updated; unresolved modes remain visibly unverified.

## 16. Recommended execution order for the next session

1. Read this file and current diffs; record the user's working transmission fix.
2. Preserve code/transmitter settings and run the existing software baseline.
3. Obtain one complete clean real capture and a transmitter-off capture.
4. Decode the clean capture independently and preserve exact expected bytes.
5. Add diagnostics and locate the first failing stage in our decoder.
6. Fix that stage with a failing-before/passing-after independent test.
7. Complete the first-mode development/holdout corpus and M2A gate.
8. Expand the verified mode/impairment matrix, then complete GUI/live acceptance.

Do not start by sweeping every radio setting or rewriting the entire PHY. The first concrete deliverable is one immutable real capture with trusted expected bytes and a reproducible explanation of where our existing decoder diverges.
