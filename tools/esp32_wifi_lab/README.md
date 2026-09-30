# Owned-AP Wi-Fi disconnect reception check

Heltec WiFi LoRa 32 V3 / ESP32-S3, Arduino on PlatformIO. This fixture tests
reception of a normal AP-originated disconnect. It is not a deauthentication
flood generator and cannot establish flood-detector sensitivity.

- Boots with Wi-Fi disabled; no SX1262, LoRa or OLED code.
- `ap_start`: dedicated WPA2 test AP `RFMON-LAB-D682C4`, password
  `rfmon-test-29`, channel 11, maximum one associated client, no upstream access.
- `status`: reports AP state, BSSID and associated client count.
- `disconnect_once`: asks the ESP32 driver to disconnect its own associated
  station using `esp_wifi_deauth_sta`. Requires one client; 10-second cooldown.
- `ap_stop`: stops the AP. BOOT has no application command binding.

Commands are newline-delimited at 115200 baud on the CP2102 UART. Reduced transmit
power is not RF isolation. Use an owned test client. Do not interpret a successful
API return as proof of a received frame; correlate with USRP/reference captures.
A single normal disconnect should NOT create a flood incident.

Build:

```sh
platformio run -d tools/esp32_wifi_lab
```

Before flashing, back up and verify current full flash. Do not overwrite the
original factory image. The 2026-09-29 session uses
`data/esp32_wifi_validation/2026-09-29/current_lora_8MB.bin` for the current-image
backup, separately from `/home/sudeep/Documents/factory_backup_8MB.bin`.
The connected chip reports ESP32-S3 revision 0.2, 8MB flash, MAC
`70:af:09:d6:82:c4`; soft-AP BSSID must be obtained at runtime.

Recovery (only when intentionally restoring the corresponding firmware):

```sh
python3 /home/sudeep/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
  write_flash 0 data/esp32_wifi_validation/2026-09-29/current_lora_8MB.bin
```

Restoring the current LoRa image may resume its automatic 888 MHz transmissions.
A factory image is a separate restore choice, not necessarily the current LoRa
application. Never erase or write eFuses as part of this test.
