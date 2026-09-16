# newrocktest-cpp — Project Context

Single-file handoff covering what this project is, what it does today, how
every non-obvious decision was reached, and the one thing currently blocking
progress.

**Scope note:** this document describes ONLY the C++ application in
`newrocktest-cpp/`. A separate Python prototype exists elsewhere on this
machine; it is deliberately not referenced here. The `.py` files under
`tools/` belong to this project and are in scope — they drive the LoRa test
transmitter over serial, not the DSP.

---

## 1. What this is

A live RF emitter monitor: a background thread streams IQ from a USRP, detects
and classifies emitters across three bands, and renders them in a Dear ImGui
desktop UI.

- **C++17**, no GNU Radio, no VOLK, no FFTW. Dependencies are UHD, KissFFT,
  GLFW/OpenGL and a vendored Dear ImGui.
- Three bands, one active at a time: **Sub-GHz ISM** (LoRa / IN865),
  **2.4 GHz Wi-Fi**, **5 GHz Wi-Fi**.
- Each band keeps its own persistent device registry, so switching modes and
  back does not lose an accumulated list.

### Build and run

```bash
cd build && cmake .. && cmake --build . -j4
./rf_monitor_gui          # the app
./band_smoke_test         # manual hardware validation, ~4 min, all three bands
```

The GUI defaults to the **B210**; select **USRP X310** in the device selector,
then pick a band. `band_smoke_test` defaults to X310 (override: `./band_smoke_test b210`).

---

## 2. Hardware, exactly as configured

| Item | Value |
|---|---|
| Radio | USRP X310, serial `326CF02`, **HG** FPGA image, `addr=192.168.10.2` |
| Daughterboards | Two **UBX-160 v2** — Radio#0 (slot A), Radio#1 (slot B) |
| RX antenna ports | `TX/RX`, `RX2`, `CAL` on both; 10 MHz – 6 GHz; gain 0–31.5 dB |
| **Main RX** | **RF A / RX2** — `ANTENNA = "RX2"`, channel 0, no subdev spec set, so UHD defaults to slot A |
| Second antenna | RF B / TX-RX — intended for a TX loopback test rig, **not yet wired up in code** |
| Host link | `enp3s0`, Realtek RTL8125 (2.5GbE capable) negotiated at **1000 Mb/s**, **MTU 1500** |
| 10GbE | **None.** No 10G NIC in the machine |
| Also available | USRP B210 (`max_sample_rate_hz` 56e6) — not currently used |
| LoRa test TX | Melange TarangMini on `/dev/ttyUSB0` (FT232), currently **SF07 / BW125** |
| Wi-Fi adapter | Realtek RTL8188FTV — **2.4 GHz only**, and it is the host's internet connection |

### Transport ceiling — the single most important constraint

Both X310 RX channels mux over one SFP0 link at 1 GbE. At sc16 that is ~31 MS/s
absolute, ~25 MS/s sustained *in theory*. **The practical ceiling on this
machine is lower** — see §7.

Consequences already established:
- Wi-Fi captures at **20 Msps** (`DeviceProfile::max_sample_rate_hz` for X310).
- There is no configuration in which both antennas stream Wi-Fi usefully at once.
- Oversubscribing this link does **not** degrade gracefully — it starves the
  RFNoC control channel and aborts the process.

---

## 3. Source map

| File | Role |
|---|---|
| `src/config.hpp` | Bands, scan plans, all tuning constants, `DeviceProfile` per radio |
| `src/scanner.{hpp,cpp}` | Background scan thread; per-band loop; Wi-Fi + LoRa processing |
| `src/registry.{hpp,cpp}` | Device registry — permanence, bucket keys, fingerprint matching |
| `src/sdr_capture.{hpp,cpp}` | UHD wrapper. RX-only, single channel, channel 0 hardcoded |
| `src/spectrum.cpp`, `detector.cpp`, `classifier.cpp` | Generic energy detection and labelling, all bands |
| `src/wifi_phy.{hpp,cpp}` | Burst segmentation, DSSS/OFDM classification, occupied bandwidth |
| `src/wifi_frame.{hpp,cpp}` | 802.11 frame layer: FCS-32, PLCP CRC-16, descrambler, beacon/IE parsing |
| `src/wifi_dsss_rx.{hpp,cpp}` | 802.11b 1 Mbps DSSS receive chain: IQ → BSSID/SSID |
| `src/lora_phy.cpp`, `lora_phy_std.cpp` | Two LoRa codecs (own + SX127x-compatible) |
| `src/fingerprint.{hpp,cpp}` | LoRa RF fingerprint extraction |
| `src/lora_master.{hpp,cpp}` | Persistent cross-run LoRa device list |
| `src/main.cpp` | ImGui UI and all tables |
| `tools/band_smoke_test.cpp` | Manual hardware validation across all bands |
| `tools/tarangnet_api.py` | TarangMini serial control (config, flash, restart, TX burst) |
| `tools/tarangmini_sf_bw_sweep.py` | Sweeps the TarangMini through SF/BW combos and bursts |

