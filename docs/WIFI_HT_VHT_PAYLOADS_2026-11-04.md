# 802.11n/ac payload decoding (stage 2) — 4 November 2026

Follows [stage 1](WIFI_HT_VHT_HE_HEADERS_2026-11-04.md) (HT-SIG / VHT-SIG-A /
HE-SIG-A headers). Stage 2 decodes the payload of 802.11n and 802.11ac PPDUs
that a 20 MHz, single-receiver capture can carry, so those frames become real
MAC frames (addresses, frame type, FCS) instead of anonymous RF readings.

## Scope

Decoded: **802.11n (HT) and 802.11ac (VHT), 20 MHz, one spatial stream, BCC**,
MCS 0–7 (HT) / 0–8 (VHT, up to 256-QAM 3/4), long or short GI, with or without
**STBC** (two space-time streams, Alamouti, combined with one receive antenna).
A-MPDUs are split into subframes and every MPDU is checked against its own FCS.

Not decoded (the header is still reported, with the reason):

| Reason shown | Why |
|---|---|
| `40 MHz; 20 MHz only`, `80 MHz …`, `160 MHz …` | The X310 on 1 GbE captures 20 MSps. |
| `2 spatial streams; one only` | Needs two receive channels. |
| ~~`LDPC coding not supported`~~ | Decoded since stage 3. |
| `802.11ax payloads not supported` | Stage 4. |
| `multi-user`, `extension LTFs`, `no data (sounding)`, `MCS 9 is not defined for one stream at 20 MHz` | Out of scope / not a data PPDU. |

## How it works (`src/wifi_ofdm_rx.cpp`)

After a CRC-valid header in scope:

1. **Channel estimate** from the HT-LTF / VHT-LTF on 56 subcarriers (the L-LTF
   estimate is not reused: cyclic shifts and spatial mapping differ in the HT
   portion). With STBC, two LTFs give both stream channels (P = [1 −1; 1 1]).
2. **Symbol count**: 802.11n from the HT length; 802.11ac from the L-SIG
   length (PPDU duration), including the short-GI disambiguation bit.
3. **Per symbol**: FFT window 3 samples into the guard interval (8-sample short
   GI included); common phase from the four pilots (802.11n per-stream pilot
   patterns, 802.11ac single-stream pattern, polarity from symbol 3 / 4);
   sampling-clock drift tracked as a slowly updated phase slope.
4. **Equalise** (or Alamouti-combine pairs of symbols), **demap** with max-log
   soft bits (BPSK to 256-QAM, Gray labels separable in I and Q), weighted by
   channel power; **deinterleave** (13 columns); **depuncture** (2/3, 3/4, 5/6).
5. **Viterbi** (now one decision bit per state and step, renormalised; a 5 ms
   PPDU needs a few MB) — 802.11n terminated after the PSDU, 802.11ac after the
   PHY padding; **descramble**; SERVICE check.
6. **MPDUs**: a non-aggregated 802.11n PSDU is one MPDU; otherwise the A-MPDU
   is walked delimiter by delimiter (signature 0x4E, CRC-8, 12/14-bit length),
   resynchronising after a damaged delimiter, and each MPDU's FCS is checked.

Result fields (`OfdmDecodeResult`): `mpdus` (every FCS-valid MPDU; legacy
frames now fill it too), `mpdu` (the first), `fcs_valid`, `ampdu_subframes`,
`data_rate_mbps`, and `rate_mbps` (the rounded HT/VHT rate). The status line
reads e.g. `…header CRC OK; payload decoded (19.5 Mbps), A-MPDU 2/3 MPDUs FCS-valid`.

`HighThroughputSig` also gained `space_time_streams`, `extension_ltfs` and
`nsym_disambiguation`. **Correction to stage 1:** `streams` is now the number
of spatial streams; with STBC it was previously reported as the space-time
count (an STBC uplink frame showed "2 streams"; it is 1).

## Where it shows

