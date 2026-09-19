# LoRa receive-only PHY upgrade

## Result and scope

Production live monitoring and offline replay now share `src/lora_receiver.cpp` through `lora_observation.cpp`. This is a receive-only implementation of commercial LoRa explicit-header conventions, not the project's own transmit/receive format. The experimental `lora_phy_std.cpp` and internal codec remain available only through **Legacy laboratory codecs (nonstandard)**. That option is off by default; production never falls back to them. The pre-existing, uncommitted changes to `lora_phy_std.cpp` were preserved.

Wi-Fi processing, configuration values and GUI controls were not changed. Shared files have LoRa-specific changes only.

Seven hardware recordings were successfully decoded: six existing cropped recordings, plus one fresh TarangMini/X310 recording made during this upgrade. This establishes interoperability with the tested TarangNet firmware/mode, not universal compatibility with all LoRa hardware.

## What is derived from received IQ

1. Try the configured bandwidth/SF hypotheses, using the actual capture sample rate to reject incompatible decimation ratios.
2. Find a stable upchirp preamble, then the downchirp SFD. Use both chirp slopes to resolve symbol timing and frequency offset; choose the timing ambiguity by correlation with the preamble and both full SFD chirps.
3. Estimate local frequency drift from six preamble symbols. Track the reduced symbol lattice for the header and LDRO payload. Ordinary payload symbols use the fitted linear drift correction.
4. Recover and checksum the explicit PHY header: payload length, coding rate and presence of payload CRC. The first block always uses SF−2 bits and 4/8 coding, independently of payload LDRO.
5. Evaluate both payload LDRO hypotheses. Payload CRC selects a verified result where possible. Without a valid CRC the selected hypothesis remains explicitly ambiguous (`On?`/`Off?` in the GUI).
6. Deinterleave, FEC-decode, assemble nibbles and dewhiten payload bytes. CRC bytes are not dewhitened. Validate the physical payload CRC separately from the header checksum.
7. Continue through the capture to recover subsequent packets, rather than stopping at the first header.

No Tarang address, payload prefix, expected text, payload length, frequency, coding rate, fixed CFO or sync-word whitelist participates in the production decode. The new test string and its expected length were never passed into the receiver.

The two sync bins are measured. A conventional eight-bit sync observation is displayed only when both bins lie sufficiently close to the nibble mapping; otherwise the raw observations appear in the details. Other sync values do not block header decoding. Sync words do not identify a device or prove LoRaWAN.

## GUI and capture changes

- The table shows the production decoder, header validity, CR, header length, observed sync, LDRO hypothesis, CRC outcome and payload hex/ASCII.
- Hover the outcome/payload for sample offset, drift estimate and ambiguity details.
- Header-only, CRC-absent, CRC-failed and CRC-valid outcomes remain distinct. An absent CRC is not a failure or an integrity guarantee.
- The default LoRa capture window is six seconds. **LoRa capture seconds (1–30)** lets the user select a window from the observed packet duration. It is additionally bounded by the existing 32-million-sample capture limit.
- Independent RF fingerprint collection still runs through its existing gates and updates the master registry even when the new PHY succeeds. Its older burst alignment is not attached to a newly decoded packet as though they were the same measurement. Physical CRC is never promoted to a device identity.
- Offline replay never writes persistent emitter records.
- The running old GUI process was not stopped. Restart the rebuilt application to load the new code. For this board, use frequency lock **865.9000 MHz**, as read back during this session; this is not added as a new default scan frequency.

## Evidence and provenance