### Test suite (all pure software, no hardware)

```
test_dsp  test_lora_phy  test_lora_phy_std  test_fingerprint
test_registry  test_lora_master
test_wifi_phy  test_wifi_stress  test_wifi_frame  test_wifi_dsss_rx
```

All 10 pass. Run them individually from `build/`.

---

## 4. LoRa side — complete, **do not modify**

Working and validated. Left alone deliberately; Wi-Fi work must not touch it.

- **Fingerprinting** (`fingerprint.cpp`): CFO ppm plus the IQ-imbalance / DC
  "stable core" via a widely-linear least-squares fit on the preamble.
- **Sub-bin CFO refinement**: `find_preamble()` only produces an integer FFT bin,
  and the residual (±0.5 bin) accumulates to `π × preamble_len` radians across a
  40-symbol preamble — ~20 uncompensated rotations, which destroyed EVM on every
  real capture. Fixed with a differential (Moose-style) estimator. Validated:
  0.000 ppm residual error vs 0.394 ppm naive.
- **Quality gates recalibrated from real data**, not the source spec:
  `LORA_EVM_CEILING_PCT = 75.0`, `LORA_SYNC_CORR_FLOOR = 0.65`. The spec's
  10%/0.95 rejected 100% of real TarangMini captures.
- **Persistent master list** (`data/lora_master/`): one NDJSON file per device,
  `MAX_READINGS_PER_DEVICE = 1000` with FIFO pruning, permanent `first_seen`
  preserved in a meta line that survives pruning, atomic rename on compaction.
  Path resolved from `PROJECT_ROOT_DIR` (a CMake compile definition) so a
  `build/` wipe cannot destroy it.

### Known-unresolved on the LoRa side

Real TarangMini captures plateau at **25–73% EVM** even with the correct
(SF, BW) hypothesis confirmed by reading the module's own config over serial.
Controlled A/B testing ruled out cross-hypothesis confusion, the boxcar
decimator, and CFO quantisation. The residual is believed to be a mismatch
between the idealised chirp reference and TarangMini's real transmitted signal
(PA non-linearity or a multipath-heavy environment) — consistent with this same
module's header/CRC never validating in this project either. The gates were
recalibrated to that measured reality rather than chasing it further.

Also unresolved: the master list fragmented **one physical TarangMini into 19
device entries**, because `irr_db`/`dc_dbc` turned out to be strongly
**SF-dependent** (a ~30 dB swing from SF5 to SF10) rather than device-dependent.
The matcher deliberately excludes `cfo_ppm` for a related reason.

---

## 5. Wi-Fi side — current state

### 5.1 The classifier was inverted, and is now fixed

Measured before any changes: an unmodulated carrier scored **3.17–4.20**
confidence while genuine OFDM scored **2.56**. Receiver self-noise outranked
real signal, and since the scanner keeps the highest-confidence result, LO
leakage won every time.

Six defects found and fixed:

1. **No DC removal before Schmidl-Cox.** The documented 40–50 dB LO spike at
   every tuned centre is perfectly self-similar at all lags, so it produced a
   sustained plateau. Now removed by subtracting the complex mean *before*
   mixing, while its position is known exactly.
2. **Unbounded plateau.** A carrier and a real L-STF produce *identical*
   plateaus — no threshold separates them. What does: a real L-STF is 10 short
   symbols so its plateau **stops**, while a carrier's runs for the whole
   buffer. `MAX_SC_PLATEAU_SYMBOLS = 20` rejects the latter.
3. **Spectral-span gate.** First attempt counted occupied bins and broke real
   OFDM — the L-STF is genuinely *sparse* (12 subcarriers spaced 4 apart, which
   is exactly what creates its 0.8 µs periodicity). The correct measure is how
   far the comb is **spread**, not how many bins it fills.
