# Wi-Fi RF fingerprinting, phase A: whole-frame features and evaluation — 4 November 2026

Goal of the plan: bind each transmitter address to physical-layer evidence,
so a frame claiming to come from a MAC can be checked against that MAC's
radio, catching evil twins, MAC clones and forged deauthentication.

Phase A builds the measurement foundation and measures, on live traffic,
what the features can separate. Nothing here raises alerts yet; that is
phase B.

## Why the old features were not enough

The existing extractor (`wifi_fingerprint.cpp`) works on the 8 µs preamble.

- **Per-device features on stored data** (705 profiles, 111 MACs): the CFO
  separated 62–67 % of device pairs. I/Q imbalance and DC offset separated
  **0 %**, because their per-device spread (IRR 2.5 dB, phase 2.7°)
  swamped any difference between devices.
- **Stored references go stale.**
  - The X310 shifts every CFO by up to ±0.6 ppm between sessions.
  - Devices themselves drift up to 0.5 ppm (90th percentile) to 1.6 ppm
    (99th) across about a day.
  - Meanwhile 13 % of radio pairs sit within 1 ppm of each other.
- **Per-frame preamble CFO is too noisy for single-frame verdicts:** 0.3–0.6
  ppm per frame on typical transmitters.

## What was built

**`src/wifi_rf_features.{hpp,cpp}`:** data-aided fits on FCS-verified frames.

- **OFDM:** for each symbol, the common phase and the phase slope across
  tones, measured against the known transmitted points. Across symbols:
  - **CFO**, over the whole frame, and an **early CFO** over the same stretch
    of every frame (training field plus the first 16 symbols).
  - **SFO**: sampling-clock offset, signed so it equals the CFO for
    one-crystal radios.
  - **Phase jitter**, and **SNR**.
  - **Transmitter I/Q image**: error regressed on the mirror-tone point.
  - **Carrier leakage**: the DC tone.
- **The training field anchors the phase line for short frames:** it defines
  zero phase, which lengthens their baseline.
- **DSSS:** the despread phase followed symbol by symbol from SYNC to the last
  PSDU bit, using polarities from the verified bits.

**Receiver hooks (`wifi_ofdm_rx.cpp`, `wifi_dsss_rx.cpp`):** once the FCS
passes, the decoded bits are re-encoded into the exact transmitted points
(scrambler seed, tail placement, BCC or LDPC, puncturing, interleaver or tone
mapper, constellation). The result is `OfdmDecodeResult::rf` /
`DsssDecodeResult::rf`.

| Frames | Covered |
|---|---|
| Legacy OFDM | Yes |
| 802.11n/ac, BCC and LDPC, without STBC | Yes; an A-MPDU only when every subframe passed and LDPC parity holds |
| DSSS | Yes |
| 802.11ax, STBC | Not yet |

**`tools/wifi_rf_auth_eval`:** the evaluation harness, receive-only on IQ
captures.

- **Per-frame precision:** each frame's deviation from its transmitter's
  median, by method, length and SNR.
- **Verification trials:**
  - Each frame is compared with its claimed sender's other frames within ±5 s,
    a contemporaneous reference, so receiver and device drift cancel.
  - **Genuine:** the frame's own transmitter.
  - **Impostor:** a frame from another radio relabelled as the claimed sender,
    which is what a forged frame looks like. Virtual BSSIDs (same last five
    octets) are never paired.
- **Detection** at 10 %, 1 % and 0.1 % false-alarm rates, per feature and
  combined. `--csv` writes every frame for further analysis.

## Results

**Dataset:** 11 channels × 6 s, receive-only, tuned as the scanner tunes
(1.5 MHz offset). 2.4 GHz channels 1, 6, 11; 5 GHz channels 36, 40, 44, 48,
149, 153, 157, 161. **12,745 verified frames and 145,636 impostor trials.**

**Precision of the early CFO** (90th-percentile deviation):

| Frames | p90 | Gate |
|---|---|---|
| OFDM < 20 symbols (control frames, deauth-length) | 0.053 ppm | ≤ 0.2 ppm: **passes** |
| OFDM 20–99 symbols | 0.026 ppm | |
| OFDM ≥ 100 symbols | 0.020 ppm | |
| OFDM ≥ 20 symbols, SNR ≥ 15 dB | 0.022 ppm | ≤ 0.1 ppm: **passes** |
| Beacons | 0.020 ppm | |
| DSSS (86 frames on the first four captures) | 0.013 ppm | |

**Verification** (detection rate at each false-alarm rate):

