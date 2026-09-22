# SX1262 baseline and LoRa monitoring improvements

## Hardware findings — 22 September 2026

The connected CP2102 UART bridge initially denied access at the OS level. After the user granted access, a read at 115200 baud produced an ESP32-S3 startup banner identifying **Heltec WiFi LoRa 32 V3 (SX1262)**. The firmware reported **888.0 MHz, SF7, BW125**, automatic numbered transmissions and a serial-text transmit interface. Startup output was observed while opening serial, so serial inspection should not be assumed incapable of disturbing the board's reset state, even though DTR/RTS were set false and no reset command was sent.

No firmware flash, configuration write or serial transmit command was issued. The X310 passively received the firmware's existing automatic transmissions. Frequency 888 MHz is an observed receive setting, not a regional channel recommendation or a new application scan default.

X310 configuration: RX2, channel 0, gain 5 dB, requested/actual sample rate 500000 samples/s, 12-second capture, 6,000,000 samples, **no overflow**. UHD printed a send-buffer warning but capture completed. Full local recording:

`data/lora_m2/sx1262-validation/capture-1790061126454567560-0`

The production decoder recovered:

| Payload | CRC | SF/BW | Header-derived coding rate | Sync observation | IQ polarity |
|---|---|---|---|---|---|
| `Auto 888MHz #12` | Valid | SF7 / 125 kHz | 4/7 | 0x12 | Normal |
| `Auto 888MHz #13` | Valid | SF7 / 125 kHz | 4/7 | 0x12 | Normal |
| `Auto 888MHz #14` | Valid | SF7 / 125 kHz | 4/7 | 0x12 | Normal |

Each payload contains 15 bytes. The coding rate comes from the recovered header; it was not separately read back from the transmitter. The firmware banner independently supports the frequency/SF/BW and the automatic-message format. The specific #12–14 byte strings are captured regression expectations, not a separate serial log of those exact three transmissions. This is a successful cross-hardware baseline, not a measured packet-delivery rate.

Full-capture observations:

| Payload suffix | Coarse capture offset | Preamble peak ratio | SFD peak ratio | CFO estimate | FEC mismatched codewords |
|---|---:|---:|---:|---:|---:|
| #12 | 0.742400 s | 0.981346 | 0.982406 | −122.070 Hz | 0 |
| #13 | 5.751808 s | 0.982316 | 0.982416 | −122.070 Hz | 0 |
| #14 | 10.760192 s | 0.977347 | 0.980995 | −168.573 Hz | 3 |

CFO is a receiver-relative estimate, not a calibrated transmitter frequency. Mismatched codewords are not a corrected-bit count. Ratios measure dechirped coherence, not probabilities, SNR or absolute RF power. A CRC can pass despite nonzero FEC mismatches, as #14 demonstrates.

## Receiver and GUI changes

- **Both IQ polarities:** the receive-only decoder tests the captured IQ and its conjugate. Polarity is recorded on each accepted header. CFO/drift signs are converted back into the original capture convention. Both sets of observations are retained in time order; they are not silently deduplicated.
- **Failure-stage evidence:** unsuccessful hypotheses now report preamble, SFD, alignment and rejected-header search counts in the outcome tooltip. These aggregate normal/inverted search attempts, not distinct packet counts. They distinguish “no SFD/alignment” from “aligned but no header accepted.” SF5/6 still use the older detection-only fallback.
- **Seven added measurements:** IQ polarity, aligned preamble peak, weaker of the two SFD peaks, fractional CFO in Hz, preamble drift in Hz/symbol, coarse capture offset and FEC mismatches. Existing columns, raw hex/ASCII and fingerprint information are retained. Scroll horizontally to see the new columns.
- **LoRaWAN structure column:** a separate structural inspection result, explained below. It never replaces raw bytes or physical integrity status.
- **Retained-observation summary:** header/CRC/partial/failure counts summarize the current bounded table. They are explicitly not unique-device counts or delivery rates.
- **Additional frequency map:** a LoRa frequency lock outside 863–868 MHz adds a separate receive view centered on the lock, retaining the original map. It derives the range from the user input; 888 MHz is not hardcoded.
- **Replay parity:** the offline tool prints the new measurements and any LoRaWAN candidate fields. Replay remains separate from persistent identity records.

The Active emitters spectral “LoRa-like” label remains a bandwidth heuristic. Packet header/CRC outcomes provide stronger, separate evidence. No manufacturer identity is inferred from polarity, sync word, payload text or CRC. Existing RF fingerprint gates and master records remain intact.

## First LoRaWAN inspection layer

