# Wi-Fi RF identity rules (RF fingerprinting, phase B) — 4–5 November 2026

Phase A ([WIFI_RF_FINGERPRINT_PHASE_A_2026-11-04.md](WIFI_RF_FINGERPRINT_PHASE_A_2026-11-04.md))
measured each verified frame's carrier offset precisely enough to tell
radios apart. Phase B turns that into two passive security rules in the
Wi-Fi security monitor.

- **4 November:** built and validated offline on the 11 saved captures (rule
  version 1).
- **5 November:** checked live on the X310, receive-only. Live traffic showed
  three things the 6 s captures had not, and rule version 2 handles them (see
  "Live findings").

## How a frame is judged

- **Feature:** each FCS-verified frame's early CFO from phase A, converted to
  ppm of the channel frequency (`attach_rf_features`). It travels on the
  `FrameEvent` (`rf_method`, `rf_symbols`, `rf_cfo_ppm`, `rf_cfo_early_ppm`,
  `rf_sfo_ppm`, `rf_snr_db`) and is recorded in the monitor's NDJSON.
- **Frames used:** management and data frames only. Control frames (RTS,
  block acks, NDP announcements) are left out.
- **Reference:** the claimed sender's own non-disconnect frames on the same
  channel within ±10 s, from the same length class (DSSS, OFDM < 20 symbols,
  OFDM ≥ 20 symbols). This needs no enrolment, and receiver and device drift
  cancel.
  - When that window has fewer than 5 frames, the reference falls back to
    ±60 s.
  - With fewer than 5 even then, the frame counts as **unverifiable**, never
    as inconsistent.
  - When the same length class has too few frames, other classes are used,
    with 0.15 ppm extra tolerance for the short/long offset found in phase A.
- **Roles:**
  - An address is an **AP** if it sends frames as its own BSSID.
  - It is a **client** if it sends data frames to the DS.
  - A disconnect frame shows its own claimed role: from the BSSID it claims
    the AP, otherwise a client.
- **Inconsistent** when both hold:
  - |frame − reference median| ≥ 8 × the reference's robust spread
    (floor 0.02 ppm);
  - |frame − reference median| ≥ 0.25 ppm when the AP is the claimed sender,
    or ≥ 2 ppm when a client is.
- **Time:** host time, or device time when host time is missing. Frames with
  neither are skipped.
- **Degraded captures** (overflow, timeout, burst cap, queue loss) are
  excluded and never judged. Loss or restart clears all references.

## The two rules

### `rf_forged_disconnect` (severity High)

- **What it flags:** deauthentication or disassociation frames whose carrier
  offset doesn't match their claimed sender's.
- **When it reports:** at least 3 inconsistent frames, which must also be at
  least half of the disconnect frames evaluated within 10 s.
- **Confidence:** High with ≥ 10 inconsistent frames, ≥ 80 % share and a
  ≥ 1 ppm difference; Medium otherwise.
- **Evidence and measures:**
  - the last 8 inconsistent frames;
  - inconsistent and evaluated counts, reference size, reference and suspect
    CFO, the difference, and the reference spread;
  - the claimed role (`context.claimed_role`).
- **Benign explanations given:** a repeater cloning the sender's MAC, or a
  second radio of the same device. For a client claim, also a pre-correction
  step larger than 2 ppm.

### `rf_identity_two_transmitters` (severity Medium)

- **What it flags:** one **AP** address whose recent frames form two CFO
  clusters that interleave in time: an evil twin or AP clone.
- **Not run for:** addresses also seen as clients (repeaters pre-correct their
  uplink), or addresses of unknown role.
- **Method:**
  - Split the frames at the largest gap that leaves at least 5 frames on
    each side.
  - The gap must be at least 0.3 ppm and at least 8 × the wider cluster's
    spread.
  - The clusters must alternate at least twice in time order. A single step
    change (retune, temperature, a replaced device) is not reported.
- **Length classes:** each class is checked first. Then all classes are
  pooled, with the 0.15 ppm allowance added to the gap. That pooling catches
  a clone sending other frame lengths than the owner.
- **Confidence:** High with ≥ 10 frames in each cluster and ≥ 4 alternations.

