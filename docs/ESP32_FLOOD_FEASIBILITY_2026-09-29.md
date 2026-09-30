# ESP32-S3 disconnect flood feasibility — 2026-09-29

Scope requested by the user: preserve and reflash the currently connected
Heltec WiFi LoRa 32 V3, and assess whether it can produce a valid live input for
the implemented Wi-Fi disconnect flood rule. Single-disconnect capture and any
flood transmission are deferred to later steps. No LoRa project source is changed.

## Detection acceptance

The production `management_disconnect_flood` rule requires, after at least 30
clean analysed seconds and three included baseline windows, a two-second usable
observation window with at least 20 **distinct** deauth/disassoc frame contents,
from at least two capture records, for the same claimed BSSID and receiver. The
observed distinct-unit rate must reach the larger of 10/s or 4 times the combined
baseline p95. Repeating the identical packet does not increase this distinct
count. Successful calls or serial counters alone cannot satisfy the criterion.

## ESP32-S3 transmitter assessment

- Official Espressif Wi-Fi API supports `esp_wifi_deauth_sta(aid)` for the SoftAP's
  *currently associated* stations. This can test normal client removal from the
  board's own AP. The API does not promise a repeatable stream after the station
  is no longer associated, and its documented semantics do not supply a
  disassociation flood facility.
- Official `esp_wifi_80211_tx()` documentation lists beacon, probe request,
  probe response, action and non-QoS data frames; deauth and disassoc are absent.
  The installed ESP32-S3 Arduino SDK header matches the same API shape. Do not
  assume that a crafted deauth buffer reaches the air.
- The supplied `Downloads/atest/attack_simulator/main/wifi_attacks.c` calls the
  raw TX API for deauth/disassoc and increments its loop's `frames_sent` even
  when the API returns an error. Its blocking flood loop also prevents the
  queued stop command from being processed until the loop ends. This is not a
  trustworthy positive-test transmitter without independent RF confirmation.
- The owned-AP fixture at `tools/esp32_wifi_lab/` permits a deliberate single
  disconnect with a ten-second cooldown, no raw injection and no flood mode.

**Conclusion:** This board is suitable for verifying ordinary disconnect-frame
reception after the user's spare client joins its own AP. With the documented,
supported interfaces and one client, it is **not currently established** as a
source of 20+ distinct qualifying frames in two analysed seconds. No positive
live flood result should be claimed from this setup. A capable authorized lab
transmitter with RF isolation and an independent monitor log would be needed to
validate the sustained positive case. Offline synthetic and real-packet-derived
tests already exercise the detector logic, but they do not establish RF yield.

The user reported no RF shield box. Thus no sustained over-the-air flood was
attempted. The next task, when requested, is one normal owned-client disconnect
captured by the X310 and correlated to a serial command. A later positive flood
campaign requires a transmitter whose emitted frame count/content is independently
verified, plus RF isolation and a controlled test network.

References: [Espressif ESP32-S3 Wi-Fi API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_wifi.html),
[existing flood implementation](WIFI_SECURITY_FLOOD.md),
[owned-AP fixture](../tools/esp32_wifi_lab/README.md).

## Step 1 completion

The changed firmware was saved as the full 8 MB image at
`data/esp32_wifi_validation/2026-09-29/pre-reflash/current_8MB.bin`.
SHA-256: `85fe89b670f6da9db2ee3e49872002ce6b0b1611262ca17b98020bba42e72417`.
Esptool verified the image against the connected board (digest matched).
This differs from the earlier current-LoRa image and is the correct image to
restore if the user wants the firmware that was on the board immediately before
this reflash.

The limited owned-AP fixture was then uploaded once; the upload hash check passed.
Serial boot and `status` confirmed the expected firmware and `AP=off, clients=0`.
No `ap_start`, `disconnect_once`, flood or USRP capture was run in this step.
To restore the immediately preceding image later, use the exact image above:

```sh
python3 /home/sudeep/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
  write_flash 0 data/esp32_wifi_validation/2026-09-29/pre-reflash/current_8MB.bin
```

The serial console was closed after the idle status check. The board currently
runs the Wi-Fi fixture with its AP off.


## Measured Step 2 and Step 4 outcome

The subsequent user-authorized own-client disconnect was captured by the X310.
One esp_wifi_deauth_sta(0) call produced 17 FCS-valid over-the-air disconnect
transmissions in one capture: 12 disassociation and five deauthentication frames.
Retry-normalized contents collapse to three distinct units. The incident count
was zero in both the live scan and offline replay, as expected for an ordinary
disconnect. See [the reception check](ESP32_WIFI_RECEPTION_CHECK.md) and its
saved recording for times, addresses and full coverage results.

This improves the transmitter assessment: the supported SoftAP operation did
emit both disconnect subtypes. It does **not** establish a qualifying sustained
flood source. The detector requires at least 20 distinct units across two
captures within its usable two-second window; this observed event supplied
three units in one capture. Repeating the same call after a client rejoins would
also be separated by the reconnect interval and the fixture's ten-second
cooldown. The user has no RF shield box, so no sustained over-the-air flood was
attempted. A positive live flood test remains open pending a controlled
RF-isolated setup and independently verified stimulus. Do not label the
zero-incident result as a failed detector: the positive trigger condition was
never presented.

After the finite receive-only scan, the test AP was stopped. The immediately
prior firmware remains recoverable from the verified image above.
