# Bluetooth & Zigbee page — 3 November 2026

## Delivered

A new **Bluetooth & Zigbee** workspace page lists Bluetooth LE advertisers and
IEEE 802.15.4 / Zigbee nodes decoded from the 2.4 GHz captures the receiver
already takes. It is receive-only and **off until "Start Bluetooth & Zigbee
decoding"** is pressed. Tuning, scheduling and existing decoders are unchanged:
the decoder is one more Wi-Fi IQ observer next to the drone and ML observers.

- Two lists side by side (BLE left, 802.15.4 right), each with search, a
  selectable table and an inspector showing every decoded field and the raw bytes.
- Channel coverage table: decoded seconds, captures, CRC-valid packets, syncs and
  failed checks per BLE advertising channel and per 802.15.4 channel.
- Recent packets (200 per protocol), JSON export to `data/shortrange/`.
- Drones page: a **Bluetooth Remote ID** section lists ASTM F3411 messages from
  BLE legacy advertising, parsed with the existing Remote ID message parser.
- Capabilities page entry; legacy presentation gets a tab too.

## What is decoded

**Bluetooth LE** (LE 1M, advertising channels 37/38/39, access address
0x8E89BED6): GFSK demodulation at 4 samples/symbol, de-whitening, CRC-24 check.
PDUs: ADV_IND, ADV_DIRECT_IND, ADV_NONCONN_IND, ADV_SCAN_IND, SCAN_REQ, SCAN_RSP,
CONNECT_IND (connection parameters), ADV_EXT_IND (extended header incl. AuxPtr).
Address type (public / random static / resolvable / non-resolvable) and OUI vendor
for public addresses. AD structures: flags, service UUIDs (16/32/128-bit, named),
local name, TX power, appearance, advertising interval, service data, URI,
manufacturer data with company name. Recognised formats: Apple iBeacon and
Continuity (Find My, AirPods pairing, Handoff, Nearby, AirPlay, …), Google Fast
Pair, Eddystone, Exposure Notification, Microsoft Swift Pair / CDP, ASTM Remote ID.
Company, service-UUID, AD-type and appearance names (2,219 / 995 / 46 / 150) were
generated from the local Wireshark 3.6.2 tables; newer assignments show as numbers.

**IEEE 802.15.4 / Zigbee** (O-QPSK 2.4 GHz, channels 11–26): MSK-style
demodulation with 16-symbol template correlation and chip-timing tracking,
preamble + SFD sync, CRC-16 FCS check. MAC: frame type, flags, PAN IDs and
addresses (2003/2006 and 2015 rules), security header fields. Beacons: superframe,
PAN coordinator, association permit, Zigbee stack profile, extended PAN ID,
router/end-device capacity. MAC commands (association, data request, beacon
request, …). Zigbee NWK header: addresses, radius, IEEE addresses, source route,
security header. 6LoWPAN / Thread recognised by dispatch byte. Role hints (PAN
coordinator, router, end device, joining device) come only from frames seen.

## Not decoded (by design or limit)

Classic Bluetooth (BR/EDR hops over 79 MHz), BLE connections and data channels,
LE 2M / LE Coded and secondary-channel extended advertising, and any encrypted
payload (Zigbee NWK/APS, MAC security, BLE encryption). No keys are captured or
used, and nothing is transmitted (no active scanning).

## Coverage