Both rules go through `IncidentStore`, so they coalesce, carry evidence, and
close after 60 s of quiet analysed air like the other rules.
- Configuration string: `rf-identity-v2 mgmt+data w10 fb60 … client2.00 … ap-only`.
- Incident `rule_version`: 2.

## Live findings (5 November, X310, receive-only)

**Runs:** recorded with `coordinated_live_check --security-record`.
- 60 s and 5 min on channel 36;
- 5 min on channel 6;
- 5 min sweeping both bands.

In total about 185,000 accepted frames.

**1. Clients re-adjust their carrier in steps.**
- **First live run:** rule version 1 raised "two transmitters" on two
  addresses within a minute. Both were clients of one AP.
  - Their carrier jumped back and forth: for example −9.6 → −8.6 → −9.7 →
    −8.9 ppm.
  - Power, rates and receiver stayed the same, so it was one radio.
  - This fits clients pre-correcting to their AP and updating that correction.
- **APs do not do this.**

| Role (no control frames) | Genuine frames judged | Inconsistent at 0.25 ppm | Deviation p99.9 | Largest |
|---|---|---|---|---|
| APs (38) | 5,634 | 0 | 0.17 ppm | — |
| Clients (40) | 5,048 | 128 (2.5 %) | 1.04 ppm | 1.10 ppm |

- **Hence:** a 2 ppm tolerance for client claims (about twice the largest
  step), and the two-transmitter rule for APs only.

**2. An AP's RTS frames are unreliable.** One AP's RTS bursts (6 Mbps, 8
symbols) sat 0.3–0.8 ppm off its own beacons, giving 23 inconsistent frames.
- Short control frames are often sent as non-HT duplicates over a wide
  channel, and an 8-symbol fit has little to go on.
- Leaving control frames out removed every AP inconsistency.
- The cost: control frames are most of the traffic (ch36: 91,617 → 3,412
  frames used), so references are thinner.

**3. AP carriers hold over minutes.**
- Even against the AP's own frames from 60–120 s earlier, the deviation p99.9
  is at most 0.26 ppm. About 1 frame in 8,000 counts as inconsistent.
- That justifies the 60 s fallback.
- The sweep returns to a channel only about every 91 s, though, so in
  practice references come from the current visit (several 1 s captures).

## Results (rule version 2)

Measured with `wifi_rf_auth_eval`, section 4.
- **Attacks are simulated by relabelling:** frames of another radio heard on
  the same channel are given the victim's address.
  - A forged frame claims the victim's role: from its BSSID for an AP, to its
    AP for a client.
  - Each victim is paired with up to 6 other radios.
- **Judged per 1 s capture**, as the monitor does.
- **Data folders** (gitignored): `data/rf_fingerprint/2026-11-04/` (IQ captures,
  `rules_v2.txt`) and `data/rf_fingerprint/2026-11-05/` (live recordings,
  `rules_live.txt`).

**False alarms: none anywhere.**
- Genuine traffic: 0 two-transmitter findings across all 4 datasets.
- Senders' own frames relabelled as disconnects: 0 of 35 flagged.
- Replaying all four live recordings through the full monitor
  (`wifi_security_replay`): 0 RF incidents.
- 3 genuine deauths were judged consistent.

**Detection:**

| Dataset | Victims | Forged disconnect bursts | … radios ≥ 1 ppm apart | AP clones |
|---|---|---|---|---|
| Live ch36, fixed | APs | 12 of 12 | 12 of 12 | 12 of 12 |
| | clients | 45 of 48 (94 %) | 45 of 46 | — |
| Live ch6, fixed | APs | 24 of 24 | 23 of 23 | 24 of 24 |
| | clients | 42 of 48 (88 %) | 42 of 44 | — |
| Live sweep, both bands | APs | 89 of 114 (78 %) | 84 of 102 | 106 of 114 (93 %) |
| | clients | 42 of 99 (42 %) | 42 of 84 | — |
| 11 IQ captures, 6 s | APs | 26 of 31 (84 %) | 26 of 28 | 28 of 31 (90 %) |
| | clients | 24 of 42 (57 %) | 24 of 37 | — |

**What's missed:**
- **Fixed channel:** radios within about 1.3 ppm of a client victim (client
  tolerance), and radios within 0.4 ppm of an AP.
