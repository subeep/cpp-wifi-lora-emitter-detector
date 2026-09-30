# ESP32 deauthentication shield-box test — 2026-09-29

The user authorized a positive live flood test aimed at their phone, with the
ESP32, phone and X310 reportedly inside an RF shield box. This document records
what was actually performed and the gate still open.

A separate bounded firmware fixture was built and flashed to the Heltec ESP32-S3.
Its flash image was verified by the uploader. Boot and serial `status` confirmed
Wi-Fi/AP off and zero probe/burst attempts. The prior board image remains saved at
`data/esp32_wifi_validation/2026-09-29/pre-reflash/current_8MB.bin`
(SHA-256 `85fe89b670f6da9db2ee3e49872002ce6b0b1611262ca17b98020bba42e72417`).
The new fixture uses no LoRa API.

After the user confirmed the box was closed, a receive-only X310 recording began
on channel 11. The ESP32 AP was started and its serial status confirmed one
associated phone. Before any deauthentication probe, the X310 decoded hundreds
of FCS-valid beacons from unrelated APs despite the reported closed box. The
largest outside source was `Airtel_Zerotouch`, BSSID
`92:f0:4c:34:99:b9`: 451 beacons during the 104-capture recording, with
median recorded power about -22.3 dB in scanner units. The test AP's BSSID
`72:af:09:d6:82:c4` contributed 195 beacons at median about -7.3 dB.
After the ESP32 AP was stopped at 12:13:00 UTC, the receiver still decoded 38
beacons from `Airtel_Zerotouch` and six from another outside BSSID. The finite
scan accepted 3,376 frames, recorded no overflow and zero incidents.

This does not establish RF containment. It may reflect a lid/feedthrough/antenna
path issue; these observations alone do not locate the leak or quantify outbound
attenuation. **No `probe_once` or `flood_once` command was sent.** The ESP32
AP was stopped and serial status showed zero probe/burst attempts. The user was
asked to check the enclosure and RF cable/antenna paths. After correction, the
next gate is a closed-box passive scan that sufficiently suppresses outside
beacons, then one raw-frame probe measured by the X310. Only a verified probe
can justify the bounded burst and a production-rule incident check. Espressif's
documented raw-transmit API does not list deauthentication frames, so an API
success alone will not be accepted as RF evidence.

Artifacts:
`data/esp32_wifi_validation/2026-09-29/shield-box-probe/` has the receive-only
events and snapshot; `shield-box-probe.log` and `shield-box-console.log`
retain scanner and ESP32 timings. The isolated fixture source is in
`tools/esp32_wifi_shield_test/`.

## Subsequent one-frame probe — 2026-09-29

The user then requested only a few addressed frames to verify transmitter
capability. A new receive-only X310 channel 11 recording was started. After the
phone rejoined the lab AP and the user reconfirmed that the box was closed,
serial status showed exactly one associated client. At 12:17:42.979 UTC,
`probe_once` was issued. The ESP32 Wi-Fi driver printed
`unsupport frame type: 0c0` and returned `ESP_ERR_INVALID_ARG` at
12:17:42.992 UTC. In the six-second USRP interval around the call, 107 frames
were decoded but none was a deauthentication or disassociation frame. Thus the
API rejected the raw deauthentication request, and no RF transmission from
that request is established.

The AP was stopped at 12:18:05 UTC. Final ESP32 status was AP off,
`probe_used=1`, `burst_used=0`, `burst_active=0`, `attempts=0`,
`api_ok=0`. The bounded burst command was **not** sent. The user-authorized
positive flood test remains unperformed because this ESP32-S3 driver rejects
raw deauthentication frames. A different independently verified transmitter
is needed for an RF-positive production-detector test; the existing supported
SoftAP own-client disconnect was already measured separately and yielded only
three retry-normalized units in one capture.

The new recording and serial timestamps are in
`data/esp32_wifi_validation/2026-09-29/shield-box-one-probe-connected/`,
`shield-box-one-probe-connected.log`, and `shield-box-console.log`.

### Completed recording check

The final 72-capture connected-phone recording accepted 1,405 FCS-valid
frames, including zero deauthentication and zero disassociation frames. It had
zero overflow, 36 flood-rule evaluations, no excluded flood captures, and zero
incidents. Offline replay matched the live capture count, frame-type totals,
flood diagnostics, coverage and incident count. This confirms a negative
transmitter result, not detector sensitivity to a real flood.
