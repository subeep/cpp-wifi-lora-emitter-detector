# Legacy OFDM beacon reception

Implemented 2026-09-19 in `src/wifi_ofdm_rx.*`. The scanner now decodes
legacy 20 MHz OFDM beacons and probe responses on both Wi-Fi bands, alongside
the existing DSSS receive path. No transmitter is used. LoRa code, radio
configuration, and fingerprint acceptance thresholds were left unchanged.

## Receive path and scope

The receiver mixes to the monitored channel and, if needed, uses a windowed-sinc
filter to resample to 20 Msps. It detects L-STF, corrects coarse CFO, locates the
two L-LTF symbols, estimates the channel and fine CFO, then decodes L-SIG.
Payload recovery includes pilot phase/slope correction, soft constellation
metrics, deinterleaving, depuncturing, K=7 Viterbi decoding, scrambler recovery,
SERVICE checks, and the MAC FCS. Rates are 6/9/12/18/24/36/48/54 Mbps.
The convolutional state puts the newest bit in bit 0, so standard octal
133/171 taps are represented as bit-reversed 155/117 masks.

Only an FCS-valid beacon/probe response from the shared `wifi_frame` parser can
create a decoded identity. L-SIG validity or a preamble alone is insufficient.
HT/VHT/HE payload decoding is not implemented. Those formats can share a legacy
preamble; diagnostics such as invalid SERVICE or failed FCS do not distinguish
an unsupported format from RF corruption. An AP advertising HT/VHT/HE capability
IEs can still send a perfectly decodable legacy OFDM beacon.

The existing energy detector, modulation classifier and 4 MHz occupied-bandwidth
gate still precede live decoding. OFDM gets 4 us context around each burst and
does not use the DSSS beacon-duration gate. Captures are finite and channel swept:
collisions, weak signals, merged bursts, truncated packets, or off-channel traffic
can be missed. This is not exhaustive capture or a claim that every beacon decodes.
Input is bounded to 6 ms at 20–64 Msps. Actual B210 operation has not been tested;
56 Msps resampling has software coverage only. X310 tests used 20 Msps.

## Identity and GUI integration

- Verified identities persist even when fingerprint quality gates reject RF data.
- Identity records now save `phy`; older identity records without it default to
  DSSS. Identity PHY is separate from the latest RF reading's PHY and survives
  compaction/restart. Existing schema-2 files remain readable.
- The packet table shows decode status, OFDM rate and PSDU length, plus the
  decoded BSSID/SSID/security metadata. A valid non-beacon frame is reported as
  such and does not supply a beacon identity. Failed frames do not create one.
- The identity detail popup shows the FCS-valid observation's PHY. It works
  even with zero accepted RF readings. Channel mapping uses the current band,
  including advertised 5 GHz channels. The monitored channel is retained separately.
- RF clusters are not automatically merged into BSSID identities. Multiple BSSIDs
  are not proof of multiple physical radios. RF calibration is outside this change.

## Hardware evidence (2026-09-19)

Receive-only X310, serial 326CF02, address 192.168.10.2, channel 0 / RX2, requested
20 dB gain, actual 20 Msps. Raw capture manifests record requested tuning/gain,
actual sample rate, host time, overflow state, and an FNV-1a accidental-corruption
checksum. Successful captures below contain 7,000,000 samples (350 ms), no overflow.

| Recording | Replay settings | FCS-valid beacons |
|---|---|---|
| `ofdm-initial-20260919-retry1/0-2412.json` | Production threshold 12 dB / max 400 bursts | 2, `Airtel_kira_7992`, `92:f0:4c:40:52:99`, channel 1 |
| `ofdm-initial-20260919-retry1/3-5180.json` | Diagnostic threshold 6 dB | 8, `Avgarde_airtel` (`3c:52:a1:0b:bf:d6`) and hidden sibling, channel 36 |
| `ofdm-offset-20260919/1-5180.json` | Production +1.5 MHz tuning offset, threshold 12 dB / max 400 | 7, `Avgarde_airtel` and hidden sibling, channel 36 |