- **Sweep and short captures:**
  - Most misses are **unverifiable**: the claimed sender had fewer than 5 of
    its own management or data frames in that visit, so there is no
    reference. These are quiet senders, mostly clients.
  - A busy AP being impersonated is covered.
  - Fixed-channel monitoring of the network to protect gives the best
    coverage.

## Live attack validation (5 November, test #3)

The first test on a genuine transmitted attack, not relabelled traffic.

- **Monitor:** X310 receive-only, fixed on channel 13 (`coordinated_live_check
  --channel 13 --security-record`).
- **Attacker:** a Heltec ESP32-S3 (`tools/esp32_deauth_attacker/`, a bounded
  deauth source — manual arm, 60-frame cap, low power), spoofing a phone
  hotspot we own ("Redmi 9", BSSID `fa:fa:99:c6:4b:c1`) to broadcast.
- The ESP32 is a different radio from the phone, so its forged deauths carry a
  different carrier offset — exactly the cross-radio forgery the rule targets.

**Result: `rf_forged_disconnect` raised, severity High, confidence High.**

| | CFO | Frames |
|---|---|---|
| Genuine Redmi beacons (reference) | −2.376 ppm (spread 0.02) | 58 |
| Forged deauths (from the ESP32) | −0.515 ppm | 35 of 35 flagged |

- Carrier-offset gap **1.86 ppm**, far above the 0.25 ppm AP-claim threshold.
- The incident named the impersonated AP (`fa:fa:99:c6:4b:c1`) and broadcast
  target; all 35 received deauths were judged inconsistent.
- Recording: `data/rf_fingerprint/2026-11-05/deauth_run.ndjson`.

Two things learned operating live:
- `esp_wifi_80211_tx` refuses raw deauth frames unless
  `ieee80211_raw_frame_sanity_check` is overridden (done in the attacker
  firmware for this authorized own-AP test).
- Sweep mode only captured channels 1–10; channels 11–13 need a fixed tune.
  The target hotspot was on channel 13, so a sweep would have missed it —
  a coverage gap worth fixing for live monitoring.

### Evil twin (test, same day)

The second rule, on a genuine two-radio setup.

- **Radio 1:** the real "Redmi 9" hotspot beaconing as `fa:fa:99:c6:4b:c1`.
- **Radio 2:** the Heltec (`tools/esp32_evil_twin/`) running a low-power softAP
  whose BSSID is set to the same `fa:fa:99:c6:4b:c1`, on channel 13 — a real
  evil twin. Both beaconed at once for ~22 s while the X310 recorded.

**Result: `rf_identity_two_transmitters` raised, Medium/Medium**, naming
`fa:fa:99:c6:4b:c1`: two carrier offsets under one BSSID, −2.215 ppm (the
phone) and −0.116 ppm (the ESP), **gap 2.10 ppm**, 3 time-order alternations
(both active over the same window, not a step change). Recording:
`data/rf_fingerprint/2026-11-05/eviltwin.ndjson`. The clone beaconed far more
often than the phone (≈135 vs 14 decoded), yet both clusters cleared the
5-frame minimum.

### SDR CFO-match (blind-spot boundary, same day)

How close must an attacker's carrier offset be to the AP's before
`rf_forged_disconnect` stops flagging it?

- **Live attempt:** a HackRF One transmitting a generated 802.11a deauth
  (`tools/wifi_tx_gen.cpp`, a legacy-OFDM modulator self-validated against our
  own receiver). The live decode did not land: at low TX power the X310 could
  not decode the HackRF, and at full power the HackRF — inches from the X310 —
  saturated the receiver. A clean over-the-air decode needs physical
  attenuation/separation between the two radios. (High-power deauth transmission
  from the agent is also refused by the safety tooling, correctly.)
- **Measured data-driven instead:** the real genuine beacons and real forged
  deauths from the test above, with the attacker's CFO recentered to
  `AP + delta` (keeping its real jitter) and judged with the exact rule
  thresholds. The model reproduces the real incident (35/35 inconsistent at the
  measured 1.85 ppm gap).

| Attacker CFO gap from the AP | Verdict |
|---|---|
| ≤ 0.20 ppm | missed |
| 0.22–0.24 ppm | partial |
| ≥ 0.26 ppm | detected (35/35) |

