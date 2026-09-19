# TarangMini hardware assessment and LoRa improvement plan

Assessment date: 2026-09-18. Scope: inspect supplied documentation, query the attached demo board, check receiver availability and existing software tests, and plan improvements. No production decoder, GUI, firmware or radio configuration changes were made.

## Main finding

The currently connected board identifies itself as **ST22LR01 running `TarangConnect_LW_S201_LoRaWAN_CA_v1_0_4`**, not the TarangNet firmware targeted by `tools/tarangnet_api.py` and `tools/tarangmini_sf_bw_sweep.py`.

The supplied TarangNet API manual is explicitly for `TarangNet_TN_STD_WL_v0_0_4`. Its command IDs cannot be used to interpret or configure the installed LoRaWAN firmware. This is confirmed by live responses: the command described as sleep mode in that manual returns the LoRaWAN firmware name, and the command described as wake time returns the hardware name. A successful status byte alone does not establish that a response has the expected meaning.

**Do not run the existing TarangNet sweep against this board.** It writes settings, saves flash and restarts using a different firmware's command map. The first improvement should be firmware identification and a matching protocol adapter, before any configuration or transmit operation.

Real over-the-air packet decoding was **not tested successfully in this session**. The host can ping the presumed X310 address, but UHD discovery fails. Antenna attachment also remained unconfirmed. The user confirmed that only the three supplied PDFs are available; none documents the installed LW-S201 API.

## 1. Sources and how they apply

| Source | Relevant content | Limitation |
|---|---|---|
| [ST22LR01 Datasheet v1p2.pdf](ST22LR01%20Datasheet%20v1p2.pdf) | Module capabilities, electrical interfaces and supported firmware. Page 17 distinguishes LW-S201 LoRaWAN from TN-S101 point-to-point/multipoint. | Does not specify the LW-S201 command map or full radio packet format. |
| [TarangNet STD API Documentation_V2.pdf](TarangNet%20STD%20API%20Documentation_V2.pdf) | TarangNet UART framing, configuration, send/receive operations; rate tables on pages 13-14. | For a different firmware family from the attached module. |
| [TarangMini ST22LR01 Code Loading User Guide.pdf](TarangMini%20ST22LR01%20Code%20Loading%20User%20Guide.pdf) | Demo board layout on page 5; UART bootloader and firmware-loading procedure. | A loading guide, not a LoRaWAN API manual. Firmware replacement was not performed. |

Text was extracted from all three PDFs. The board-layout page and both data-rate table pages were also rendered and visually checked.

The datasheet describes an STM32-based sub-GHz module supporting LoRa/GFSK, the 863-870 MHz range, and separate firmware products. LoRa modulation and LoRaWAN networking are different layers; the same module name does not identify its networking protocol.

