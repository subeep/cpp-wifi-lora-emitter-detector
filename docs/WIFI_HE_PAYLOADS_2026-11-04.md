# 802.11ax HE SU payloads (stage 4) — 4 November 2026

Follows [stage 3](WIFI_HT_VHT_LDPC_2026-11-04.md). Stage 4 decodes the payload
of 802.11ax single-user PPDUs that a 20 MHz, single-receiver capture can
carry.

## Scope

**Decoded:** 802.11ax HE SU at 20 MHz (242-tone RU) with one spatial stream.
- Coding: BCC up to MCS 9, or LDPC up to MCS 11 (1024-QAM).
- 2× HE-LTF with a 0.8 or 1.6 µs guard interval (HE-SIG-A GI+LTF values 1
  and 2). This is what every HE SU frame seen here used.
- Packet extension handled, including the PE-disambiguity bit.

**Reported as header only, with the reason:**

| Reason | Why |
|---|---|
| 1× or 4× HE-LTF | Not seen live, so their sequences could not be derived and confirmed. |
| DCM, STBC, midambles (Doppler) | Not implemented. |
| 40 MHz and wider, two streams | X310 limits (see stage 3). |
| HE MU, HE TB, HE ER SU | Other formats. |

## How it works (`src/wifi_ofdm_rx.cpp`, `decode_he_payload`)

- **Timing:**
  - Order after the L-SIG: RL-SIG, HE-SIG-A (2 symbols), HE-STF (4 µs), one
    HE-LTF (6.4 µs plus the guard interval), then 256-point data symbols
    (12.8 µs plus the guard interval, 78.125 kHz tones).
  - The symbol count comes from L-LENGTH (m = 2 for SU), minus one when the
    PE-disambiguity bit is set.
- **Channel estimate:**
  - The 2× HE-LTF is read with a 128-point FFT, giving the even tones.
  - Odd tones are interpolated.
  - A factor of √2 accounts for the 2× LTF's tone power. On the cleanest live
    frames, equalised data power was exactly 2.00 without it.
  - Triangular smoothing over ±3 even tones applies up to 64-QAM. It is ±1 for
    256-QAM and off for 1024-QAM, where its bias on frequency-selective
    channels costs more than the noise it removes.
- **Phase tracking without pilot tables:** the 8 pilots are BPSK. Their
  squares give the common phase modulo π, and continuity from the HE-LTF
  resolves the ambiguity. A slowly tracked slope follows sampling-clock drift.
- **Demapping:**
  - Data tones are ±2..±122, minus pilots ±22, ±48, ±90, ±116, giving 234 tones.
  - Gray demapping is generalised up to 1024-QAM. One formula (the axis bits
    are the binary-reflected Gray code of the level index) replaces the
    hand-written 16/64/256-QAM tables. The existing tests confirm it matches
    them; for 1024-QAM it is the same rule, untested by any other source.
  - BCC uses a 26-column interleaver. LDPC uses the tone mapper, which puts
    point k on data tone 9·(k mod 26) + ⌊k/26⌋.