`src/lorawan_inspect.hpp` checks supported MHDR/layout constraints and bounds before extracting information. It follows the visible layouts in the [LoRa Alliance TS001-1.0.4 specification](https://lora-alliance.org/wp-content/uploads/2021/11/LoRaWAN-Link-Layer-Specification-v1.0.4.pdf), sections 4 and 6:

- Data-frame candidates show direction/type, DevAddr, transmitted FCnt16, FCtrl, ADR/ACK bits, raw FOpts, optional FPort, opaque FRMPayload and MIC bytes.
- Join-request candidates show JoinEUI/AppEUI, DevEUI, DevNonce and MIC bytes.
- Join-accept candidates keep their encrypted bodies opaque.

Every result says **candidate / MIC not verified**. No session version is inferred; no complete frame counter is reconstructed. Rejoin/proprietary types are not parsed. MAC-command semantics, keys, authentication and decryption are not implemented. Structurally plausible unrelated data may still match; candidate fields must not become persistent identities.

Only complete production-PHY payloads with a valid CRC or an absent CRC are inspected. CRC-absent bytes receive an additional unverified-bytes notice. Failed-CRC and laboratory-codec bytes are not offered to the parser. These are framing checks, not full LoRaWAN protocol validation.

The board's current `Auto 888MHz ...` packets remain **unclassified** at the LoRaWAN layer. No LoRaWAN traffic was claimed or generated during this test.

## Reproducible fixtures and verification

Three unmodified 0.16-second IQ crops are included under `tests/fixtures/lora_sx1262/`, with manifests, capture checksums and `expected.json`. Together they are approximately 1.9 MB. `.gitignore` explicitly permits these small IQ fixtures; the full local recording remains ignored. The ordinary receiver test now requires all three, so a clean checkout receives actual SX1262 regression coverage.

```sh
cmake -S . -B build
cmake --build build --target rf_monitor_gui lora_replay test_lora_receiver test_lora_observation test_lora_gui -j4
ctest --test-dir build --output-on-failure
build/test_lora_receiver --hardware-fixtures
build/lora_replay tests/fixtures/lora_sx1262/capture-1790061256825496286-0
build/test_lora_gui /tmp/lora-gui.ppm tests/fixtures/lora_sx1262/capture-1790061256825496286-0 --scroll-right
```

Verified in this session:

- 101 symbol vectors and 96 normal/inverted synchronization cases across SF7–12, CR1–4 and both LDRO settings.
- Byte-exact CRC-valid decoding of the three included SX1262 recordings.
- Regression against all six older local Tarang recordings using `--hardware-fixtures` (those older recordings remain locally ignored).
- Multiple-packet, corruption, truncation, noise, nonfinite-symbol and rejected-header diagnostic checks.
- LoRaWAN layout/endian/bounds/rejection and opaque-join-accept unit checks. These are software-only, not LoRaWAN hardware validation.
- Observation/capture tests and offscreen GUI rendering with both a real SX1262 row and explicitly synthetic LoRaWAN display data.

## Remaining work and limitations

1. The board currently exposes text transmission, not a confirmed configuration API. Do not send guessed radio commands. A wider hardware parameter matrix requires documented controls or agreed test firmware.
2. Inverted IQ has synthetic regression coverage but no inverted-IQ hardware capture yet. Dual-polarity processing adds CPU work and a conjugated-buffer allocation per SF/BW hypothesis. The existing maximum of 32 packets now applies per polarity; table counts are bounded observations, not totals.
3. The scanner still uses finite windows and processing/retune gaps. This upgrade does not claim continuous lossless capture or a measured sensitivity improvement.
4. Hypotheses across SF/BW/polarity are retained, not grouped into unique RF events. False header acceptance remains possible with the short header checksum. A partial accepted header can also cause the rest of that hypothesis's capture to be skipped by the existing search logic.
5. PHY support remains explicit-header SF7–12 at the app's 125/250/500 kHz hypotheses, with existing boxcar decimation. Implicit profiles, other bandwidths, stronger channel filtering and collision separation remain work.
6. Existing preamble/FFT/correlation thresholds, six-symbol drift fit, conventional eight-bit sync mapping and fingerprint tolerances remain fixed assumptions. No new vendor payload/address whitelist was introduced.
7. Candidate LoRaWAN fields need a controlled node/gateway/server test next. MIC verification requires keys, the correct version/session context and complete counters; it cannot follow from physical CRC alone.

To use the new hardware now, restart the rebuilt GUI, select LoRa, enable frequency lock and enter the board's observed **888.0000 MHz** receive frequency. Existing information remains available; the additional measurements and LoRaWAN structure column are on the right of the packet table. No other GUI process was stopped by this work, and no Wi-Fi code or controls were changed.
