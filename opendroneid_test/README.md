# OpenDroneID over-the-air test transmitter (ESP32-S3)

This is a standalone test rig for the Wi-Fi Remote ID receiver in `src/drones/remote_id.cpp`.
Nothing in this folder is part of the main build, and nothing outside it was changed.

- `firmware/` is an ESP-IDF 5.2 app. It sends a simulated multirotor that circles a fixed
  point. The aircraft is sent as ASTM F3411 message packs in **Wi-Fi Beacon** frames
  (vendor IE `fa:0b:bc / 0d`) and **NAN action** frames (service ID `88:69:19:9d:92:09`).
  The encoder (`main/odid_encode.c`) was written from the wire layout and doesn't share
  code with the receiver.
- `host/odid_serial.py` talks to the board's serial console. `check` captures the exact
  frames being sent and decodes them with `build/wifi_remote_id_inspect`.

## What it sends

The defaults are set in `idf.py menuconfig` → *OpenDroneID test transmitter*.

| Item | Default |
|---|---|
| Channel / rate | 2.4 GHz ch 6, 1 Mbps DSSS |
| Transports | Beacon + NAN, every 100 ms |
| TX power | 10 dBm max |
| Protocol | v2 (F3411-22a) |
| Basic ID | Serial `ESP32S3RID` + last 3 MAC bytes (`ESP32S3RIDD682C4`), multirotor |
| Location | 40 m circle at 5 m/s, 30 m above takeoff, airborne |
| System | Operator at the circle centre, EU open / C0, v2 timestamp |
| Self ID / Operator ID | `newrock ODID bench test` / `TEST-OPERATOR-0001` |
| Transmitter address | the board's STA MAC, `70:af:09:d6:82:c4` |

The circle centre is a placeholder (51.4779, -0.0015). Set `ODID_CENTER_LAT_E7` and
`ODID_CENTER_LON_E7` to your own location if you want sensible positions on a map.

## Build and flash

Linuxbrew's `python3` (3.14) shadows the Python 3.10 that ESP-IDF's venv was made for,
so put `/usr/bin` first:

```bash
cd opendroneid_test/firmware && export PATH=/usr/bin:$PATH && . ~/esp/esp-idf/export.sh && idf.py -p /dev/ttyUSB0 flash
```

## Control it at runtime

Opening the port may reset the board, and the settings then go back to their defaults.

```bash
/usr/bin/python3 opendroneid_test/host/odid_serial.py check
```

```bash
/usr/bin/python3 opendroneid_test/host/odid_serial.py cmd ch 11
```

The console commands are `status`, `ch <1-11>`, `rate <1|2|5|11|6..54>`,
`mode beacon|nan|both`, `int <ms>`, `pwr <8-84 quarter-dBm>`, `ver <0-2>`,
`time <unix>`, `hex [n]` and `start`/`stop`. Because a reset reverts the settings,
use `idf.py menuconfig` for changes that should stick.
The board's clock starts at the firmware build time, which is local time treated as UTC.
`check` sets it to the host's UTC clock.

## Receive it with the monitor

1. Run `build/rf_monitor_gui` and monitor a 2.4 GHz band that includes the transmitter's
   channel (6 by default).
2. Open **Drones → Wi-Fi Remote ID** and enable **Wi-Fi OpenDroneID**.
3. Two sources should appear for `70:af:09:d6:82:c4`, one *Wi-Fi Beacon* and one
   *Wi-Fi NAN*. Each should show the Basic ID `ESP32S3RIDD682C4`, a position that moves
   around the circle, and the Self ID and Operator ID above.

To isolate a PHY path, try `rate 6` (OFDM) as well as the default 1 Mbps DSSS. To isolate
a transport, try `mode beacon` or `mode nan`.