4. **Incomparable confidences.** Both branches returned unbounded run-length
   ratios on different scales. Now both return a mean normalised correlation in
   [0, 1].
5. **Bandwidth read the first 256 samples** of a 20-million-sample buffer,
   un-mixed, with the LO spike as the peak reference — the source of the
   "~0 MHz" labels. Now Welch-averaged across the whole burst with a Hann
   window and DC removed, and the FFT size **scales to the burst** (64–256 bins)
   rather than padding short bursts with idle air.
6. **Modulation rows lost their registry bucket** to bare energy segments,
   because the two `peak_db` values come from measurements ~50 dB apart. Fixed
   with an explicit `Detection::modulation_confirmed` priority.

Plus: the Barker template was mismatched at 20 Msps (1.818 samples/chip with
chip boundaries quantised to sample boundaries). Now resampled to an exact
`BARKER_SAMPLES_PER_CHIP = 2.0` grid, decoupling it from capture rate.

`SC_PLATEAU_THRESHOLD` was lowered 0.6 → **0.35**, extending OFDM sensitivity by
~5 dB. Safe only because carrier rejection now comes from the duration and
spectral gates rather than from that threshold.

### 5.2 Measured performance

`test_wifi_stress` uses **spec-faithful generators that share no formula with
the code under test** — a real 802.11a L-STF through a 64-point IFFT, and
pulse-shaped DSSS at 20 samples/chip.

```
SNR dB   OFDM correct/wrong/miss   DSSS correct/wrong/miss
30         40 /  0 /  0              40 /  0 /  0
15         40 /  0 /  0              40 /  0 /  0
10         40 /  0 /  0              40 /  0 /  0
 5          6 /  0 / 34              40 /  0 /  0
 0          0 /  0 / 40              39 /  0 /  1
```

Zero misclassifications at every SNR. Zero false positives across noise, DC
carrier, strong LO leakage, and CW interferers at +3/−5/+8 MHz.

### 5.3 Packet list and per-burst processing

`wifi::detect_bursts()` segments a capture into individual transmissions by
energy (block envelope, 25th-percentile noise floor). Classification then runs
**per burst** instead of per whole buffer. This made the packet list possible
*and* cut sweep time roughly 3× by keeping the correlators off 20 M samples of
idle air.

The GUI's **Detected Wi-Fi packets** table shows Time, Freq, Ch, Modulation,
Power, BW, Duration, Confidence — one row per burst, newest first.

`MIN_WIFI_BANDWIDTH_HZ = 4e6` rejects narrowband emitters. Nothing in Wi-Fi is
narrow; the 2.4 GHz band is full of Bluetooth (1 MHz), BLE and Zigbee (2 MHz),
and a Barker correlator has no inherent reason to reject them. Live captures had
been producing "DSSS" rows at 0.23–0.70 MHz.

Emitter labels follow **airtime**, not peak confidence — OFDM bursts
consistently outscore DSSS, so a peak-confidence pick made DSSS invisible even
on channels it dominated.

The registry now flushes **per channel hop** (Wi-Fi only) instead of per full
52 s sweep. Sub-GHz still accumulates to the cycle boundary, because its
wideband steps can each see the same emitter and per-step flushing would inflate
`hit_count`.

### 5.4 Multi-emitter support

A Wi-Fi channel routinely carries several networks — the local channel 9 has
6–7 BSSIDs. The registry key went from `(band, bucket_hz)` to
**`(band, bucket_hz, source_id)`**. An empty `source_id` reproduces the previous
one-device-per-bucket behaviour exactly, which is what keeps the LoRa/sub-GHz
path byte-identical (pinned by a test).

`wifi::find_beacon_sources()` infers a source count from **beacon cadence** —
every BSS beacons every 102.4 ms with an independent phase, giving ~100
distinguishable slots versus ~6–25 for carrier frequency offset, and needing no
new DSP since burst offsets are already known.

**This nearly shipped broken.** The first version passed every functional test
but produced **26 phantom sources per capture** from purely aperiodic traffic at
realistic density — 400 bursts saturate a 102.4 ms phase space at ~1 ms
resolution, so periodicity stops carrying information. Fixed by feeding it only
**beacon-plausible bursts** (DSSS, ≥ `WIFI_BEACON_MIN_DURATION_S = 1 ms`; a
1 Mbps beacon is ~2 ms, data frames are tens of µs) plus a straight-line
residual test and an 80% slot-occupancy requirement. Now zero phantoms from
10–80 candidates/s while still finding 4 real trains buried in 40 aperiodic
bursts.