For the standardized packet-layer plan, see the [LoRa Alliance LoRaWAN specification v1.0.3](https://resources.lora-alliance.org/getting-started-with-lorawan/lorawan-specification-v1-0-3), particularly frame formats, payload encryption and activation. Firmware name `v1_0_4` is a vendor software version string; it does **not** prove the implemented LoRaWAN specification version.

## 2. Live hardware results

### USB and UART

Two different FTDI adapter identities appeared during the session:

1. Initially `usb-FTDI_FT232R_USB_UART_B002H3TM-if00-port0` at `/dev/ttyUSB0`.
2. Later `usb-FTDI_FT232R_USB_UART_B002HEFW-if00-port0`, also at `/dev/ttyUSB0`.

Both enumerate as FTDI FT232R USB UART, VID:PID `0403:6001`. The change of USB serial number means observations must not silently be attributed to one physical adapter. `/dev/ttyUSB0` alone is not a stable identity. The module EUI and USB adapter serial are distinct identifiers.

The initial adapter produced two zero bytes during a three-second passive listen at 9600 baud. Those bytes are not a valid API response. The first active query attempt failed before opening the disappeared serial path; no bytes were sent on that attempt.

The second adapter answered binary UART requests at **9600 baud, 8N1**. The probe used exclusive serial access, disabled DTR/RTS assertions before opening, sent complete request frames, and allowed approximately 1.2 seconds per response. All requests used the read-shaped frame `2b 05 02 00 00 <command-id>`; no data-send, configuration-write, reset, defaults or flash-save commands were issued. These are documented as reads for TarangNet; the supplied documents do not independently establish all corresponding LW-S201 command semantics.

Selected responses, recorded at 10:08:22-10:08:44 UTC (15:38 IST):

| Command ID | Value/status observed | Interpretation |
|---|---|---|
| `0x0004` | Success, `3cc1f6050005df88` | EUI-shaped identifier; matches the supplied manual's EUI read. Preserve UART byte order pending LW-S201 documentation. |
| `0x0006` | Status `01`, no value | The TarangNet firmware-version read did not succeed. |
| `0x000A` | Success, ASCII `ST22LR01` | Hardware identification returned from an ID that TarangNet calls wake time. |
| `0x000B` | Success, ASCII `TarangConnect_LW_S201_LoRaWAN_CA_v1_0_4` | Strong evidence of installed LoRaWAN firmware; TarangNet calls this ID sleep mode. |
| `0x000F` | Success, one byte `ff` | Cannot interpret as the four-byte frequency value expected by the TarangNet manual. Current frequency remains unknown. |
| `0x0008` | Success, one byte `00` | Do not interpret using the TarangNet SF/BW table until the LW-S201 mapping is known. |

All 19 read-shaped requests and raw responses are preserved in [uart-read-results.json](docs/tarangmini/2026-09-18/uart-read-results.json). Its command labels explicitly identify their TarangNet origin and are **not validated LoRaWAN field names**. Unknown fields have deliberately not been assigned meanings. The probe establishes working UART access and firmware-family mismatch, not a complete trustworthy radio configuration snapshot.

### SDR receiver

- Host interface `enp3s0` had address `192.168.10.1/24`.
- `192.168.10.2` responded to ICMP twice, with approximately 0.6 ms latency and no loss in the final check.
- `/usr/bin/uhd_usrp_probe --args addr=192.168.10.2` failed with `No devices found`.
- Retrying with `--args type=x300,addr=192.168.10.2` also failed. [Probe output](docs/tarangmini/2026-09-18/uhd-probe.txt).
- The probe uses UHD 4.1.0.5; the GUI links to the same system UHD library family.
- A `rf_monitor_gui` process was present, although the inspected UDP socket listing showed no matching connection to the receiver. This does not prove exclusive availability at all times.
- No B210 appeared in the inspected USB device inventory.

ICMP reachability alone does not prove that the responder is a usable X310 or that UHD control/streaming works. Next diagnostics should check device ownership, UHD discovery traffic, host routing/firewall, the actual radio address, and device/FPGA compatibility. Do not upgrade FPGA images as the first troubleshooting step. No IQ capture, receiver-overflow assessment, RF packet count or packet-error-rate measurement was obtained.

## 3. Existing app and tests

The three relevant targets were successfully built using the current build directory and then executed:

| Test | Result | What it establishes |
|---|---|---|
| `test_lora_phy_std` | 9 cases passed | Internal encoder/decoder round trips across selected SF/CR and payload cases. |
| `test_lora_phy` | 13 cases passed | Internal round trips, including padding and light synthetic noise. |
| `test_lora_master` | All checks passed | Existing fingerprint record matching, retention and reload behavior. |

Logs are in [docs/tarangmini/2026-09-18](docs/tarangmini/2026-09-18). These tests do **not** establish Semtech radio interoperability. In particular, a shared encoder/decoder mistake can pass every round trip. Passing SF5/SF6 synthetic cases is not evidence of compatibility with every physical radio's SF5/SF6 modes.

Code review found the following actionable issues or audit targets:

| Location | Observation | Needed improvement |
|---|---|---|
| `tools/tarangnet_api.py` | No firmware-family gate; parser takes the first response without fully checking frame length, expected command ID and packet type. Input flushing can discard unsolicited events. | Validated protocol selection and a buffered UART parser with request/response matching and event handling. |
| `tools/tarangmini_sf_bw_sweep.py` | Writes rate settings, saves flash and restarts according to TarangNet semantics. | Refuse unsupported firmware; snapshot and verify restoration before allowing controlled sweeps. |
| `src/lora_phy_std.cpp`, `src/lora_phy.hpp` | Decoder requires the shared fixed `SYNC_WORD_DEFAULT` value `0x12`; synchronization mapping is shared with the internal encoder. | Make sync handling explicit and validate it against independent captures. Do not assume changing one constant alone fixes interoperability. |
| `src/lora_phy_std.cpp` | `ppm = sf` is used for first-block and payload processing; no explicit LDRO handling is apparent in this path. | Audit reduced-rate first-block mapping, LDRO, interleaving, FEC, whitening and CRC against independent vectors. These are audit candidates, not measured root causes yet. |
| `src/lora_phy_std.cpp` | Preamble/sync path relies on symbol-grid timing and integer CFO bins. | Test arbitrary arrival timing, fractional CFO, SFD alignment and sample-rate error with real IQ. |
| `src/scanner.cpp` | A valid header sets row status to `decoded`, even if payload CRC fails. | Separate detection, header validity and payload integrity. Represent CRC absent, failed, valid and not checked distinctly. |
| `src/lora_phy_std.hpp/.cpp` | Implicit-mode attempts are exploratory. | Require known implicit parameters and independent validation before presenting payload bytes as decoded. |

The historical suggestion in `HANDOVER.md` that TarangMini decoding is blocked by above-PHY encryption is **not established**. Application encryption does not prevent recovery of on-air ciphertext bytes or, where present, verification of their PHY CRC. Header failure should first be investigated as synchronization/PHY/configuration mismatch. This assessment supersedes that hypothesis without rewriting historical handover entries.

## 4. What the supplied TarangNet API can and cannot provide

These capabilities apply to the documented TarangNet firmware, **not automatically to the installed LW-S201 firmware**:

- Read hardware/firmware/EUI and settings; select documented data-rate indices, carrier frequency, output power and addressing; send controlled UART application bytes; receive payloads and optional metadata.
- Send commands atomically over UART and allow at least the documented 500 ms response interval. Some settings require flash-save and restart, so sweeps must account for persistence and restoration.
- API selection can be fixed or controlled by the MODE pin depending on configuration. Transparent mode interprets serial bytes as data. The physical BOOT switch is separate and is used for programming; it should not be changed casually during testing.
- The manual does not expose a complete set of raw PHY controls: coding rate, explicit/implicit header, sync word, preamble length, LDRO and encryption control are not supplied as configurable fields.
- No complete TarangNet over-air frame schema is provided. UART framing is not evidence of the RF packet's exact bytes.
- TX acknowledgement is not proof of successful SDR decoding or of peer application delivery. RSSI/SNR readback concerns received traffic, not a measurement of the just-transmitted packet at our SDR.

Documentation discrepancies to preserve as uncertainties:

1. Rate indices `0x01` and `0x02` both say SF12/BW250. Do not silently correct the second to BW500 without evidence.
2. The read table includes SF5/SF6 indices `0x12`-`0x17`, while the write table ends at `0x11`.
3. Receive-payload framing uses `0x2d` in one table versus the generic `0x2b` response prefix elsewhere. Validate against actual receive frames for the matching firmware.
4. Some command access descriptions differ between summary and detailed sections. Implement only verified semantics for the detected version.

## 5. Recommended implementation order

### First: firmware-aware board support and reliable evidence capture

Obtain the LW-S201 API reference matching this board from the vendor. A bounded public search did not locate it. Keep the current firmware unless a deliberate later decision is made to replace it; the supplied loading guide is not a reason to flash it now.

Add a board connection panel showing stable USB path, UART parameters, raw firmware/hardware identity and supported operations. Unknown firmware must remain visibly unsupported for configuration/transmit commands. Preserve raw replies, timestamps and parser errors. Keep serial module identity separate from identities inferred from received RF.

Acceptance: connect/reconnect the observed adapter reliably; identify firmware before enabling writes; reject truncated, wrong-ID or unrelated event frames; never interpret LW-S201 values with TarangNet labels.

### Second: restore SDR reception and create independent RF fixtures

Resolve UHD discovery, then perform a receive-only sample capture with actual sample rate and overflow metadata. Confirm the module antenna, receiver setup and documented transmit behavior before controlled RF tests. Record frequency, SF/BW, applicable network/PHY settings, payload or join-request trigger, firmware, gain, timestamps and exact UART events with each IQ recording.

For the installed LoRaWAN firmware, a documented join-request trigger would be a useful first fixture: its identity fields can be parsed without decrypting application data. A gateway is not needed merely to capture a transmitted request, but may be needed to complete joining and validate subsequent application traffic. Do not infer a trigger command from the TarangNet manual.

Acceptance: independent on-air packets reproduced from saved IQ, with valid boundaries and expected identifiers/bytes; repeatability across more than one capture. Start with one verified mode before attempting a broad SF/BW sweep. Save overflowing or clipped captures as failed fixtures, not decoder regressions.

### Third: correct and validate the LoRa PHY

Use those fixtures and an independent reference implementation or known radio vectors to test sync mapping, first-block processing, timing/CFO, FEC, whitening, payload CRC and LDRO. Change one demonstrated failing stage at a time. Retain raw symbols and stage-level diagnostics. Test start offsets, noise, fractional CFO and truncated frames after clean independent packets pass.

Acceptance: exact recovery of independent physical payload bytes and valid integrity checks where present. An encoder round trip alone is insufficient. Do not classify internal-codec results as standardized hardware decoding without this evidence.

### Fourth: standardized LoRaWAN interpretation and GUI identification

On top of verified PHY bytes, add a bounded, version-aware LoRaWAN parser. Show message type, join-request identifiers where present, and visible data-frame address/control/counter/port fields. Distinguish observed fields from inferred values. Keep encrypted application bytes visible as hex; application semantics still require a device-specific payload schema, and decryption/authentication require the appropriate authorized keys and context.

Do not treat a visible session address as a permanent hardware identity, or claim that every packet contains a DevEUI. Link serial identity to an RF observation only with supporting capture evidence. Keep fingerprint clusters as probabilistic observations, separate from decoded protocol identity.

Suggested packet statuses: `detected`, `header valid`, `payload CRC valid`, `payload CRC failed`, `CRC absent`, `LoRaWAN parsed`, `MIC not checked`, `MIC valid/invalid`, and `application payload encrypted/decoded`. PHY CRC and LoRaWAN MIC are distinct checks. Bounds validation must precede reading any variable-length field.

Acceptance: independent join/data fixtures, malformed/truncated rejection, explicit integrity status, and persistent identity provenance in the GUI. Universal interpretation of every vendor's sensor payload is outside a standardized packet parser.

## 6. How far this board can take us

| Available setup | Achievable validation | Remaining limit |
|---|---|---|
| Current board over USB, supplied PDFs | UART connectivity and self-reported hardware/firmware; detection of API mismatch. Completed. | Reliable LW-S201 configuration and TX control need matching documentation. |
| Board plus working SDR and matching API | Controlled real uplink captures, synchronization/PHY debugging, visible LoRaWAN field extraction and realistic GUI fixtures. | One firmware/radio is not coverage of all LoRa devices or modes. |
| Authorized LoRaWAN network/gateway and keys | Join completion, traffic comparison, MIC checks and application decryption using correct context. | Application payload schemas remain vendor-specific. |
| Additional documented raw-LoRa radio or independent fixture source | Known PHY parameters and byte-level cross-validation across modes. | Broader hardware/firmware interoperability still needs a test matrix. |
| Second compatible TarangNet node, if that firmware is intentionally selected later | End-to-end TarangNet delivery, acknowledgements and RX metadata. | Does not establish a universal standard for proprietary TarangNet application framing. |

The best immediate path is therefore **firmware-aware host integration, working SDR capture, independent PHY validation, then LoRaWAN parsing and honest GUI status**. This board is useful for that path, but the current evidence does not justify claiming completed real-hardware decoding.

## 7. Changes and outstanding work

Created this assessment and small evidence files under `docs/tarangmini/2026-09-18/`. No source, GUI or device configuration changes were intentionally made. No firmware loading, defaults reset, flash save, transmit command or SF/BW sweep was performed. Existing unrelated CMake/bench changes were left intact.

Still needed for RF tests: a working UHD connection, confirmation of the RF antenna/setup, and a documented LW-S201 transmit/join procedure. These are concrete blockers, not evidence that the decoder works or that encryption is the cause of its historical failures.

## 8. Implementation follow-up (2026-09-18)

After the assessment, the user approved beginning the interpretability plan. The
app now has explicit integrity statuses, decoder provenance, one-shot IQ saving
and isolated offline replay. See [docs/LORA_CAPTURE_REPLAY.md](docs/LORA_CAPTURE_REPLAY.md)
and HANDOVER section 10 for changes and validation. The assessment's earlier
"no source changes" statements describe the investigation session, not this later
implementation. PHY interoperability and the hardware blockers remain unresolved.

### Failed sweep check, 2026-09-18 11:04 UTC

After the user reported `config_write(0x0008, 0x00)` returning status `01`,
readback confirmed the same firmware identity, EUI, and register `0x0008` value
`00` as the original baseline. Evidence: [post-sweep-readback.json](docs/tarangmini/2026-09-18/post-sweep-readback.json).
Inspection of `set_data_rate()` confirms that this failure occurs before its
flash-save and restart calls; the outer loop never reaches packet sending. The
`finally` block repeats the same rejected write and produces the second traceback.
No restore write or factory reset was needed for the attempted setting. This
checks the implicated register and identities, not an exhaustive device backup.
The script's SF12/BW125 label remains unsupported for this installed LoRaWAN
firmware; the TarangNet script must not be used to configure it.

### Firmware reflash confirmed, transmission restored, 2026-09-18 ~17:00-17:25 IST

The board was reflashed at some point after the failed preflight above (that
attempt's own log, `data/tarangmini_firmware_recovery/preflight.json`, records
`write_performed: false` from a bootloader-handshake timeout - a LATER attempt
evidently succeeded without leaving its own log entry here). A fresh read-only
`identify()` now returns **`TarangNet_TN_LW_STD_WL_v0_0_6`** - a different exact
string from the v0_0_4 the API doc names, but the same EUI (`3cc1f6050005df88`)
as the original LoRaWAN firmware, confirming this is the same physical module
with new firmware, not a different board.

`tools/tarangnet_api.py`'s `require_supported_firmware()` only accepted the
literal string `"TarangNet_TN_STD_WL_v0_0_4"`, so it rejected every write/
transmit call on this new firmware string even though the module itself was
correctly flashed and responsive - this, not a hardware fault, was the
"not transmitting" symptom reported after the reflash. Fixed by widening the
accepted set to `SUPPORTED_FIRMWARE = {"TarangNet_TN_STD_WL_v0_0_4",
"TarangNet_TN_LW_STD_WL_v0_0_6"}` (see that file's own header comment) on the
assumption that v0_0_6 is the same command family with a version bump - not an
independently confirmed fact, since no v0_0_6-specific manual was supplied.

Before trusting that assumption, a read-only `--inspect` pass was run first:
frequency read back as `865.100000 MHz`, matching a FREQ_NAMES table entry
exactly; data rate `0x00` (SF12,BW125) and connection mode (`router`) both
decoded to valid, documented values. All three lining up with the documented
v0_0_4 semantics is supporting evidence for the compatibility assumption, not
proof of the full command map.

With that check passed, `tarangmini_sf_bw_sweep.py --only 00 --reps 1` (the
module's own current rate, so no rate-change/flash/restart cycle was
triggered - the lowest-risk possible transmit test) produced a module uplink
ACK: `payload=b'SF12_BW125_TEST', response=2b038100`. The module confirms it
transmitted. **SDR reception was not verified in this pass** - UHD discovery
against the X310 was already failing earlier in this same assessment (see
Section 2's SDR receiver findings), and that was not re-tested here. Closing
the loop (confirming the SDR actually receives and decodes this uplink) is
still outstanding and depends on resolving that separate UHD/X310 blocker.

### Receiver blocker root cause found and cleared, 2026-09-18 ~17:50-18:00 IST

Re-checked UHD discovery fresh at the start of Milestone 2 work rather than
reusing the finding above (the plan explicitly requires this - a stale prior
failure must not be assumed to still hold). Host network path was unchanged
and healthy: `192.168.10.1/24` on `enp3s0`, ICMP round-trip ~0.6ms, valid ARP
entry (`00:80:2f:36:45:0e`, `REACHABLE`). `uhd_usrp_probe --args
addr=192.168.10.2` and `uhd_find_devices` still both failed ("No devices
found") despite this.

Root cause: a leftover `wifi_test_bench` process (PID 45769, from earlier
same-day bench testing, unrelated to LoRa) held three live UDP sessions to
`192.168.10.2:49152`/`:49153` (`ss -tunp` confirmed this directly) - the
X310's discovery/control service cannot answer a new probe while another
process already holds the device. Not a firewall or network configuration
issue. A second, older `rf_monitor_gui` process (PID 35308, ~1h20m uptime,
provenance unknown - possibly left running from this same day's earlier
assessment work) held no live socket to the X310 at check time and was left
running.

Fix: `kill 45769` (graceful SIGTERM, process exited; no SIGKILL needed).
Re-ran `uhd_usrp_probe --args addr=192.168.10.2` immediately after: full
successful enumeration, both UBX-160 v2 daughterboards found (Radio#0 serial
325B957, Radio#1 serial 325B9BA), mboard serial 326CF02. Full output saved at
[uhd-probe-post-fix.txt](data/lora_m2/2026-09-18-tx-recovery/baseline/uhd-probe-post-fix.txt).
The probe logged a UDP send-buffer-size warning (`net.core.wmem_max` at
1048576, wanting 2426666) - this is the same pre-existing, already-documented
limitation `config.hpp` and `PROJECT_STATUS.md` describe and already work
around by capping the X310 at 20Msps; it did not block this probe and was not
changed.

**Receiver is confirmed reachable again as of this check.** Physical antenna
attachment on the X310 side and the TarangMini's antenna are still
unconfirmed - the plan's Milestone 2 Section 5.1 capture-session work still
needs that confirmed with the user before any capture is treated as
meaningful, and needs the user to actually trigger transmissions during a
capture window since automating that timing was out of scope here.
