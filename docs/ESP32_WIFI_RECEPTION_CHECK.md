# ESP32 owned-AP disconnect reception check — 2026-09-29

The user authorized temporarily repurposing the Heltec WiFi LoRa 32 V3 for Wi-Fi
validation. Its previous firmware reported automatic LoRa transmissions at
888 MHz. Project LoRa source/settings were not changed.

## Recovery

- Original factory image: `/home/sudeep/Documents/factory_backup_8MB.bin`,
  8,388,608 bytes; SHA-256
  `3cbcde3ae9a0a88b22c39c08a0f60d152df7b71b35def1753057a55c74ac105a`.
  A second copy in `Documents/usrp x310/` matches.
- Saved current flash before writing:
  `data/esp32_wifi_validation/2026-09-29/current_lora_8MB.bin`, SHA-256
  `e5bbd289228b531a70dbfe7fcf529470969a4103b1659584f73e0aea829badae`.
  Full 8 MB esptool device verification passed (digest matched).
- These images differ. Restore the current-image backup to recover the prior
  LoRa application; do not assume the factory image contains it.
- Board identified as ESP32-S3 revision 0.2, 8 MB flash, chip MAC
  `70:af:09:d6:82:c4`, CP2102 at `/dev/ttyUSB0`.

## Fixture and scope

`tools/esp32_wifi_lab/` builds with the existing PlatformIO ESP32-S3 Arduino
packages. It uses normal SoftAP operations only, no raw-frame injection and no
SX1262/LoRa API. It boots with Wi-Fi disabled, starts only on serial command,
allows one associated client, and supports a single deliberate own-client
disconnect with a ten-second cooldown. BOOT has no application action.

Build and upload completed successfully; esptool verified programmed image
hashes. Serial confirmed idle boot, then successful AP start:

- SSID `RFMON-LAB-D682C4`, temporary test password `rfmon-test-29`.
- Channel 11, actual SoftAP BSSID `72:af:09:d6:82:c4`.
- No upstream internet; reduced transmit power does not provide RF isolation.

The user was asked to connect a spare client. The acceptance gate is an FCS-valid
management disconnect attributed to the test AP/client in the USRP recording,
correlated with the serial command. A normal single disconnect should not cause
a flood incident. A successful API result alone is insufficient RF evidence.
This fixture cannot validate sustained deauth/disassoc flooding.

Session logs, firmware/recovery hashes, isolated scanner state and captures are
under `data/esp32_wifi_validation/2026-09-29/`. The serial console only accepts
`status`, `ap_start`, `disconnect_once`, `ap_stop` and `quit`; opening it can reset
a board, so use it only with the known test firmware.

## Simulator audit

The supplied `Downloads/atest/attack_simulator` was inspected but not run, flashed
or modified. Its transmit counters advance even after TX API errors; the queued
stop command is not processed while blocking transmit loops run; BOOT queues a
deauth test. It is not a trustworthy live-test instrument as supplied. The new
fixture avoids those behaviors and does not add a flood-generation capability.


## Live owned-AP disconnect result — 2026-09-29

The user connected their phone to the test AP. Serial status confirmed one
associated client at 11:59:25 UTC. A fixed-channel, receive-only X310 recording
on channel 11 was running before the single disconnect_once command at
11:59:45.040 UTC. The board returned ESP_OK at 11:59:45.052 UTC.

The X310 recorded 17 FCS-valid disconnect transmissions from the test AP
72:af:09:d6:82:c4 to the associated client f4:30:8b:af:56:69, from
11:59:45.047 to 11:59:45.641 UTC: 12 DSSS disassociation frames and five
deauthentication frames (three OFDM, two DSSS). All were in capture 39. After
ignoring the Retry bit and FCS, these represented **three distinct frame
contents**, not 17 independent requests: one disassociation sequence and two
deauthentication sequences. This establishes RF reception and packet decoding
for the ordinary owned-AP disconnect; the serial API result alone would not.

The finite scan analysed 164 seconds across 164 captures, accepted 8,412 valid
frames, and reported no overflow, failed capture, security-queue loss or incident.
The flood rule evaluated 82 windows with no excluded captures. No flood alert is
the expected result for this one ordinary disconnect. Offline replay of the saved
events.ndjson matched the live snapshot for frame counts, PHY/type counts,
coverage, rule diagnostics and incidents (zero).

Artifacts: data/esp32_wifi_validation/2026-09-29/live-single-disconnect/
contains events.ndjson, snapshot.json and offline.json; the adjacent
live-single-disconnect.log and live-disconnect-console.log retain scan and
serial timing. The phone had automatically rejoined by 12:02:46 UTC. The test AP
was stopped at 12:03:31 UTC. No sustained flood was transmitted.