The scanner already routes every FCS-valid OFDM frame, so decoded 802.11n/ac
frames now get a frame type in **Wi-Fi → Packets**, feed the security monitor
and the master list's MAC enrolment, and are no longer enrolled as anonymous
RF readings. The Packets tab note counts `… (payload decoded)` per format; the
capability page is updated. One event per PPDU is submitted (the first
FCS-valid MPDU of an A-MPDU), matching how legacy bursts are counted; the other
subframes are in `mpdus` but not submitted.

## Verification

**Live traffic, offline** (X310, 5 s per channel, tuned on-channel, production
burst pipeline). Every counted MPDU passed its FCS-32:

| Channel | In-scope PPDUs decoded / failed | MPDUs | FCS-valid OFDM PPDUs, stage 1 → 2 |
|---|---|---|---|
| 1 (2412) | 62 / 8 (802.11n, incl. STBC and A-MPDU) | 62 | 153 → 215 |
| 6 (2437) | 50 / 40 (802.11ac 256-QAM STBC 44 / 45) | 53 | 1170 → 1220 |
| 11 (2462) | 34 / 5 | 47 | 139 → 173 |

All decoded MPDUs were QoS data between clients and APs. CPU cost: +1–3 %
single-threaded on the same captures.

Why frames fail (measured, not assumed):
- **Weak stations at high MCS.** Channel 6 failures were one uplink client
  (partial AID 51) sending 256-QAM at EVM −16 to −24 dB; on channel 1, 64-QAM
  5/6 frames at −44 dB had EVM −13 to −18 dB (needed: about −25 dB). Frames
  that decode are typically −29 dB or stronger.
- **Collisions.** On long MCS 0 A-MPDUs (1.5 ms) per-symbol EVM shows bursts of
  another transmitter (power ×4 for tens of symbols).

Tried and dropped (no gain or harmful): clamping soft values (+1 frame), LS
channel smoothing (breaks STBC from beamforming APs: 50 → 8), erasing the
band-edge subcarriers that the scanner's 1.5 MHz tuning offset pushes past
Nyquist at 20 MSps (24 → 22), FFT backoff 1–5 samples and slope gain 0–0.3
(flat).

**Live, production `Scanner`** (`build/wifi_phy_formats_check`, receive-only):
- 2.4 GHz, 90 s: 39 802.11n PPDUs decoded (run 1). In run 2, 5 were decoded
  and 13 failed; the traffic had shifted to 40 MHz (104 PPDUs), LDPC (87) and
  two-stream (31), all out of scope.
- 5 GHz, 60 s: all traffic is 80/160 MHz, so nothing is in scope. Headers and
  network attribution are unchanged.

**Tests**: `test_wifi_ht_payload` (29 checks) builds PPDUs with a transmitter
written from the standard. The PPDUs go through a two-tap channel with a 23 kHz
CFO and noise. Coverage:
- every MCS in both formats;
- short GI with A-MPDU;
- STBC with and without short GI;
- the 802.11ac N_SYM disambiguation case;
- a damaged subframe (2/3 kept);
- LDPC and 40 MHz not claimed;
- truncation;
- heavy noise never yielding a wrong FCS-valid MPDU.

Three injected receiver bugs (wrong 802.11ac pilot polarity, wrong Alamouti
sign, swapped 256-QAM labels) each fail it. The live captures are client
traffic, so they are not committed as fixtures. A fixture from our own
transmitter (the Heltec ESP32-S3 sending fixed-MCS HT20 frames) would add one;
that needs a reflash. Full suite 77/77.

## Not done

- LDPC: done in stage 3, see
  [WIFI_HT_VHT_LDPC_2026-11-04.md](WIFI_HT_VHT_LDPC_2026-11-04.md). Stage 3
  also showed the 1.5 MHz tuning offset is not what limits live decoding.
- 802.11ax HE SU payloads (stage 4).
- 40 MHz and wider, and two spatial streams. These need a wider or second
  receive channel: the B210 at 56 MSps for 40 MHz, or a 10 GbE X310 link.
- Using subframes other than the first for security events.