Reported as `>=N` in the GUI, because it **under-counts and never over-counts**:
co-phased BSSes merge, and virtual/multi-BSSID networks sharing one radio are
physically identical here by construction.

> Note on the local RF environment: the six channel-9 BSSIDs are ~4 physical
> radios. `3C:52:A1:0B:BF:D7`/`5E:52:A1:0B:BF:D7` and the two
> `98:BA:5F:…`/`9E:BA:5F:…` pairs differ only in the locally-administered bit —
> virtual BSSIDs on one transmitter. Only a decoded BSSID can separate those.

### 5.5 Beacon decoder — built, validated, not yet usable live

`wifi_frame` + `wifi_dsss_rx` implement the full 802.11b 1 Mbps chain:
resample to chip grid → Barker despread with timing search → DBPSK
differential demod → descramble → SYNC/SFD → PLCP header (CRC-16) → PSDU →
beacon parse (FCS-32) → **BSSID, SSID, channel**.

Measured end to end against an independently-written transmitter:

```
SNR  20.0 dB -> 20/20 decoded, 0 WRONG
SNR  10.0 dB -> 20/20 decoded, 0 WRONG
SNR   0.0 dB -> 20/20 decoded, 0 WRONG
SNR  -3.0 dB -> 20/20 decoded, 0 WRONG
```

Zero wrong decodes at any SNR; pure noise fabricates nothing; CFO tolerated to
100 kHz. **Precision is not the open question — yield is.** The FCS is CRC-32
with HD=4 out to 2974-bit datawords, so for a ~1856-bit beacon every 1-, 2- and
3-bit error is caught and random garbage passes at 2⁻³² (2.3e-10).

Findings worth preserving:

- **The PLCP CRC-16 parameterisation** was solved against the spec's worked
  example (SIGNAL `0x0A`, SERVICE `0x00`, LENGTH 192 → **`0x5B57`**), not
  assumed. The answer is unusual: bits fed **LSB-first** into a **non-reflected
  MSB-first** register, init `0xFFFF`, **ones-complemented**. Two neighbouring
  variants look equally plausible and are silently wrong — reflecting the output
  instead of complementing gives `0x1525`, and MSB-first bits give `0x05C0`.
  Both decode synthetic frames happily and reject **100% of real frames**.
- **Descramble before searching for anything.** The scrambler covers every
  transmitted bit including SYNC, SFD and the PLCP header.
- **The SFD search must not key off the first zero bit.** `0xF3A0` begins with
  four ones and flows straight out of the all-ones SYNC field.
- **BSSID is Address 3, not Address 2** — they differ on multi-BSSID radios,
  which is precisely the case this feature exists to resolve.
- **Channel comes from the DS Parameter Set IE**, not the tuned channel. At
  20 Msps a capture centred on channel 9 spans channel 11's centre, so
  adjacent-channel beacons *will* decode here; attributing them to the tuned
  channel would invent co-channel emitters.

It is wired into `scanner.cpp` and keys the registry by BSSID. It will start
producing named APs the moment the capture rate can reach 22 Msps.

---

## 6. Conventions and hard-won lessons

**Never let a test share a formula with the code under test.** The original
DSSS test generated its waveform with the correlator's own zero-order-hold chip
formula, so it structurally could not fail — and it was hiding a completely
broken DSSS path (1/40 detection at 30 dB SNR against a properly pulse-shaped
signal). The reference prototype consulted during this work had the same defect
at a larger scale: its tests round-tripped through its own standards tables, two
of which are provably wrong. **Assert against published vectors.**

**Verify before fixing.** The LoRa EVM investigation burned three wrong
hypotheses (CFO drift, fit-window length, decimation artifacts) before
controlled A/B testing found the truth. Every subsequent problem here was
reproduced in software first — the inverted classifier, the phantom beacon
sources, the phantom-source density curve.

**Measure, then set thresholds.** Gate values that came from an abstract
specification (LoRa 10%/0.95, OFDM 0.6) all had to be replaced with values
derived from measured hardware behaviour.

**Prefer duplicating a small helper over refactoring validated code.** Several
files intentionally carry their own copy of `base_upchirp`, `decimate_boxcar`
and `nearest_channel`.

