# 802.11n/ac LDPC payloads (stage 3) — 4 November 2026

Follows [stage 2](WIFI_HT_VHT_PAYLOADS_2026-11-04.md) (BCC payloads). Stage 3
adds LDPC, the coding most 802.11ac traffic at 2.4 GHz here uses, within the
same scope: 20 MHz, one spatial stream, with or without STBC and short GI.

## What was added

**`src/wifi_ldpc.{hpp,cpp}`** (new):
- **Parity-check matrices:** the 12 base matrices of the standard: 648, 1296
  and 1944-bit codewords at rates 1/2, 2/3, 3/4 and 5/6.
  - They are taken from two independent public transcriptions:
    - `simgunz/802.11n-ldpc`: dense prototype files;
    - `syndrome` (MIT): edge lists, cross-checked by its author against the
      802.11n-2009 Annex R text.
  - The two were diffed programmatically and all 12 match. Every shift is
    below Z, and every matrix has the dual-diagonal parity structure.
- **`ldpc_plan()`:** the PPDU encoding parameters (19.3.11.7.5): codeword count
  and length, shortening, puncturing, repetition, and the rule for an extra
  OFDM symbol. For 802.11ac the extra symbol is read from VHT-SIG-A
  (`ldpc_extra_symbol`, B3 of SIG-A2) instead of re-derived.
- **`ldpc_decode()`:** layered normalised min-sum (scale 0.75, at most 30
  iterations).
  - It works on edge-to-bit index tables built once per code.
  - It gives up on a codeword when its count of unmet checks has not improved
    for 5 iterations.
  - Shortened bits are known zeros, punctured parity bits are erasures, and
    repeated bits are added to the bits they copy.
- **`ldpc_encode()`:** dual-diagonal encoder; it checks every parity equation
  of its output. Used by the tests.

**`src/wifi_ofdm_rx.cpp`:**
- **Coded-bit order:** no BCC interleaver. 802.11n uses subcarrier order;
  802.11ac uses the LDPC tone mapper, which puts constellation point k on data
  subcarrier 4·(k mod 13) + ⌊k/13⌋.
- **Symbol count:** 802.11n derives it from the HT length through the
  encoding parameters. 802.11ac takes it from the L-SIG duration, with the
  payload filling the symbols before any extra one.
- **No tail bits.** A failed decode reports how many codewords had unmet
  parity (`…; LDPC parity unmet in 2/5 codewords`).
- **The device MCS 9 case:** 802.11ac MCS 9 at 20 MHz with one stream is not
  defined by the standard, because it has 346⅔ data bits per symbol. A device
  here sends it anyway, with LDPC, and it was 279 of the 802.11ac PPDUs on
  channel 6. Three roundings were tried against FCS:

  | Data bits per symbol | PPDUs passing FCS |
  |---|---|
  | 346 (truncated) | 272 of 279 |
  | 347 | none |
  | 346⅔ (exact) | none |
  | rounded up | none |

  So it is decoded with 346, LDPC only. With BCC, MCS 9 is still reported as
  not defined.

## Verification

**Live traffic, offline** (the stage 2 captures, 5 s per channel, production
burst pipeline, every MPDU FCS-checked):

| Channel | PPDUs with an FCS-valid MPDU (stage 2 → 3) | FCS-valid MPDUs (stage 2 → 3) | LDPC PPDUs decoded / failed |
|---|---|---|---|
| 6 (2437) | 50 → 417 | 53 → 4,763 | 367 / 126 |
| 1, 11 | unchanged (no LDPC traffic in those captures) | | |

- **802.11ac LDPC by MCS:**
  - MCS 5: 15 of 17
  - MCS 7 short GI: 6 of 8
  - MCS 8 STBC short GI: 3 of 4
  - MCS 9 STBC short GI: 272 of 279
- **Failures:** almost all failed LDPC PPDUs (MCS 7 and 8, no STBC) come from
  one uplink client (partial AID 51). Its frames decode at the same power
  about half the time. Its EVM was measured at −16 to −24 dB in stage 2,
  marginal for 64/256-QAM.
- **Decoder settings:** scale 0.65–1.0 and up to 60 iterations decode no more.

**CPU** (single thread, channel 6 capture, the busiest):

| Version | Processing time for 5 s of capture |
|---|---|
| Stage 2 | 5.3 s |
| First LDPC version | 15.3 s |
| With edge index tables and a table-driven demapper | 11.2 s |
| With the stall stop | 8.9 s (loses 4 of 1,591 PPDUs) |

On channels without LDPC the cost is unchanged. The scanner spreads bursts
over its worker threads; live runs showed 0 producer waits.

**Live, production `Scanner`** (`build/wifi_phy_formats_check`, receive-only,
2.4 GHz, 60–90 s runs):
- 802.11ac LDPC and 802.11n payloads decode live.
- In-scope PPDUs succeeded 30–45 % of the time, depending on which stations
  were active. Most 802.11n/ac traffic in those minutes was 40 MHz or
  two-stream, which is out of scope.

**Capture width is not the limit.** Alternating runs at 20 and 25 MSps (the
new `--rate` option) gave the same success ratios: 45 % and 30 % at 20 MSps,
33 % and 38 % at 25 MSps. The 1.5 MHz tuning offset, which at 20 MSps pushes
the channel edge past Nyquist, is not what limits live decoding. This settles
the open question from stage 2.

**X310 on this 1 GbE link:**
- 25 MSps (16-bit) streams cleanly.
- 33.3 MSps overflows.
- The 8-bit wire format cannot be used: UHD 4.1 here has no `sc8` → host
  converter.

40 MHz channels need at least 40 MSps, so they need 10 GbE or the B210.

**Tests** (`test_wifi_ht_payload`, now 54 checks):
- LDPC at every MCS in both formats (802.11ac MCS 9 included), over three
  payload sizes each. Between them they exercise puncturing (42 cases),
  repetition (12), extra symbols (12) and multi-codeword PPDUs (36).
- LDPC with STBC and with short GI.
- Codec checks: 120 noisy round trips over all codes and rates, and unmet
  parity reported at very low SNR.

Full suite 77/77.

## Not done

- 40 MHz and wider: needs ≥ 40 MSps (see above).
- Two spatial streams: needs two receive channels.
- 802.11ax HE SU payloads: done in stage 4, see [WIFI_HE_PAYLOADS_2026-11-04.md](WIFI_HE_PAYLOADS_2026-11-04.md).
