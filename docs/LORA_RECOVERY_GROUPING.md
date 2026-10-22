# LoRa priority 1 — decoder recovery and reception grouping

Implemented against commit `818e814` on 15 October 2026. This work is limited to LoRa recovery, conservative hypothesis grouping, their GUI/replay presentation and validation. It adds no model, channelizer, streaming acquisition mode, new PHY family or attack rule. RF hardware was not operated and recordings/datasets were not modified.

## Decoder recovery

Previously, a header-valid but incomplete candidate assigned its end to the capture end and advanced the search there. A false or damaged long candidate could therefore hide a later good packet. Complete but CRC-failed payloads could similarly reserve an unverified span.

The receiver now skips the complete declared span only after payload CRC passes. Partial, failed-CRC and CRC-absent candidates remain as original evidence, but searching resumes after their eight-symbol header block. This avoids repeatedly accepting the same SFD throughout a long preamble while allowing later packets to be found. Both normal and inverted IQ use the same policy. It is not collision separation and does not guarantee recovery of a packet whose preamble overlaps that header block or lacks sufficient usable samples.

The existing maximum of 32 accepted candidates per polarity/SF/BW remains. Diagnostics record unverified recovery resumes and whether the packet limit prevented further search. Observations propagate the actual per-polarity limit rather than assuming that 32 combined normal/inverted results necessarily reached a limit. No preamble threshold, CRC algorithm, coding convention or sync whitelist changed.

A regression was run before the fix and failed on a truncated candidate followed by a valid packet. It passes afterward. Tests cover both polarities, boundaries with and without a zero-sample gap, damaged but complete payloads, CRC-absent traffic followed by CRC-valid traffic, preservation of unverified evidence, and explicit packet-cap reporting. Packet association in these tests uses the aligned data anchor; the coarse preamble-start estimate can legitimately precede the actual waveform start within its stated uncertainty.

## Grouping rules and limits

Grouping is performed on one capture/context at a time through `group_lora_receptions`. It returns indices into the unchanged source rows and a representative index. `annotate_lora_receptions` adds capture-scoped IDs and source-hypothesis counts; it does not remove rows or rewrite measurements, bytes or integrity flags.

Two candidates group only when all of the following agree:

- Production decoder, SF, bandwidth, receive centre, coding rate and declared payload length.
- Payload completeness, integrity state and complete retained payload representation. Partial, failed, absent-CRC and valid-CRC evidence are never promoted into each other.
- Compatible sample coordinates at the actual input sample rate. With aligned SFD/data anchors, separation must be at most a quarter symbol, further limited by both stated start uncertainties; start/end differences must also stay within one symbol.
- With no aligned anchor on either old row, start/end spans must match exactly. A known anchor is not guessed equivalent to an unknown one.
- Different polarity interpretations group only for the same complete CRC-valid bytes. Unverified opposite-polarity interpretations stay separate.

Matching always uses the first group member as the fixed anchor, so nearby candidates cannot chain into a group spanning later receptions. A member with fewer reported FEC-disagreeing codewords may become the representative, but all members remain available. That count is not a corrected-bit count or proof of identity.

Different SF/BW interpretations, conflicting payloads, incompatible headers and repeated packets at later sample positions stay separate. Overlap alone is insufficient. These are conservative **reception-candidate groups**, not verified physical transmissions, unique devices or delivery-rate measurements. Very closely overlapping identical emissions cannot be reliably distinguished by this policy.

Comparison work is capped at 1,048,576 group probes per call. At the limit, remaining candidates stay separate and the grouping result/row metadata exposes the limit. No original row is dropped to satisfy that grouping budget. The normal decoder's existing capture and packet limits bound live input sizes separately.

## Integration and compatibility