- **Padding (27.3.12):** the pre-FEC padding factor a sets how many coded bits
  the last symbol carries (a × 60 tones' worth, or all of it for a = 4). The
  rest is post-FEC padding and is dropped.
  - For LDPC, the extra symbol segment bit gives the initial a and symbol
    count. The codeword choice uses the initial available bits; puncturing and
    repetition use the final ones (`ldpc_plan_he()` in `src/wifi_ldpc.cpp`).
- **Payload:** A-MPDU de-aggregation and per-MPDU FCS, as in 802.11ac.

## The HE-LTF sequence

The 2× HE-LTF sequence for 20 MHz (123 values, tones −122..122 step 2) was
derived from the air, not typed from memory:

1. Collect live HE SU frames. 183 were harvested from 20 minutes of
   receive-only captures on channels 1, 6 and 11.
2. A real channel is smooth across neighbouring even tones (156 kHz apart).
   So for every neighbouring pair, the sign of Re(R_k · conj(R_k+2)) across
   frames votes on L_k · L_k+2. Chaining the 121 links gives the sequence up
   to one overall sign.
3. Two links had low agreement (−12/−10 at 49 %, 78/80 at 73 %). Together with
   the overall sign, that leaves 8 candidates. Every frame was decoded with
   each: only one candidate decoded any payload (40 FCS-valid PPDUs at the
   time), and the other seven decoded none.
4. **Independent check:** a search snippet of the TGax specification framework
   (IEEE 802.11-15/0132) lists the first 18 values with the minus signs lost
   in extraction. Its explicit "+1" entries fall exactly where the derived
   sequence has +1, and its bare "1" (stripped "−1") entries where it has −1.

## Verification

- **Harvested live HE SU frames:** 183 harvested, of which 165 were in scope
  with a complete capture.
  - **100 decoded, yielding 134 FCS-valid MPDUs:** 87 QoS data frames (both
    directions) and 47 Action No Ack management frames.
  - By source and rate:

    | Source | Coding | MCS | Decoded |
    |---|---|---|---|
    | Colour 0 (downlink) | BCC | 3 | 47 of 59 |
    | Colour 10 uplink client | LDPC | 0 | 15 of 20 |
    | | | 2 | 22 of 26 |
    | | | 3 | 5 of 8 |
    | | | 4 | 3 of 13 |
    | | | 5 | 5 of 23 |
    | | | 6 | 3 of 11 |
    | | | 7 | 0 of 5 |

- **Failures are signal-limited.** Median-EVM per frame shows a sharp
  threshold. Colour 10's MCS 5 frames decode at −18.4 to −20 dB and fail at
  −17.2 to −18.2 dB. MCS 6/7, which need a few dB more, sit around −18 dB.
  - The worst frames also carry narrowband interference, for example tones
    −115 to −108 (about −8.6 MHz, probably Bluetooth).
- **Live scanner (90 s, 2.4 GHz):** no HE traffic was active during the run;
  this client is bursty. The scanner calls the same `decode_ofdm_burst`.
- **Tests** (`test_wifi_ht_payload`, now 79 checks): synthetic HE SU PPDUs
  from a standard-based transmitter through a channel with CFO and noise.
  - Coverage:
    - every MCS (BCC 0–9, LDPC 0–11);
    - both guard intervals;
    - payload sizes covering a = 1, 2, 3, 4;
    - LDPC extra segments;
    - a packet extension with the PE-disambiguity bit;
    - DCM and 4× HE-LTF reported, not claimed.
  - The test transmitter carries a copy of the derived sequence. It guards the
    receiver against regressions; the live FCS results are what validate it.

## Two receiver fixes found on the way (all formats)

- **False preamble on very clean bursts.**
  - The cause: `baseband()` removes the burst mean (LO leakage). On a strong
    burst that mean comes from the signal itself, so the quiet lead-in becomes
    a near-constant. A constant repeats every 16 samples like the L-STF, so
    detection locked onto it at sample 0 and the frame was lost.
  - The fix: windows with a 1-sample correlation above 0.8 are now skipped.
    A real L-STF stays below that, while a constant (or DC plus a little
    noise) reaches it.
  - Effect on the stage 2 captures: +3, +4 and +1 FCS-valid PPDUs. A 0.5
    cutoff was tried and lost a quarter of all frames.
- **Truncated bursts.** Energy segmentation can end a weak or fading PPDU
  early. The decoded header states the PPDU length, so
  `decode_ofdm_with_context` now decodes again over that length when the
  capture holds it. This gave +1 PPDU on a 10 s channel-6 capture, and no
  truncated decodes remain.

Full suite 77/77.

## Not done

- 1× / 4× HE-LTF (needs live frames to derive and confirm their sequences).
- DCM, STBC and midambles.
- HE MU / TB / ER SU, two streams, and 40 MHz or wider.