| Feature | at 10 % | at 1 % | at 0.1 % |
|---|---|---|---|
| **CFO, early (whole frame)** | 97.4 % | **95.4 %** | **94.9 %** |
| CFO, preamble only (before phase A) | 94.0 % | 91.2 % | 87.1 % |
| CFO, early, same length class | 97.2 % | 95.1 % | 94.6 % |
| Received power | 57.3 % | 27.3 % | 15.7 % |
| Early CFO + power | 96.5 % | 95.0 % | 93.4 % |
| LO leakage | 34.9 % | 11.3 % | 4.6 % |
| SFO (per frame) | 23.6 % | 10.9 % | 3.8 % |
| I/Q image | 24.1 % | 5.8 % | 1.2 % |
| Phase jitter | 21.1 % | 5.3 % | 0.6 % |
| Frequency settling (frames ≥ 40 symbols) | 10.1 % | 1.3 % | 0.7 % |

At a 0.1 % false-alarm rate, missed impostors fell from 12.9 % to 5.1 %.
CPU cost: +2–3 % single-threaded on the same captures (9.16 → 9.40 s for
5 s of the busiest channel).

### What the misses are

**Clients share their AP's carrier.**
- Of 368 transmitter pairs heard together, 14 sit within 4 spreads of each
  other. Nearly all are an AP and one of its own clients. For example
  94:bb:43:19:26:8c sits within 0.005–0.028 ppm of the Flo Mobility AP in two
  captures, and e0:d4:64:50:a4:d3 within 0.1 ppm of Avgarde_airtel.
- Clients estimate their AP's carrier offset and pre-correct their transmit
  frequency to it (802.11ax uplink requires this). So a CFO match cannot tell
  an AP from its own clients, and an attacker who joins the network may
  inherit the AP's CFO.

**The sampling clock separates them.**
- Pre-correction moves only the carrier; the sampling clock stays on the
  client's own crystal.
  - **APs:** SFO equals CFO (beacon-heavy APs agree within 0.02–0.1 ppm).
  - **Clients:** they don't. 94:bb:43:19:26:8c shows −13.8 ppm SFO with the
    AP's −8.95 ppm CFO.
- But per-frame SFO from short control frames is unreliable: the same AP
  gave −4.8 to −9.9 ppm across captures. It needs a better timing estimator
  (or long frames) before it can count as evidence.

### Other findings

- **Short and long frames differ slightly.** OFDM frames carry a small
  device-specific offset between short (< 20 symbols) and long frames:
  −0.005 to −0.12 ppm. It is most likely a phase step between the training
  field and the data, which the anchor reads as frequency. The early CFO
  reduces it; comparing within the same length class cancels it. Phase B
  should build references per length class.
- **The fit's own CFO uncertainty is optimistic.** Real deviations are 3–60×
  the formal σ (short-term device frequency variation dominates). Scores
  therefore use each reference's measured spread, not the per-frame σ.
- **Power is a weak, location-dependent feature here.** It helped in a small
  sample (79 % at 1 %) and fell to 27 % on the full set; adding it to CFO
  does not help.
- **I/Q image, carrier leakage, phase jitter and settling are fitted
  correctly** on synthetic signals (recovered within 1.5 dB / 0.1 ppm), but on
  real devices they do not separate radios at useful false-alarm rates yet.

## Tests

- **`test_wifi_rf_features` (12 checks):** synthetic OFDM with known CFO,
  SFO (sign included), carrier leakage, I/Q image and SNR; a short frame using
  the anchor; DSSS CFO.
- **`test_wifi_ht_payload` (+3, now 82):**
  - the 23 kHz offset of the synthetic channel is recovered on 802.11n/ac BCC
    and LDPC;
  - features are not claimed for STBC;
  - features are not fitted when an A-MPDU subframe failed.
- **`test_wifi_ofdm` (+3):** features on the real beacon fixtures, close to
  the preamble estimate.
- **`test_wifi_dsss_rx` (+1):** the DSSS deauth built with a 10 kHz offset
  comes back at 9999.5 Hz over 426 symbols.
- Full suite 79/79.

## What this means for phase B

- **Score each frame's early CFO** against the claimed sender's own frames of
  the same length class from the last few seconds. That gives about 95 %
  detection of a different radio at a 1 % (and 0.1 %) per-frame false-alarm
  rate, with no enrolment or calibration.
- **Aggregate before alerting:** an incident should need several
  inconsistent frames. Forged-deauth floods provide dozens.
- **"One identity, two transmitters"** uses the same CFO precision.
- **State the limit:** a CFO match means "this AP or a client that
  pre-corrects to it". Separating those needs the SFO estimator improved
  first.

## Not done

- 802.11ax and STBC frames carry no features yet.
- SFO from short frames.
- Phase D's controlled attacks: an own-hardware evil twin, forged deauth to
  our own MACs, and an SDR deliberately matching CFO.
- Day-long false-alarm runs.