Each target channel is decoded only while a capture covers it (usable ±8.8 MHz at
20 MSps, minus the signal's half-width):

| Mode | BLE 37 (2402) | BLE 38 (2426) | BLE 39 (2480) | Zigbee 11 | Zigbee 12–26 |
|---|---|---|---|---|---|
| Wi-Fi 2.4 GHz / Both | no | Wi-Fi ch 3–5 | Wi-Fi ch 13 | no | yes |
| Coordinated | 2401 MHz visit | yes | yes | 2401 MHz visit | yes |
| 5 GHz / sub-GHz | no | no | no | no | no |

The lists are **addresses seen, not devices**: phones rotate private BLE
addresses (~15 min), and a Zigbee node can appear by short and IEEE address.
Infrequent transmitters can be missed between visits.

## Processing and load

`src/shortrange/`: `ddc.hpp` (block polyphase channel extraction to 4 MSps,
energy gating), `ble.*`, `ieee802154.*`, `assigned_numbers.*`, `monitor.*`
(bounded worker, lists, coverage, export), `panel.*` (ImGui page). The observer
copies a capture into a queue of at most 4 captures / 768 MiB; beyond that a
capture is dropped and counted, never waited for. Channels decode in parallel
at nice 10 (below the Wi-Fi lane's 5). Lists hold 2,048 BLE and 256 802.15.4
addresses (oldest evicted, counted).

Measured on this machine: ~0.2–0.28 s to decode a 1 s, 20 MSps capture
(4–5 channels in parallel); coordinated 0.52 s visits take ~0.1 s.

## List order and rotating addresses

Rows keep a fixed position: BLE entries with an advertised name first, then
first-seen order (802.15.4: known IEEE vendor first). Only counts change as
packets arrive; power and last-seen start as hidden columns. Tables have a
drag handle for height and resizable / reorderable / hideable columns.

Phones and laptops use private addresses that change about every 15 minutes.
Without the device's identity key (IRK) no passive receiver can prove two
addresses are one device, so **"Merge rotating addresses"** (on by default) is a
provisional heuristic. A new resolvable / non-resolvable address is joined to an
earlier one when, after the new address has sent 3 packets:

- the earlier address went quiet before the new one's first capture, at most
  180 s earlier, and has no other successor;
- they share an advertising layout (PDU type, AD types, company / Apple
  Continuity types / service UUIDs; values and lengths ignored);
- median carrier offsets are within 3 kHz and median powers within 12 dB;
- the best candidate beats the next one clearly; otherwise nothing is linked.

A link breaks automatically if the earlier address is heard again. A merged row
keeps its place, shows the current address with "(+N)" and sums the counts; the
inspector lists the chain and the latest link's evidence. "Hide if not heard for"
(2 / 10 / 30 min) hides quiet rows instead. Unticking the option shows every address.

**Receiver centre fix found here.** UHD's RX tune result centres the stream at
`actual_rf − actual_dsp` (equal to `get_rx_freq()`, checked on the X310); the
code used `actual_rf + actual_dsp`, off by 2 × the DSP residual (−3.7 to
+8.5 kHz depending on the frequency). All users now share
`CaptureRecord::stream_center_hz()` (`rf − dsp`): this decoder, the native and
legacy DJI observers (`src/drones/addon.hpp`) and the ML worker's reported
centre (`src/ml_inference.cpp`); the coordinated derived-LoRa-channel provenance
(`src/scanner.cpp`) adjusts `actual_dsp_hz` with the matching sign.

Live, coordinated mode, 20 min each, receive-only:

| Run | Offset spread per address (median std) | Links | Proven wrong (old address heard again) |
|---|---|---|---|
| First version, `rf + dsp` centre | 5.7 kHz | 75 | 19 |
| Whole-packet offset fit, `rf + dsp` | 5.8 kHz | 63 | 12 |
| Corrected centre, 6 kHz tolerance | 2.0 kHz | 62 | 7 |

With the final 3 kHz tolerance, 42 of that run's 55 surviving links would still
be made. Proven-wrong links are a lower bound on false merges. In this busy
environment (~600 addresses per 20 min) about 1 in 5 addresses has a median
offset within 3 kHz of any given one, so false merges remain possible;
treat a merged row as "probably one device".

## Verification

Automated (76/76 suite passes):

- `shortrange_dsp` (46 checks): Bluetooth assigned-number lookups, the
  CRC-16/KERMIT check value, and a BLE round trip.
  - A BLE advert, a Zigbee beacon and a Zigbee data frame are modulated into a
    20 MSps capture with ±60–70 kHz carrier offset, noise and a CW interferer,
    then extracted, demodulated and parsed field by field.
  - It also covers noise-only rejection, Remote ID extraction, the coverage
    planner, device lists and coverage counters, JSON export, and the observer's
    queue and drop behaviour.
- `shortrange_panel` (14 checks): headless render of the page (lists,
  inspectors, mode warnings, start/stop) and the Drones Remote ID section.

Live, X310 at 192.168.10.2, receive-only, `build/shortrange_live_check`:

| Run | Captures decoded / dropped | BLE CRC-valid packets | BLE addresses | 802.15.4 |
|---|---|---|---|---|
| 2.4 GHz, 120 s, one-slot handoff (first version) | 57 / 55 | 3,987 | 198 | 0 syncs |
| Coordinated, 120 s | 56 / 0 (82 outside 2.4 GHz) | 3,092 (37: 216, 38: 2,203, 39: 673) | 148 | 0 syncs, 11–26 |
| 2.4 GHz, 75 s, final queue | 60 / 0 | 7,489 | 171 | 0 syncs |

Real adverts decoded with valid CRC include named earbuds ("Nothing Ear",
"OnePlus Nord Buds 3r", "Ion ANC Pro_BLE"), Apple Find My / Nearby / Handoff /
AirPlay, Google Fast Pair, Microsoft CDP, scan requests/responses, extended
advertising headers. Exports: `data/shortrange_validation/`.

## Not verified

- **Zigbee on real radios.** No 802.15.4 sync occurred on any channel in ~5 minutes
  of live runs, consistent with no Zigbee network nearby but not proof. The
  demodulator is validated only against its own synthetic modulator; a real
  Zigbee/Thread device (hub + bulb, ESP32-C6/H2, nRF52840 or CC2531) is needed.
- **Bluetooth Remote ID on air** (needs the ESP32 rig or a compliant drone).
- **Clicking through the page in the running GUI** (headless render only).