**Boundary ≈ 0.25 ppm** (≈ 600 Hz at 2.4 GHz) — the `min_delta_ppm` floor, with
a tight 0.02 ppm reference. An SDR that matches the AP's offset to within about
0.25 ppm evades this rule; beyond that it is flagged. This quantifies the known
limit and is the same reason a client pre-correcting to its AP (naturally inside
this window) cannot be separated. Data and sweep notes:
`data/rf_fingerprint/2026-11-05/cfo_match_README.md`.

## What it means in practice

- **Forged deauth / disassoc floods claiming an AP are flagged** on a fixed
  channel, from any radio whose carrier differs by ≥ 0.4 ppm. That is the
  standard attack.
- **Floods claiming a client are flagged** when the attacker's radio is ≥ 2 ppm
  away, which is most radios.
- **An evil twin of an AP transmitting at the same time as the real AP is
  flagged** (100 % of the live fixed-channel cases at ≥ 0.5 ppm).
- **Not covered:**
  - A cloned client MAC (clients step legitimately).
  - Senders too quiet to have a reference.
  - 802.11ax / STBC frames (no features yet).
- **A match is not proof.** A match means "this sender or a client that
  pre-corrects to it". An attacker associated to the AP, or an SDR that
  deliberately matches the CFO, passes. Separating those needs the SFO
  estimator improved (later phases).

## Files

| File | Change |
|---|---|
| `src/security/wifi_security_rf_identity.{hpp,cpp}` (new) | `RfIdentityTracker` (pure: observations in, findings out; roles, fallback reference), `RfIdentityRule` (per-capture adapter, incidents; control frames skipped). |
| `src/security/wifi_frame_event.{hpp,cpp}`, `wifi_security_io.cpp` | RF fields on `FrameEvent`, `attach_rf_features`, NDJSON round-trip. |
| `src/scanner.cpp`, `tools/wifi_security_replay.cpp` | Both scanner event paths and the IQ replay attach the features. |
| `src/security/wifi_security_state.{hpp,cpp}`, `wifi_security_monitor.cpp` | Rule wired in (frames, capture finish, interrupts); counters in the snapshot and its JSON (`rf_identity`). |
| `src/security_gui.cpp` | Security page, rule diagnostics: an "RF identity" line with frames, disconnects judged / inconsistent / unverifiable, excluded and tracked senders. |
| `tools/wifi_rf_auth_eval.cpp` | Section 4: the rules with simulated forgeries and clones, by victim role, judged per capture. Also reads monitor recordings (`.ndjson`). |
| `tools/coordinated_live_check.cpp` | `--channel N`, `--security-record PATH`; RF identity counters and incidents in its JSON summary. |
| `tests/test_wifi_rf_identity.cpp` (new) | 27 checks. |

**`test_wifi_rf_identity` checks:**
- **Tracker:**
  - genuine disconnects; a forged burst; fewer than 3 inconsistent frames, or
    a minority;
  - no reference; a stale reference; the 60 s fallback;
  - the short/long allowance;
  - interleaved clusters; a clone in another length class; a step change; a
    noisy single radio; the short/long offset not split;
  - a client stepping 1 ppm (no finding); client claims at 1.1 ppm (passes) and
    3 ppm (flagged); AP claims at 0.6 ppm (flagged); unknown role (no cluster
    rule).
- **Monitor path through `SecurityState`:** genuine deauths; a forged burst
  raising the incident with evidence; a degraded capture excluded; the rule
  disabled; control frames not used.

## Not done

- **Day-long false-alarm runs.** The longest live run here was 5 minutes per
  mode.
- **Real attacks against our own devices** (needs approval to transmit):
  - forged deauths — **done 5 November** (test #3 above: detected, High/High);
  - an own-hardware evil twin — **done 5 November** (test above: detected, Medium/Medium);
  - an SDR deliberately matching an AP's CFO — **boundary measured 5 November** (≈ 0.25 ppm; live HackRF decode needs RF attenuation, not yet done).
- **No CFO features yet** for 802.11ax and STBC frames, so the rule cannot
  judge them. On busy 5 GHz channels, that is most data frames.
- **SFO from short frames**, needed to separate an AP from a client that
  pre-corrects to it, and to cover client clones.