Byte/symbol conventions were checked against [LoRaPHY](https://github.com/jkadbear/LoRaPHY), pinned to commit `4fddd9a7b47682781c663608bfc4f196e4bf656d`. Its MIT notice is retained at `third_party/loraphy_reference/LICENSE`.

The published independent SF12/CR4 vector decodes to bytes `01 02 03 04 05 06 07 08 09` and CRC bytes `ba 2e`. This caught a header-checksum matrix transcription error that the CR1 hardware recordings did not exercise; the error was fixed before integration was accepted.

`tools/generate_lora_reference_vectors.py` generates supplemental fixed vectors in `tests/fixtures/lora_standard_symbols.json`. These use an independently written Python transmitter based on the same reference conventions; they are not a substitute for hardware or an independently executed MATLAB/GNU Radio receiver. Tests never use the application's legacy modulator as the production interoperability oracle.

### Fresh hardware test

Read-back before transmit:

- Firmware: `TarangNet_TN_LW_STD_WL_v0_0_6`.
- Frequency: `865900000 Hz`.
- Data-rate index: `0x00`, documented as SF12/BW125.
- Connection mode: root.
- X310 RX2, channel 0, 500000 samples/s, gain 5 dB.
- One test transmission per attempt: `RX_UPGRADE_19SEP_A7`.

First attempt: the module acknowledged TX, but UHD timed out during a management transaction and returned zero samples. This is **not** counted as a successful reception. No system network settings were changed and no other application's process was killed.

One retry: 10,000,000 samples over 20 seconds, no overflow. Replay recovered SF12/BW125, CR4/5, explicit length **43**, LDRO on, sync observation `0x12`, and a valid physical payload CRC. Exact payload:

```text
13ffffffffffffffffffff3cc1f6050005d8e000000c000052585f555047524144455f31395345505f4137
```

The final 19 bytes are the transmitted test string. Rate and frequency were read back unchanged after both attempts; no configuration/flash writes were needed.

The retained cropped recording and receive/replay logs are under `data/lora_m2/receiver-upgrade-validation/`. They are local evidence ignored by Git, as are the six older hardware fixtures. The original full retry recording remains under `/tmp/lora-upgrade/live-captures/` and is temporary. The portable software vector fixture is not ignored.

## Reproducible verification

From the repository root:

```sh
cmake -S . -B build
cmake --build build --target rf_monitor_gui lora_replay test_lora_receiver test_lora_observation test_lora_gui band_smoke_test -j4
ctest --test-dir build --output-on-failure
build/test_lora_observation
build/test_lora_receiver --hardware-fixtures
build/test_lora_gui /tmp/lora-gui.ppm PATH_TO_SAVED_HARDWARE_CAPTURE
build/lora_replay PATH_TO_SAVED_HARDWARE_CAPTURE
```

`test_lora_receiver` without arguments is portable and is registered with CTest. It verifies:

- 101 fixed symbol vectors: the published independent example, SF7–12 × CR1–4 × both LDRO settings × CRC on/off, and lengths 0/1/2/255.
- Forty-eight full IQ synchronization cases with fractional CFO, arbitrary leading samples and sync `0xAB`.
- Multiple packets in one capture, short/long preambles, sync `0x12`/`0x34`, negative CFO, truncation, missing samples, silence, random noise, nonfinite symbols and unsupported SF.
- The optional `--hardware-fixtures` run requires all six pre-existing cropped recordings and checks the **entire** expected payload including each recorded counter, plus valid CRC and header parameters. A clean checkout without those ignored recordings should run the portable test; it cannot claim hardware regression coverage.

The existing observation/capture tests now explicitly select laboratory mode for their old synthetic waveforms. They continue checking capture-format validation and integrity states and additionally check that production does not silently invoke a legacy decoder. The GUI test renders the real new widgets with integrity-state fixtures and, optionally, a CRC-valid saved hardware capture. That was run using the fresh recording and visually inspected.

## Remaining limits and fixed assumptions

| Area | Current boundary / consequence |
|---|---|
| PHY families | Explicit-header conventional LoRa, SF7–12. SF5/6 may produce detection evidence through the old detector but are not decoded by the new receiver. LR-FHSS, FSK and other formats are unsupported. |
| Implicit headers | There is no transmitted explicit header to recover. A future profile must supply length/CR/CRC/LDRO settings; blind arbitrary guessing is not implemented. |
| Bandwidth/front end | App hypotheses are still 125/250/500 kHz. Integer boxcar decimation remains; it is not a selective channelizer and can admit adjacent-channel interference. Other bandwidths/resampling require work. |
| Frequency coverage | Existing four scan frequencies remain, including the old 866.9 MHz lab entry. Lock to actual device read-back for other channels. Automatic serial-to-GUI frequency discovery is not implemented. |
| IQ polarity | Normal-IQ path only. Inverted-IQ traffic, including typical LoRaWAN downlinks, needs explicit polarity hypotheses and validation. |
| Preamble/synchronization | Defaults: at least 6 stable symbols, search at most 256 symbols, peak ratio >0.5, adjacent coarse bins within 1.5. SFD structure is two downchirps plus a quarter symbol. These are supported-mode assumptions, not learned universal thresholds. |
| DSP bounds | Fine FFT uses 8× zero padding; fit uses 6 preamble symbols; extraction capped at 1100 data symbols and 32 packets per SF/BW hypothesis. Options expose preamble/packet/peak limits to callers, not GUI controls. |
| Sync display | Conventional nibble×8 mapping with <1.5-bin tolerance. Nonmatching observations are retained, not rejected. This is not a universal SX126x 16-bit sync-register decoder. |
| Drift/noise | The reduced-lattice tracker assumes less than two bins of intersymbol drift. Ordinary payload drift is linear. Weak signals, collisions, clipping, interference or stronger drift may fail; no sensitivity/BER claim is made. |
| Capture scheduling | Finite windows and processing/retune gaps remain. Six seconds is a default, not lossless continuous monitoring; a packet crossing a boundary can still be lost. Long windows increase scan latency. |
| Integrity | Header checksum is only five bits; false headers are possible. CRC validates recovered physical bytes, not authenticity, vendor, encryption status or identity. CRC-off bytes and failed bytes stay unverified. |
| Higher layers | Raw PHY payload is preserved. This upgrade does not parse/authenticate LoRaWAN MAC fields or decode undocumented TarangNet payload framing. Encrypted application data remains bytes; no decryption or vendor-identification claim. |
| Identity | Existing RF fingerprint matching still has its previous calibration/alignment and hypothesis-duplication limitations. CRC success does not establish a new identity. Cross-SF/BW rows are not deduplicated unique packet counts. |
| Hardware breadth | Real validation is TarangMini at SF12/BW125/CR4/5. Other SF/CR/LDRO combinations have software coverage, not cross-vendor or all-rate RF evidence. |
| TX tooling | Existing TarangNet firmware whitelist and command/data-rate tables remain device-specific. The `0x02` manual ambiguity and lack of a matching LoRaWAN API manual remain. Production RX does not depend on these tables. |
| Host/radio | One UHD management timeout occurred before the successful retry. Network send-buffer warnings remain; no system-level tuning was attempted. |

Recommended next validation is a second manufacturer's ordinary explicit-header transmission with independently recorded settings/payload, then inverted-IQ support and a strictly separate LoRaWAN MAC parser. Continuous reception/channelization should follow if missed packets between finite windows are the main operational problem.
