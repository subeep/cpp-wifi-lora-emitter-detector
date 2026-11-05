# 802.11n/ac/ax header decoding (stage 1) — 4 November 2026

## Why

The OFDM receiver decoded only legacy 802.11a/g payloads. 802.11n (HT), 802.11ac
(VHT) and 802.11ax (HE) PPDUs start with a legacy-compatible L-SIG, so they were
"decoded" as 6 Mbps legacy frames that always failed the FCS and became
anonymous RF readings (the main source of `WIFI-FP-*` clusters in the master
list). Stage 1 decodes the header that follows the L-SIG, so these PPDUs are
recognised and, where possible, tied to a network. Payloads are still not
decoded.

## What is decoded

After a valid 6 Mbps L-SIG, the next symbols are compared with the L-SIG on every
data subcarrier (the channel cancels):

| Format | Detection | Header decoded | Network identifier |
|---|---|---|---|
| 802.11ax HE | RL-SIG repeats L-SIG; L-LENGTH mod 3 ≠ 0 | HE-SIG-A (2 BPSK symbols, 52 subcarriers, 13×4 interleaver, CRC-4): SU, TB or MU; MCS, DCM, width, GI/LTF, streams, coding, STBC, spatial reuse, TXOP, uplink/downlink | **BSS colour** (6 bits), matched to the HE Operation element of decoded beacons |
| 802.11n HT | symbol 1 QBPSK | HT-SIG (2 QBPSK symbols, CRC-8): MCS, 20/40 MHz, PSDU length, aggregation (A-MPDU), STBC, LDPC, short GI | none in the header |
| 802.11ac VHT | symbol 1 BPSK, symbol 2 QBPSK | VHT-SIG-A (CRC-8): width, STBC, group ID, streams, **partial AID**, short GI, LDPC, MCS | group 0 (to an AP): partial AID = BSSID bits 39..47, matched to decoded beacons |

Conventions were settled on live traffic, not assumed. On real frames only one
variant passed: QBPSK bit 1 = +j; CRC-8 x⁸+x²+x+1 preset to ones, complemented,
sent c7 first; HE-SIG-A carries c7..c4 of the same CRC. Edge tones ±27/±28 of
HE-SIG-A are treated as erasures (no assumption about their training values).

A CRC-valid header ends the burst's decode with status "<header summary>;
payload not decoded (<reason>)" (stage 2 decodes 802.11n/ac payloads in scope). A header whose CRC fails falls back
to the legacy payload attempt exactly as before, so legacy frames that happen to
look like a new format are never lost.

## Where it shows

- **Wi-Fi → Packets:** the Decode column shows the header (e.g. "802.11ax HE SU |
  MCS 8 | 160 MHz | 2 streams | LDPC | BSS colour 12 | downlink | header CRC OK").
  When a header points to a network decoded this session, the SSID column shows
  it as a hint ("~ BSS colour 10 -> Airtel_Zerotouch (92:f0:4c:34:99:b9)"); the
  packet details give the format and hint. A hint lists every matching BSSID
  (virtual BSSIDs of one radio share colour and partial AID).
- **Packets tab note:** OFDM PPDUs this session by format, including headers whose
  CRC failed.
- **Beacons:** BSS colour (and "disabled") from the HE Operation element is kept
  in the identity and persisted in the master list.
- **Capabilities page:** updated Wi-Fi PHY entry.

## Verification

Prototype on live captures (5 GHz ch 36, 1 s; 2.4 GHz ch 1/6/11, 2 s each), then
the C++ receiver through the production burst pipeline:

| Capture | 802.11ac VHT | 802.11ax HE | 802.11n HT |
|---|---|---|---|
| 5 GHz ch 36 | 427 / 427 CRC OK | 31 / 31 | 5 false detections, all CRC-rejected |
| 2.4 GHz ch 1, 6, 11 | 1, CRC-rejected | — | 16 / 16 |

The partial-AID formula was checked against stored beacons: partial AID 150 seen
on 6 uplink frames matches exactly one known router, Flo Mobility upstairs
(`98:ba:5f:37:57:4b`).

Live, production `Scanner`, X310, receive-only
(`build/wifi_phy_formats_check`, 90 s per band):

| Band | Legacy | VHT OK / CRC failed | HE OK / CRC failed | HT OK / CRC failed | Attributed to a network |
|---|---|---|---|---|---|
| 5 GHz | 7,642 | 813 / 20 | 884 SU + 7 TB / 2 | 11 / 133 | 43 VHT uplink rows: Flo Mobility upstairs (partial AID 150 and 398) |
| 2.4 GHz | 1,010 | 42 / 5 | 25 / 0 | 315 / 12 | all 25 HE rows: Airtel_Zerotouch (BSS colour 10) |

The 5 GHz 802.11ax traffic (colour 12, 160 MHz) was not attributed: that router's
primary channel is most likely a DFS channel (52–64) that the 5 GHz plan does not
scan, so its beacons are never decoded. 802.11n headers carry no network
identifier. The 133 failed HT detections at 5 GHz are other formats tripping the
QBPSK test; their CRC rejects them and they fall back to the legacy attempt.

Tests (`wifi_ofdm`, 56 checks): seven live header fixtures (2 HT, 2 VHT, 1 VHT
uplink with partial AID 150, 2 HE SU with colour 12), each cut to preamble +
header only (780 samples, no payload), checked field by field; payload never
claimed; corrupted IQ never yields a different CRC-valid header; partial-AID
formula; BSS colour (and disabled flag) from a beacon's HE Operation element.
Full suite 76/76.

## Not done (later stages)

- Payloads: stage 2 (HT/VHT 20 MHz, one spatial stream, BCC) is done, see
  [WIFI_HT_VHT_PAYLOADS_2026-11-04.md](WIFI_HT_VHT_PAYLOADS_2026-11-04.md),
  which also corrects `streams` for STBC frames. Still open: stage 3 (LDPC) and
  stage 4 (HE SU). Most 5 GHz traffic here is
  80/160 MHz and 2-stream, which a 20 MSps / single-channel X310 on 1 GbE cannot
  decode; its headers can.
- Using the attribution in the master list (tying anonymous clusters to a
  network) — part of the duplication plan, measured with the offline replay tool.
- HE ER SU and HE MU-specific fields beyond colour, width and direction.