- The production LoRa scanner collects the same raw display rows, annotates them using run/session/capture scope, then publishes the bounded last-200-row GUI tail. Its fingerprint extraction, gates, master storage, radio ownership, scan frequencies, sample rates and capture settings are unchanged.
- Offline capture analysis uses the same grouping function. GUI replay and `lora_replay` label their file context. No offline result is inserted into live identities.
- The PHY table shows retained group counts, ungrouped rows, an optional Reception group column, and group/anchor detail in the selected snapshot. Raw hypothesis counts and existing columns remain available. A source-group size can exceed its currently retained members; the inspector labels that distinction.
- The LoRa security batch uses one representative per compatible group and records the raw hypothesis count plus duplicate-hypothesis count. Capture coverage/overflow/loss exclusions remain unchanged. The consumer's exact-repeat safeguard now includes coding rate, polarity, decoder and payload-completeness context, preserving competing interpretations.
- Capture IQ manifests and security-event JSON remain version 1/2 compatible as before; no wire field was removed or required. Existing event files remain readable. Re-decoding IQ can legitimately recover more packets and group compatible aliases differently after this update. Replaying an already saved event file preserves its recorded evidence rather than retroactively recovering IQ.
- Wi-Fi, Drones, cyclostationary analysis and ML source files were not changed. No running application was restarted; the updated executable is in `build-gui-revamp/rf_monitor_gui`.

## Lab-only padding safety correction

AddressSanitizer found an existing one-byte over-read in `lora_phy_std.cpp` while the observation test generated an 80-byte CRC-absent laboratory waveform. The final FEC block rounded beyond the actual payload/CRC vector. It now encodes zero-valued padding nibbles instead of reading outside that vector. Added empty/short/79/80/81-byte, CRC-on/off generation cases run under the sanitizer.

This is a bounds correction in the nonstandard laboratory/test generator. It does not add transmission, change the production explicit-PHY decoding conventions, or turn the laboratory generator into an interoperability oracle. Production receiver tests still use fixed independent physical-symbol vectors and recorded hardware IQ.

## Verification

Commands from the repository root:

```bash
cmake --build build-gui-revamp -j 4
ctest --test-dir build-gui-revamp --output-on-failure -j 1
build-gui-revamp/test_lora_receiver --hardware-fixtures
build-gui-revamp/lora_replay tests/fixtures/lora_sx1262/capture-1790061256825496286-0
LIBGL_ALWAYS_SOFTWARE=1 build-gui-revamp/test_lora_gui /tmp/lora-priority1.ppm \
  tests/fixtures/lora_sx1262/capture-1790061256825496286-0 --scroll-right
```

The existing five LoRa CTests passed before edits. Expanded checks cover:

- 101 symbol vectors and 96 normal/inverted synchronization cases, plus eight damaged/CRC-absent recovery scenarios and packet-cap checks.
- Byte-exact CRC-valid decoding of three included SX1262 fixtures and all six local Tarang fixtures.
- Positive anchored duplicate grouping; negative repeated-payload, conflicting-byte, SF/BW/CR, polarity, CRC-class, partial, receive-centre, malformed-coordinate and missing-anchor cases.
- Non-transitive matching, capture/session ID separation, bounded work on 1,500 conflicting candidates, and preservation of every raw row.
- Grouped security counts and JSON/live replay equality; incompatible coding-rate evidence remains separate in the consumer.
- Production scanner with a fake radio and real recorded SX1262 IQ: group tags are scoped, save still works and saved-IQ replay yields identical security state.
- Actual offscreen GUI renderer, summary text and optional recorded-hardware fixture, with zero ImGui state errors.
- Receiver and observation/grouping tests under AddressSanitizer and UndefinedBehaviorSanitizer. Leak detection was disabled because of the execution environment; leak freedom is not claimed.

The final full serial run passed **49/49 CTests**. Both the full-feature GUI and the GUI with Drones/cyclostationary disabled build successfully. Receiver and observation/grouping sanitizer runs passed with leak checking disabled.

The first full run passed 48/49 tests and hit the already documented intermittent `remote_id_monitor` timeout. An isolated retry and the final complete run passed. The Remote ID test/implementation was not changed to suppress that failure. Logs, the pre-fix failure, hardware regression output and a reviewed synthetic/recorded GUI preview are saved under `docs/lora_priority1_validation/`.

No fresh RF sensitivity, packet-delivery-rate, interference or sustained live-load claim follows from these tests. Filtering/resampling, capture-gap reduction, channelization, implicit/SF5–6 payload decoding and LoRa security rules remain subsequent priorities.
