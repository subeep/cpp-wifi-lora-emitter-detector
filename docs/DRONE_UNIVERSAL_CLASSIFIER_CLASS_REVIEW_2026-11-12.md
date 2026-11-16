# Universal classifier: A2 inventory and A3 class review

Date: 2026-11-12. **A3 reviewed by the user ("continue"); no training has started.**

## Inventory

The manifest lists 7,050 recording entries, including 127 locked DRFF-R2 entries marked unreadable. Counts include excluded entries; they are not training counts.

| Source | Entries |
|---|---:|
| DRFF-R2 | 726 |
| DroneRFb-DIR | 4,690 |
| FPV_Analog_Video | 48 |
| KULeuven_DroneRF | 883 |
| Own_X310 | 326 |
| RFUAV | 358 |
| Tampere_DroneRF | 19 |

Metadata comes from existing audited metadata (checked against source size and modification time), current archive directories, and recorder sidecars. No IQ was opened. Locked files were only listed/stat-ed; the guard rejects direct paths, symlink aliases, hardlink aliases and locked member names before opening.

FPV entries stay excluded and their receiver/rate/centre remain unknown. DroneRFa is still an incomplete download, so it has a separate pending entry. Live checks on 12 November did not retain IQ and cannot supply training recordings. Derived windows are excluded from the original-recording inventory.

## Proposed classes

Seven signal types are retained. `analogue_video` and `other_drone` currently have no admitted candidate model. Type assignments below are proposals, not per-window signal truth. B2 still has to measure presence; controller-only captures must not inherit the aircraft OFDM type without that evidence.

**Reporting proposal:** use `multi_aircraft` only where at least three explicitly identified, non-locked aircraft exist. Use `single_unit` as a conservative unit-match reporting tier for the rest. This includes Mini 3 Pro (two known aircraft), and models shared across datasets whose physical independence is unverified. This is a clarification of the plan’s two flags and needs your review; it does not claim those classes contain exactly one aircraft.