**Comments record *why*, especially for non-obvious hardware findings.** Much of
what is in this document is also inline at the relevant code.

---

## 7. ⚠️ CURRENT BLOCKER

**Live 1 Mbps beacon decoding is blocked by transport capacity, not by code.**

The decoder is complete and proven in software. It cannot run on live captures
because:

- Barker despreading needs **≥ 2 samples per 11 Mchip/s chip**.
- The X310 currently captures Wi-Fi at **20 Msps → 1.818 samples/chip**, below
  the floor. The decoder recovers nothing at that rate.
- Raising `DeviceProfile::max_sample_rate_hz` to **22.222 Msps**
  (200 MHz master clock / decim 9 = 2.02 samples/chip) was tried. On paper it
  fits: ~89 MB/s against a 1 GbE link's ~100 MB/s.

**It does not fit in practice.** Measured on this machine it starved the RFNoC
control channel within seconds:

```
[ERROR] [RFNOC::GRAPH] Timed out getting recv buff for management transaction
UsrpCapture::capture: RfnocError: OpTimeout: ... (treating as a dropped capture)
terminate called after throwing an instance of 'uhd::op_timeout'
```

…and aborted the process. **This has been reverted**; the link is confirmed
healthy at 20 Msps (`band_smoke_test` exits 0, no overflows). The revert and the
reasoning are recorded inline in `config.hpp`.

### Candidate routes out, roughly in order of effort

1. **`net.core.wmem_max` is still at the 1 MB default.** UHD explicitly requests
   `2426666` on every single run. Needs `sudo`, so it has to be done by hand:
   ```bash
   sudo sysctl -w net.core.wmem_max=2426666
   ```
   Cheapest thing to try, and it may be part of why the headroom was not there.
2. **Jumbo frames.** The path is MTU 1500, costing ~4.5% framing overhead and
   ruling out the usual X310 throughput tuning. Needs NIC and switch support.
3. **Move Wi-Fi to the B210** — 56 Msps over USB3, 2.8× the X310's ceiling here.
   Enough for full 20 MHz channels with margin *and* for DSSS decode. Costs the
   X310's second antenna and better front end. This is a one-line profile change
   plus a device-selector choice.
4. **10 GbE NIC + XG image.** The only route to genuinely more X310 bandwidth;
   the second SFP port is 10G-only under the current HG image. Hardware purchase.

### What unblocks immediately once the rate reaches ~22 Msps

Nothing further needs writing. The decoder is wired in, keyed to BSSID, and the
registry already supports multiple named emitters per channel. Decoded APs
should begin appearing in **Active emitters** as `WiFi AP <bssid> "<ssid>"`.

---

## 8. Other outstanding items

- **Second antenna (RF B / TX-RX) is unused.** The intent is a TX loopback rig
  for controlled classifier validation. `UsrpCapture` is RX-only — this needs a
  TX streamer. Use a **cable plus 30–40 dB attenuator**, not over-the-air:
  radiating risks compressing or damaging the RX front end and puts 802.11-shaped
  energy into a live band.
- **5 GHz is unvalidated** for everything in §5. Beacons there are 6 Mbps OFDM
  and need an entirely different chain (64-point FFT, channel estimation,
  deinterleaving, Viterbi) — roughly triple the effort of the DSSS path.
- **No ground-truth validation of the classifier against real traffic.** The
  local Wi-Fi adapter is 2.4 GHz only *and* is the host's internet connection,
  so monitor mode would drop connectivity. A second dual-band adapter
  (**MT7612U** recommended — mainline `mt76x2u` driver, reliable monitor mode)
  would allow a real confusion matrix. `nmcli dev wifi list` gives a
  zero-privilege AP inventory that validates detection but not modulation.
- **`sdr_capture.cpp` discards `uhd::tune_result_t`** and never calls
  `get_rx_freq()`; the scanner passes the *requested* centre frequency. Probably
  zero error under UHD's AUTO policy, but it becomes load-bearing the moment
  per-burst CFO is measured.
- **X310 connects fail intermittently** (~2 in 5) with an RFNoC graph error;
  `connect_sdr()` retries 6 times to compensate.
- **Power values are on inconsistent scales** between the energy path
  (un-normalised |FFT|², ~+30 dB) and the Wi-Fi path (time-domain mean, ~−4 dB).
  Harmless today because `modulation_confirmed` decides bucket ownership, but
  the displayed numbers are not comparable between row types.
