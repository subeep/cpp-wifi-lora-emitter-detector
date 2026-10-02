# X310 receive-only security evidence check — 2026-10-02

The connected X310 passed discovery, sample reception, device timestamp and
live/offline agreement checks. These are receiver/evidence checks, not positive
attack-detection tests. No RF transmission or firmware flashing was performed.

## Hardware and artifacts

- Address `192.168.10.2`, serial `326CF02`, UHD 4.1.0.5, FPGA XG/38.0.
- Both UBX-160 v2 boards were recognised. All captures used channel 0, RX2.
- Evidence root: `data/security_hardening/2026-10-02/rx-check-095237/`.
  `hardware-summary.json`, `metadata-checks.json` and
  `wifi-live-comparison.json` retain the machine-checked results.
- The hardware RX gain range is 0–31.5 dB. Gain fields describe requested gain;
  the Wi-Fi live smoke tool requested 40 dB, which UHD clamps. Do not interpret
  that field as a measured 40 dB receiver setting.

## Wi-Fi

Four 200 ms captures at 20 Msps covered 2462 MHz twice, 2437 MHz and 5180 MHz.
Each returned all 4,000,000 requested samples with no overflow, timeout or
exception. Device anchors advanced without overlap; actual RF minus DSP tuning
matched the requested receiver centre (including the Wi-Fi frequency offset).

Production saved-IQ replay accepted 23 FCS-valid frames: 21 OFDM and two DSSS.
All had device timestamps. Replaying the generated event log reproduced the
entire snapshot exactly. There were no incidents; the short capture is not a
mature baseline or a positive attack case.

A separate 40-second production scanner run on channel 11 returned 36 captures,
36 seconds sampled and 35.983372 seconds analysed. It accepted 2,550 FCS-valid
frames (1,893 OFDM, 657 DSSS), with device timing for every capture and frame.
There were no overflows, receive failures or queue losses. Measured gaps totalled
1.661841 seconds; 35 redundant retunes were skipped.

One capture reached the 4,000-burst limit. The monitor correctly excluded it
from clean baseline learning and replay/flood continuity; this is visible in
the evidence, rather than being counted as fully observed traffic. The beacon
replay rule evaluated 591 beacons; no incident was produced. Offline event replay
matched all live semantic snapshot fields and the live 256-event recent tail.
Only the recent-list capacity and recording-byte accounting differed.

## LoRa

A standalone two-second IQ capture at 866.9 MHz, 500 ksps, requested gain 5 dB
returned all 1,000,000 samples without overflow. Its saved-IQ and event snapshots
matched exactly.

The production LoRa scanner then collected four two-second captures at the same
settings. All eight sampled seconds were analysed and usable, all four captures
had device anchors, and there were no exclusions, input losses or recording
errors. Device anchors were ordered and nonoverlapping; measured gaps totalled
3.055955 seconds. Full live/event replay snapshots matched exactly. Replaying
the first saved IQ capture also matched its corresponding live event batch.

No LoRa packets were decoded. This verifies quiet-channel coverage and timing,
not reception of a known LoRa source. LoRa attack rules remain unimplemented.

## Reproduction and next gate

`lora_scan_smoke` is a finite receive-only production-lane helper added for this
check. It saves the first IQ capture and isolates event, identity and legacy
fingerprint logs in a new output directory. Build with
`cmake --build build --target lora_scan_smoke`; use
`build/lora_scan_smoke 866900000 30 NEW_DIRECTORY` with the X310 connected.
`wifi_scan_smoke` and `wifi_capture_cli` supplied the Wi-Fi evidence.

Next hardware requirement: the Heltec V3/SX1262 as a known LoRa source, with its
current firmware preserved and compatible owned-test settings established.
Wi-Fi positive flood/replay validation still requires an independently verified
transmitter and a controlled setup. Neither absence of incidents here nor the
earlier ESP32 raw-deauth rejection validates positive attack detection.