| Model | Proposed type | Reporting tier | Known aircraft / unresolved groups | Candidate entries | Sources |
|---|---|---|---:|---:|---|
| Air 2 | dji_ofdm | multi_aircraft | 4 / 0 | 74 | DRFF-R2 |
| Air 2S | dji_ofdm | multi_aircraft | 7 / 0 | 814 | DRFF-R2, DroneRFb-DIR |
| Air 3S | dji_ofdm | single_unit | 1 / 0 | 32 | Own_X310 |
| Avata 2 | dji_ofdm | single_unit | 0 / 1 | 44 | RFUAV |
| DEVENTION DEVO | hopping_controller | single_unit | 0 / 1 | 6 | RFUAV |
| DroneRFb A (code 84) | dji_ofdm | multi_aircraft | 3 / 0 | 916 | DroneRFb-DIR |
| DroneRFb C (code 88) | dji_ofdm | multi_aircraft | 3 / 0 | 806 | DroneRFb-DIR |
| DroneRFb D (code 93) | dji_ofdm | multi_aircraft | 3 / 0 | 809 | DroneRFb-DIR |
| DroneRFb F (code 90) | dji_ofdm | multi_aircraft | 3 / 0 | 675 | DroneRFb-DIR |
| FLYSKY EL 18 | hopping_controller | single_unit | 0 / 1 | 7 | RFUAV |
| FLYSKY FS I6X | hopping_controller | single_unit | 0 / 1 | 6 | RFUAV |
| FPV | dji_ofdm | single_unit | 0 / 1 | 38 | RFUAV |
| FUTABA T10J | hopping_controller | single_unit | 0 / 1 | 7 | RFUAV |
| FUTABA T14SG | hopping_controller | single_unit | 0 / 1 | 8 | RFUAV |
| FUTABA T16IZ | hopping_controller | single_unit | 0 / 1 | 7 | RFUAV |
| FUTABA T18SZ | hopping_controller | single_unit | 0 / 1 | 7 | RFUAV |
| Herelink Hx4 | hopping_controller | single_unit | 0 / 1 | 8 | RFUAV |
| Inspire 2 | lightbridge | single_unit | 0 / 2 | 145 | KULeuven_DroneRF, Tampere_DroneRF |
| JR PROPO XG14 | hopping_controller | single_unit | 0 / 1 | 7 | RFUAV |
| JR PROPO XG7 | hopping_controller | single_unit | 0 / 1 | 7 | RFUAV |
| JUMPER T14 | hopping_controller | single_unit | 0 / 1 | 7 | RFUAV |
| JUMPER TProV2 | hopping_controller | single_unit | 0 / 1 | 7 | RFUAV |
| Matrice 100 | lightbridge | single_unit | 0 / 1 | 1 | Tampere_DroneRF |
| Matrice 210 | lightbridge | single_unit | 0 / 1 | 3 | Tampere_DroneRF |
| Matrice 300 | dji_ofdm | single_unit | 0 / 1 | 142 | KULeuven_DroneRF |
| Mavic 3 | dji_ofdm | single_unit | 1 / 0 | 43 | DRFF-R2 |
| Mavic 3 Pro | dji_ofdm | single_unit | 0 / 1 | 18 | RFUAV |
| Mavic Mini | wifi_drone | single_unit | 0 / 1 | 1 | Tampere_DroneRF |
| Mavic Pro | dji_ofdm | single_unit | 0 / 1 | 1 | Tampere_DroneRF |
| Mini 2 | dji_ofdm | single_unit | 0 / 1 | 142 | KULeuven_DroneRF |
| Mini 3 | dji_ofdm | multi_aircraft | 3 / 1 | 689 | DroneRFb-DIR, RFUAV |
| Mini 3 Pro | dji_ofdm | single_unit | 2 / 0 | 46 | DRFF-R2 |
| Mini 4 Pro | dji_ofdm | multi_aircraft | 4 / 1 | 77 | DRFF-R2, RFUAV |
| Mini 5 Pro | dji_ofdm | single_unit | 1 / 0 | 43 | DRFF-R2 |
| Nine Eagles | hopping_controller | single_unit | 0 / 1 | 51 | KULeuven_DroneRF |
| Parrot Disco | wifi_drone | single_unit | 0 / 1 | 1 | Tampere_DroneRF |
| Parrot Mambo | wifi_drone | single_unit | 0 / 1 | 2 | Tampere_DroneRF |
| Phantom 4 | lightbridge | single_unit | 0 / 1 | 1 | Tampere_DroneRF |
| Phantom 4 Pro | lightbridge | single_unit | 1 / 0 | 7 | Own_X310 |
| Phantom 4 Pro+ | lightbridge | single_unit | 0 / 1 | 3 | Tampere_DroneRF |
| Q205 | hopping_controller | single_unit | 0 / 1 | 51 | KULeuven_DroneRF |
| RadioMaster TX16S | hopping_controller | single_unit | 0 / 1 | 7 | RFUAV |
| Radiolink AT10 II | hopping_controller | single_unit | 0 / 1 | 7 | RFUAV |
| Radiolink AT9S Pro | hopping_controller | single_unit | 0 / 1 | 8 | RFUAV |
| SIYI FT24 | hopping_controller | single_unit | 0 / 1 | 7 | RFUAV |
| SIYI MK15 | hopping_controller | single_unit | 0 / 1 | 8 | RFUAV |
| SIYI MK32 | hopping_controller | single_unit | 0 / 1 | 6 | RFUAV |
| SJRC F11 Pro | wifi_drone | single_unit | 0 / 1 | 51 | KULeuven_DroneRF |
| SKYDROID H12 | hopping_controller | single_unit | 0 / 1 | 8 | RFUAV |
| SKYDROID T10 | hopping_controller | single_unit | 0 / 1 | 9 | RFUAV |
| Spektrum DX4e | hopping_controller | single_unit | 0 / 1 | 71 | KULeuven_DroneRF |
| Taranis Q X7 | hopping_controller | single_unit | 0 / 1 | 51 | KULeuven_DroneRF |
| WFLY ET10 | hopping_controller | single_unit | 0 / 1 | 9 | RFUAV |
| WFLY ET16S | hopping_controller | single_unit | 0 / 1 | 8 | RFUAV |
| WFLY WFT09SII | hopping_controller | single_unit | 0 / 1 | 8 | RFUAV |
| WLtoys | hopping_controller | single_unit | 0 / 1 | 51 | KULeuven_DroneRF |
| YUNZHUO H12 | hopping_controller | single_unit | 0 / 1 | 4 | RFUAV |
| YUNZHUO H16 | hopping_controller | single_unit | 0 / 1 | 4 | RFUAV |
| YUNZHUO H30 | hopping_controller | single_unit | 0 / 1 | 4 | RFUAV |
| Yuneec Typhoon H | wifi_drone | single_unit | 0 / 1 | 3 | Tampere_DroneRF |
| not_a_drone | not_a_drone | background | 0 / 0 | 316 | DRFF-R2, DroneRFb-DIR, Own_X310 |

## Reasons for each assignment