All received beacons in these checks used 6 Mbps. Eight-rate coverage is from
software vectors, not an eight-rate over-the-air certification. Additional
FCS-valid non-beacon frames were recovered; replay does not save them unless
explicitly asked for packet output, and does not update live identity storage.

The production `Scanner` then passed receive-only `wifi_scan_smoke 5g` and
`wifi_scan_smoke 2g4` runs with the normal tuning offset and default threshold:

- 5 GHz: decoded and persisted `Flo Mobility upstairs`, BSSIDs
  `98:ba:5f:37:48:c7`, `98:ba:5f:37:57:4b` and hidden BSSIDs
  `9e:ba:5f:37:48:c7`, `9e:ba:5f:37:57:4b`.
- 2.4 GHz: decoded and persisted `Airtel_kira_7992`, `92:f0:4c:40:52:99`.
- Both finished connected with no reported capture overflow or scanner error.
  The tests stopped the scanner and released the radio afterward.

The X310 intermittently failed RFNoC graph initialization and once returned an
empty first capture. Those attempts are not counted as successful validation.
The normal scanner's existing connection retry handled the 5 GHz live test.
No driver, network-buffer, FPGA, radio-configuration or LoRa changes were made.
Raw recordings are ignored local data under `data/wifi_captures/`; three small
beacon crops with provenance/expected bytes are in `tests/fixtures/wifi_ofdm/`.

Python zlib independently verified each cropped MPDU's CRC, and tshark independently
parsed the BSSID/SSID/channel fields. Their expected IQ-to-MPDU bytes were obtained
from this receiver, so these are regression fixtures, not an independent PHY
oracle. An attempted offline GNU Radio `ieee802_11` reference comparison failed
because the installed module segfaulted; no reference IQ decode success is claimed.

## Build, tests, and tools

```sh
cmake -S . -B build
cmake --build build -j4 --target rf_monitor_gui test_wifi_ofdm test_wifi_identity test_wifi_gui wifi_ofdm_replay wifi_capture_cli wifi_scan_smoke
# Fixture generator needs NumPy + SciPy; neither is a production C++ dependency.
python3 tests/generate_wifi_ofdm.py /tmp/wifi-ofdm-vectors
./build/test_wifi_ofdm tests/fixtures/wifi_ofdm /tmp/wifi-ofdm-vectors
./build/test_wifi_identity
./build/test_wifi_gui /tmp/wifi-ofdm-gui

# Offline replay: default threshold/cap match production. No live storage writes.
./build/wifi_ofdm_replay data/wifi_captures/ofdm-offset-20260919/1-5180.json
# Optional packet output must be a new file; --threshold-db 6 enables diagnostic search.

# Hardware tools; close any other application using the USRP first.
# Capture directory must be NEW. At most one second per frequency.
./build/wifi_capture_cli data/wifi_captures/NEW_NAME 1500000 0.35 2412000000 5180000000
# Finite production scanner check (up to 90 seconds plus an in-flight scan step).
# Writes normal live Wi-Fi identities/RF records; never selects sub-GHz.
./build/wifi_scan_smoke 5g
./build/wifi_scan_smoke 2g4
```

Verified: 109 OFDM checks (all eight rates, exact bytes, real captures, rejection,
CFO, multipath, long-packet pilot sequence wrap, 56 Msps resampling and persistence),
45 identity checks including backwards compatibility/compaction, production GUI
render/popup interaction with screenshots inspected, and the existing Wi-Fi PHY,
DSSS RX, frame, fingerprint, master-list and stress suites. Production GUI and
smoke tools build successfully.

Next useful work is measured decode-yield/calibration against a working independent
receiver or a controlled AP capture set, especially weak/multipath signals and
other beacon rates. HT/VHT/HE payloads and wider channel decoding remain separate
future work; this milestone does not require them for legacy OFDM beacons.