- **Air 2 → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Air 2S → dji_ofdm:** Measured 15 kHz family in DIR probes and decoded product codes. Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Air 3S → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Avata 2 → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **DEVENTION DEVO → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **DroneRFb A (code 84) → dji_ofdm:** Measured 15 kHz family in DIR probes and decoded product codes.
- **DroneRFb C (code 88) → dji_ofdm:** Measured 15 kHz family in DIR probes and decoded product codes.
- **DroneRFb D (code 93) → dji_ofdm:** Measured 15 kHz family in DIR probes and decoded product codes.
- **DroneRFb F (code 90) → dji_ofdm:** Measured 15 kHz family in DIR probes and decoded product codes.
- **FLYSKY EL 18 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **FLYSKY FS I6X → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **FPV → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **FUTABA T10J → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **FUTABA T14SG → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **FUTABA T16IZ → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **FUTABA T18SZ → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **Herelink Hx4 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **Inspire 2 → lightbridge:** Plan section 4 Lightbridge assignment; verify presence at 32.8 kHz.
- **JR PROPO XG14 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **JR PROPO XG7 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **JUMPER T14 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **JUMPER TProV2 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **Matrice 100 → lightbridge:** Plan section 4 Lightbridge assignment; verify presence at 32.8 kHz.
- **Matrice 210 → lightbridge:** Plan section 4 Lightbridge assignment; verify presence at 32.8 kHz.
- **Matrice 300 → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Mavic 3 → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Mavic 3 Pro → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Mavic Mini → wifi_drone:** Plan section 4 candidate Wi-Fi assignment; requires measurement (especially Autel).
- **Mavic Pro → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Mini 2 → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Mini 3 → dji_ofdm:** Measured 15 kHz family in DIR probes and decoded product codes. Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Mini 3 Pro → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Mini 4 Pro → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Mini 5 Pro → dji_ofdm:** Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.
- **Nine Eagles → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **Parrot Disco → wifi_drone:** Plan section 4 candidate Wi-Fi assignment; requires measurement (especially Autel).
- **Parrot Mambo → wifi_drone:** Plan section 4 candidate Wi-Fi assignment; requires measurement (especially Autel).
- **Phantom 4 → lightbridge:** Plan section 4 Lightbridge assignment; verify presence at 32.8 kHz.
- **Phantom 4 Pro → lightbridge:** Plan section 4 Lightbridge assignment; verify presence at 32.8 kHz.
- **Phantom 4 Pro+ → lightbridge:** Plan section 4 Lightbridge assignment; verify presence at 32.8 kHz.
- **Q205 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **RadioMaster TX16S → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **Radiolink AT10 II → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **Radiolink AT9S Pro → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **SIYI FT24 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **SIYI MK15 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **SIYI MK32 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **SJRC F11 Pro → wifi_drone:** Plan section 4 candidate Wi-Fi assignment; requires measurement (especially Autel).
- **SKYDROID H12 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **SKYDROID T10 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **Spektrum DX4e → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **Taranis Q X7 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **WFLY ET10 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **WFLY ET16S → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **WFLY WFT09SII → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **WLtoys → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **YUNZHUO H12 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **YUNZHUO H16 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **YUNZHUO H30 → hopping_controller:** Controller equipment label; proposed hopping type needs burst/timing measurement.
- **Yuneec Typhoon H → wifi_drone:** Plan section 4 candidate Wi-Fi assignment; requires measurement (especially Autel).
- **not_a_drone → not_a_drone:** Background context only; scene approval and per-window review still required.

## Exclusions and decisions needed

- RFUAV archive/XML label conflicts are excluded; no filename is silently preferred over the XML.
- Nine Mini 3 IQ members have a pack basename that differs from their folder’s XML. Their folder metadata is recorded, but they are excluded pending verification.
- The 60 Leuven Mavic entries are excluded from the draft class list: their exact model is not established by the local README. Autel/DAUTEL alias entries are also excluded pending verification.
- The 71 extracted Leuven files remain listed but are excluded as possible duplicates of archived recordings.
- Mixed DRFF recordings are excluded from clean model candidates. DRFF environment files remain absence-unverified background candidates.
- X310 scene approval is incomplete. The 10 November background session cannot yet be treated as approved negative truth; the pilot session is unlabelled. Existing October sessions also need scene review.
- Aircraft from different datasets must not be counted as independent merely because the dataset names differ.
- DroneRFb E is proposed as Mini 3 (code 87), G as Air 2S (66); A/C/D/F retain anonymous code classes. Sources: plan sections 3–4 and the local DroneID report.

The user approved the proposed class/type assignments, exclusions, and conservative meaning of `single_unit` on 12 November 2026. A4 must then preserve whole sessions and prior partitions; a session must never be split to manufacture independent training and evaluation background.

## Reproduction

```bash
python3 -m tools.drone_ml.universal_inventory --output /tmp/universal-inventory.json
python3 -m tools.drone_ml.universal_classes --inventory /tmp/universal-inventory.json --output /tmp/universal-classes.json --review /tmp/universal-review.md
python3 -m unittest tests/test_drone_ml_universal_inventory.py
```
