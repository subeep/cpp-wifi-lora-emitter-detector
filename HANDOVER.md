# Handover — newrocktest-cpp

## Update — persistent Wi-Fi identities (2026-09-16)

The first two follow-up milestones are implemented: FCS-valid Wi-Fi identities
persist independently of fingerprint acceptance, and packet/master GUI tables
show identity metadata with a selectable details view. Includes offline IEEE
vendor lookup, advertised WPA/RSN security and PMF, HT/VHT/HE capability IEs,
WPS device/manufacturer/model hints, and separate monitored/advertised channels.
Legacy files load without a migration; details are filled in on the next decode.
See [WIFI_IDENTITIES.md](docs/WIFI_IDENTITIES.md) for schema, semantics, build
requirements, provenance, and new identity/GUI tests. Wi-Fi GUI functions now live
in `src/wifi_gui.cpp`; vendor lookup in `src/wifi_vendor.cpp`.

The earlier CRC fix was already committed in `5d2fb82`; the historical
"Uncommitted changes" section below is superseded. The earlier claim that 5 GHz
never attempts fingerprinting was inaccurate: it uses the shared OFDM branch,
but remains unvalidated. The earlier device counts are historical observations,
not current counts or verified physical-device counts. No LoRa-specific code was
changed for this update. Remaining work begins with capture/replay validation and
fingerprint calibration, as separately agreed with the user.

---


**Purpose of this file:** a complete, standalone briefing for picking up this
codebase cold — architecture, what's built, what's proven on real hardware
vs. only in synthetic tests, every non-obvious bug found this session (with
the exact reasoning that found it, since the *method* matters as much as
the fix), and the concrete next-task list. Written at git commit `6061c4b`
plus one uncommitted fix on top (see "Uncommitted changes" below).

Scope: this document covers **only** the C++ project in this directory
(`newrocktest-cpp`). There is a sibling Python prototype elsewhere on this
machine (`newrocktest`) that predates this project and is a separate,
unrelated codebase — ignore it; nothing here depends on it.

---

## 1. What this is

A live SDR-based RF emitter monitor and classifier with a Dear ImGui desktop
GUI (`rf_monitor_gui`). Three bands, one radio at a time:

- **Sub-GHz LoRa (863–868 MHz, IN865)** — chirp-spread-spectrum PHY decode,
  RF fingerprinting, a persistent cross-run device list. **Mature. Treat as
  frozen** — see §3.
- **Wi-Fi 2.4 GHz** — OFDM/DSSS modulation classification, 802.11b beacon
  decode (BSSID/SSID), RF fingerprinting, a persistent cross-run device
  list keyed by real MAC when available. **This session's main work.**
- **Wi-Fi 5 GHz** — classification only, **completely unvalidated**. No
  fingerprinting, no decode. Do not assume anything here works.

Hardware: USRP X310 (Ethernet/RFNoC, this session's radio) or USRP B210
(USB3). Two antenna slots on the X310, both UBX-160 daughterboards; **RX2
on RF-A is the main/only receive antenna in use** — the second antenna was
provisioned for a future TX-loopback validation rig, never wired up.

### Build / run

```bash
cmake -S . -B build && cmake --build build -j$(nproc)
./build/rf_monitor_gui
```

Device selector defaults to B210 in the UI (`main.cpp`'s
`selected_device` static) — click **USRP X310** then **Connect** if that's
the radio physically present. Nothing auto-connects on launch.

Test suite (12 binaries, 203 checks total as of this commit, all synthetic/
pure-software except `band_smoke_test` and `sf_estimator_spike`, which need
real hardware):

```bash
for t in test_dsp test_lora_phy test_lora_phy_std test_wifi_phy \
         test_wifi_frame test_wifi_dsss_rx test_wifi_stress test_fingerprint \
         test_wifi_fingerprint test_registry test_lora_master test_wifi_master; do
  ./build/$t
done
```

---

## 2. Repository map

```
src/
  config.hpp         Every band's scan plan, device profiles (X310 vs B210
                      sample rates/gain), WiFi channel tables, all tuning
                      constants for both LoRa and WiFi paths.
  spectrum.{hpp,cpp}  Max-hold spectrogram (KissFFT), DC/edge guard masks -
                      shared energy-detection front end for both bands.
  detector.{hpp,cpp}  Threshold segmentation of a spectrum into Segments.
  classifier.{hpp,cpp} Bandwidth-based protocol-guess labels.
  registry.{hpp,cpp}  Per-session (non-persistent) DeviceRegistry - one per
                      band, forgets everything on restart, 10-reading
                      rolling median for fingerprint matching.
  sdr_capture.{hpp,cpp} UHD wrapper, device-agnostic finite-duration capture.
  scanner.{hpp,cpp}   The background scan thread. Owns the radio, all three
                      bands' scan loops, both persistent master lists, both
                      packet logs. THE central orchestration file - 912
                      lines, read this to understand how anything gets
                      from "captured IQ" to "a GUI row".
  main.cpp            ImGui front end. 904 lines, one window, mode-switched
                      by active_band. Every table-drawing function lives
                      here.

  --- LoRa (§3 - do not modify without explicit instruction) ---
  lora_phy.{hpp,cpp}       Self-consistent CSS PHY codec (this project's own
                           modulate()/demodulate() pair).
  lora_phy_std.{hpp,cpp}   Standards-compliant SX1272/76-family codec -
                           tried against real hardware FIRST (scanner.cpp
                           tries this one before lora_phy's own).
  fingerprint.{hpp,cpp}    LoRa RF fingerprint extraction (widely-linear
                           IQ-imbalance model, CFO refinement, EVM/sync_corr
                           quality gates).
  lora_master.{hpp,cpp}    Persistent cross-run LoRa device list, numeric
                           IDs (LORA-0001...), fuzzy fingerprint matching.

  --- Wi-Fi ---
  wifi_phy.{hpp,cpp}       Modulation classifier (Schmidl-Cox for OFDM,
                           Barker-11 matched filter for DSSS), burst
                           detection, occupied-bandwidth estimate. Also
                           exposes remove_dc()/mix_to_baseband() (moved to
                           external linkage this session specifically so
                           wifi_fingerprint.cpp can reproduce the exact
                           same baseband domain classify_modulation()
                           measured its L-STF/L-LTF indices and coarse CFO
                           in - see §4.1).
  wifi_dsss_rx.{hpp,cpp}   Full 1 Mbps DSSS receive chain: resample to a
                           22 Msps chip grid, Barker despread with timing
                           search, DBPSK differential decode, descramble,
                           locate SYNC/SFD, PLCP header (CRC-16), PSDU,
                           hand off to wifi_frame.cpp for beacon parsing.
  wifi_frame.{hpp,cpp}     802.11 frame-layer decode: PLCP header CRC-16,
                           FCS-32, descrambler, beacon MAC/IE parsing.
                           Byte-level only, no DSP - checkable against
                           published vectors with no radio.
  wifi_fingerprint.{hpp,cpp}  NEW this session. WiFi RF fingerprint
                           extraction - a SEPARATE engine from the LoRa
                           one, by explicit instruction. See §4.
  wifi_master.{hpp,cpp}    NEW this session. Persistent cross-run WiFi
                           device list, MAC-keyed when a beacon decoded
                           one, fingerprint-cluster fallback otherwise.
                           See §4.4.

tools/
  band_smoke_test.cpp      Manual hardware validation - cycles all 3 bands,
                           needs the real radio, ~90s+ per full run.
  sf_estimator_spike.cpp   Standalone validation spike for a proposed
                           non-brute-force LoRa SF/BW estimator. NOT wired
                           into production. Documents a real, unresolved
                           bug in its own file header - see §6.5.

tests/  - one file per src/*.cpp module, synthetic-signal-based except
          where noted above.

data/
  lora_master/*.ndjson    Persistent LoRa device files (numeric id keyed).
  wifi_master/*.ndjson    Persistent WiFi device files. As of this session:
                          13 real MAC-keyed devices, 221 fingerprint-cluster
                          devices (mostly OFDM, which has no MAC decode
                          path at all yet - see §4.4/§7).

CONTEXT.md     An earlier, now partially-stale snapshot (pre-dates this
               session's WiFi fingerprinting/master-list/CRC work). Kept
               for history; THIS file is the current one.
PROJECT_STATUS.md, README.md   Earlier project docs, largely superseded.
```

---

## 3. LoRa subsystem — mature, do not modify without being asked

This side of the codebase went through its own extensive debugging session
before this one and is considered **stable and frozen**. Every conversation
that touched WiFi work this session was explicitly scoped to leave LoRa
files untouched, and that was verified via `git diff` before every commit
point. If you're picking this up fresh: **don't refactor, "clean up," or
touch `lora_phy*.cpp/hpp`, `fingerprint.cpp/hpp`, or `lora_master.cpp/hpp`**
unless the user specifically asks for LoRa work.

What it does: brute-force SF/BW search (8 SF × 3 BW = 24 hypotheses per
capture, no estimation — see `LORA_LISTEN_SF_LIST`/`LORA_LISTEN_BW_LIST_HZ`
in `config.hpp`), dechirp via FFT peak detection, two codecs tried in order
(standards-compliant first, this project's own self-consistent one second),
RF fingerprint extraction (widely-linear IQ model, CFO refinement via
Moose-style differential estimator, EVM/sync_corr quality gates calibrated
from real TarangMini captures — not the source spec's generic numbers,
which rejected almost every real capture), persistent device list with
fuzzy matching on IRR/DC/IQ-imbalance (CFO deliberately excluded — swings
too much burst-to-burst on this hardware to be useful for matching).

**Known, unresolved, and out of scope for now:**
- TarangMini's header/payload never fully decodes against real hardware.
  Every formula in the standards-compliant codec was verified character-by-
  character against the reference implementation; it still doesn't
  validate. Current best theory: TarangNet encrypts/scrambles the payload
  above the PHY layer — a protocol question, not a signal-processing one.
- A real EVM floor of 25–73% on confirmed-correct real captures (vs. the
  source spec's assumed ~10%), most likely PA/chirp nonlinearity or a
  multipath-heavy test environment. Gates were recalibrated to this
  measured reality rather than "fixed" — there's no known further work here
  without new hardware/environment data.
- The persistent master list fragments one physical TarangMini into many
  device entries (19+ observed in one sweep). Root cause is understood
  (SF-dependent drift in irr_db/dc_dbc — see `tools/sf_estimator_spike.cpp`'s
  own analysis for the closely related SF/BW aliasing math) but a fix
  (e.g. per-SF-partitioned matching) was never implemented — the user
  pivoted to WiFi work before this was addressed.

---

## 4. Wi-Fi subsystem — this session's work

### 4.1 What was already there vs. what's new

Already existed and unchanged in behavior: `wifi_phy.cpp`'s Schmidl-Cox/
Barker correlators, burst detection, occupied-bandwidth estimate,
`wifi_dsss_rx.cpp`'s despread/decode chain, `wifi_frame.cpp`'s PLCP/FCS/
beacon parsing. All were already tested (synthetically) and working.

**New this session:**
- `wifi_fingerprint.{hpp,cpp}` — the extraction engine (§4.2)
- `wifi_master.{hpp,cpp}` — the persistent identity list (§4.4)
- Additive fields on `ModClassification` (wifi_phy.hpp) exposing L-STF/
  L-LTF sample ranges and coarse CFO — previously computed internally and
  discarded
- Additive fields on `DsssDecodeResult` (wifi_dsss_rx.hpp) exposing the
  despread SYNC-field symbols
- `wifi_phy.cpp`'s `remove_dc()`/`mix_to_baseband()` moved from file-local
  (anonymous namespace) to externally-linked functions, so
  `wifi_fingerprint.cpp` can reproduce the exact same baseband-mixing
  domain `classify_modulation()` used
- Packet-list and master-list GUI tables in `main.cpp`
- **A real, confirmed bug fix in `wifi_frame.cpp`'s `parse_plcp_header()`**
  — not new code, a correction to existing code — see §5.3, this is the
  most important single fix from this session

### 4.2 The WiFi fingerprinting engine (`wifi_fingerprint.cpp`)

Deliberately a **separate engine from LoRa's**, per explicit instruction —
different PHY, different preamble structure, different dominant corruption
source (multipath, barely a factor for LoRa's narrowband chirp, is WiFi's
biggest problem). The quadrature/mixer math (widely-linear IQ model, same
3×3 normal-equations fit) is the same *formula* as `fingerprint.cpp`'s,
reimplemented independently — matching this project's established
convention (see `lora_phy_std.cpp`'s own header for the precedent) of
duplicating validated math for a new caller rather than risking a shared
refactor.

**OFDM path** (`extract_ofdm_fingerprint`):
1. Two-stage CFO: coarse from Schmidl-Cox's own `phase(P)` (previously
   computed nowhere — only `|P|²` was used for the plateau test), fine via
   a Moose-style delay-and-conjugate over L-LTF's two 3.2µs long symbols.
2. Mixes the channel-offset frequency down to baseband over the *whole*
   needed range in one call (critical — see §5.1).
3. DC bias estimated from the pre-burst noise window *only*, not blended
   with burst content (see §5.2).
4. Widely-linear fit against a freshly-built time-domain L-LTF reference
   (the real, standard 52-subcarrier BPSK sequence, evaluated as a direct
   sum so it's correct at any sample rate — X310 here runs 20 Msps, B210
   runs 56 Msps, neither a clean multiple of the other).
5. Yields cfo_ppm, irr_db, iq_eps, iq_phi_deg, dc_dbc, dc_ang_deg, snr_db,
   evm_pct, sync_corr — same shape as LoRa's `LoraFingerprint`.

**DSSS path** (`extract_dsss_fingerprint`):
1. CFO from despread SYNC-field symbols: re-integrates the DBPSK
   differential decisions to reconstruct an absolute phase reference (no
   need to know the scrambler state), then the same delay-and-conjugate
   estimator.
2. **A genuine, structural limitation, not a bug:** `irr_db`/`iq_eps`/
   `iq_phi_deg` are left at 0.0 for every DSSS reading. The SYNC field's
   reference is real-valued (BPSK ±1), so `s` and `conj(s)` are the *same*
   vector — the widely-linear 3-parameter fit is singular by construction.
   Gain/phase imbalance are fundamentally unidentifiable against a
   real-valued reference by this technique; this is the textbook reason
   QPSK/OFDM references can resolve IQ imbalance and BPSK ones can't. A
   genuinely complex DSSS reference (the raw chip-domain Barker pattern
   rather than the despread symbol domain) could recover this in future
   work — not attempted.
3. DC offset and CFO are still fully recoverable via a reduced 2-parameter
   fit (`r' = A·s + c`, A complex, s real).

**Gate thresholds** (`WIFI_SNR_FLOOR_DB=10.0`, `WIFI_EVM_CEILING_PCT=80.0`,
`WIFI_SYNC_CORR_FLOOR=0.5`): **starting guesses, not calibrated.**
Deliberately looser than LoRa's own recalibrated numbers, since WiFi's
L-STF/L-LTF (8µs+8µs) and DSSS's SYNC field (~128µs) are one to three
orders of magnitude shorter than LoRa's tens-of-symbols preamble — expect
materially higher single-burst variance even from a clean signal. Same
discipline as LoRa's own gates needed: measure against real data, don't
trust a first guess.

### 4.3 Packet list — richer per-packet data

`WifiPacketRow` (scanner.hpp) now carries `fp_cfo_ppm`, `fp_irr_db`,
`fp_iq_eps`, `fp_iq_phi_deg`, `fp_dc_dbc`, `fp_dc_ang_deg`, `fp_snr_db`,
`fp_evm_pct`, `fp_sync_corr`, `fp_gate_reason` — all `std::optional`, unset
when extraction wasn't attempted (OFDM without a usable L-LTF range, or a
DSSS burst too short to be beacon-plausible) vs. attempted-but-gated
(`fp_gate_reason` set). The GUI table (`draw_wifi_packet_table`, main.cpp)
shows "n/a" (grey) for IRR/IQ-eps/IQ-phi on every DSSS row specifically —
distinct from "--" (not attempted/gated), to keep the structural DSSS
limitation from §4.2 visually distinguishable from an ordinary miss.

### 4.4 The persistent WiFi master list (`wifi_master.cpp`)

Structurally parallel to `lora_master.cpp` (one NDJSON file per device,
meta line + reading lines, atomic tmp-then-rename compaction, lazy
compaction past `COMPACT_TRIGGER_READINGS`) but keyed differently — this
was the explicit design brief: **MAC address is the primary identity key
whenever one is decoded; RF fingerprint clustering is the LoRa-style
fallback for everything else**, not the primary mechanism (unlike LoRa,
which has no MAC ever).

- **MAC-keyed devices**: exact string match, no fuzzy tolerance — a
  decoded MAC is ground truth. Device key is the MAC string itself
  (`"aa:bb:cc:dd:ee:ff"`), filename is the same string + `.ndjson`.
- **Fingerprint-cluster devices**: same tolerance-gate-then-nearest-
  tiebreak algorithm as LoRa (`IRR_TOLERANCE_DB=8.0`, `DC_DBC_TOLERANCE_DB
  =8.0`, `IQ_EPS_TOLERANCE=0.02`, `IQ_PHI_TOLERANCE_DEG=3.0` — same numbers
  as LoRa's, unvalidated for WiFi specifically, see §7). Device key is
  `"WIFI-FP-0001"` etc.
- `cfo_ppm` is **excluded** from fingerprint-cluster matching tolerances,
  same as LoRa — but this is flagged explicitly as a **placeholder that
  needs its own WiFi-specific validation**, not a carried-over conclusion:
  LoRa's exclusion was justified by real-hardware evidence that CFO swings
  too much burst-to-burst on *that* hardware; the WiFi RF-fingerprinting
  literature (PARADIS) ranks CFO as the single *most* effective
  radiometric feature — the opposite conclusion. Nobody has tested which
  is true on *this* hardware for WiFi. Don't resolve this by reading a
  paper; test it against real multi-session data.
- **MAC source today**: only DSSS beacon decode (`wifi_frame.cpp`'s
  `parse_beacon()`, via `wifi_dsss_rx.cpp`) produces a MAC. There is no
  OFDM MAC decode anywhere in this project — building one would need a
  full 6 Mbps OFDM MPDU receive chain (64-point FFT, channel estimation,
  deinterleaving, Viterbi decode), explicitly scoped out (`wifi_frame.hpp`'s
  own header: doing this "would roughly triple" the project). This is why,
  in the live data as of this session, **13 devices are MAC-keyed and 221
  are fingerprint-clusters** — almost everything on this 2.4GHz capture is
  OFDM traffic without a MAC decode path.
- **Retention**: same simple per-reading NDJSON scheme as LoRa (1000-
  reading cap), *not* the more elaborate time-bucketed scheme originally
  proposed in planning — chosen as the simplest testable v1 given no real
  WiFi packet-rate data existed yet to size a bucketing scheme against.
  **Revisit if real field volume actually saturates this** (WiFi's packet
  rate is orders of magnitude higher than LoRa's; whether the existing
  bandwidth/beacon-duration gates keep the recorded rate low enough is
  untested at scale).

---

## 5. Every real bug found this session, and exactly how it was found

The methodology matters here as much as the fixes: every one of these was
found by **adding live diagnostics to real hardware captures and reading
the actual numbers**, not by reasoning from the code alone. Several
initial hypotheses were wrong and were caught *because* they were checked
against real data before being trusted. If you extend this codebase,
follow the same discipline — this project has been burned repeatedly (in
earlier sessions too, per `CONTEXT.md`) by fixes that looked right on paper
and were wrong.

### 5.1 OFDM fingerprint: missing channel-offset mixing

**Symptom:** every single real OFDM burst gate-rejected on EVM (94–157%,
ceiling 80%), with a suspiciously *consistent* ~19–20ppm CFO across many
different real access points.

**Root cause:** WiFi channels are captured `WIFI_CHANNEL_CAPTURE_OFFSET_HZ`
(1.5MHz, config.hpp) off the true channel center, to keep the channel's
peak clear of the DC-guard notch. `classify_modulation()` removes this
offset internally before measuring `mc.l_stf_start`/`cfo_coarse_hz`. My
first version of `extract_ofdm_fingerprint()` operated on the raw capture
buffer directly, never repeating that mix — a 1.5MHz residual, miles
outside either CFO estimator's unambiguous range (±625kHz coarse,
±156.25kHz fine), which **aliased to a small, plausible-looking wrong
number** rather than an obviously-huge one. That consistency across
different real APs was the tell that something systematic, not random
noise, was happening.

**Fix:** mix down properly (`wifi::remove_dc()` + `wifi::mix_to_baseband()`,
now exposed with external linkage from `wifi_phy.hpp` specifically for
this) over the *whole* range needed, in one call, before any CFO/fit math.

**Test added:** `test_wifi_fingerprint.cpp`'s test 1b constructs a synthetic
burst with a real non-zero channel offset — every *other* test in that
file uses offset 0, which is exactly why the bug wasn't caught originally.

### 5.2 A second, subtler issue found while fixing 5.1

Once mixing was fixed, `dc_dbc` recovery was still off by several dB in the
synthetic test. Cause: `remove_dc()`'s whole-buffer mean blends the
pre-burst noise (correct target for DC-leakage removal) with the burst's
own transmitted `c` term (which *rotates* with CFO and only averages
toward zero over many full rotation cycles — a short test window doesn't
complete even one cycle). **Fix:** estimate the DC bias from the clean
pre-burst noise window only, not the combined noise+burst slice.

### 5.3 The big one: PLCP header CRC-16 always failing (blocking all MAC decode)

**Symptom:** SYNC+SFD lock was 100% reliable on real DSSS traffic, but the
PLCP header CRC-16 failed 100% of the time right after — meaning no beacon
ever decoded, meaning no MAC was ever available for the master list.

**Investigation, in order** (each step was a live diagnostic against real
hardware, not a guess):
1. Logged which SFD orientation matched (`kLongSfd` vs. its bit-reversal) —
   100% matched the reversed form, on a clean signal (median SNR proxy
   ~27dB). Ruled out marginal noise as the cause outright.
2. Tried ±8-bit positional offsets around the header start — none passed.
   Ruled out a simple alignment/timing-drift bug.
3. Tried three candidate bit-reversal transformations on the *whole* 48-bit
   header window (mirror the whole span; reverse bits within each byte,
   keep byte order; reverse both) — **all three failed 100%**, which in
   hindsight ruled out "the whole header's bit order is backwards" as a
   theory too.
4. Checked how *close* the unmodified decode was: `SIGNAL` (0x0A=1Mbps,
   0x14=2Mbps — both real, valid rate encodings), `SERVICE` (0x04, rock
   stable), and `LENGTH` (repeating exact values across many captures of
   what were clearly the same real frames) were all decoding **perfectly**.
   Only the CRC's own 2 bytes were wrong, with a Hamming distance (6-8 of
   16 bits) close to what two *independent random* 16-bit values would
   show — not "a few noisy bit flips."
5. Since everything *except* the CRC's own two bytes decodes correctly,
   tested reversing *only* those two bytes' bits, keeping byte order (high
   byte still first). **100% of 381/382 real captures matched exactly**
   (the one exception plausibly a genuinely corrupted frame).

**Root cause:** a real, documented asymmetry in the 802.11b PLCP standard:
SIGNAL/SERVICE/LENGTH are transmitted LSB-of-octet-first, but the CRC-16
field specifically is transmitted MSB-of-octet-first (the CRC shift
register naturally shifts its own MSB out first). `bits_to_bytes_lsb_first()`
packs all 6 header bytes uniformly, correct for the first 4, wrong for the
CRC's own last 2 — this had never been visible before because the SYNC
field (an all-ones run) is a palindrome under bit-reversal, so it decoded
correctly *regardless* of this bug, hiding it completely until the SFD (a
non-palindromic 16-bit pattern) exposed it.

**Fix:** in `wifi_frame.cpp`'s `parse_plcp_header()`, reverse each of
`bytes[4]`/`bytes[5]`'s own bits before combining into `h.crc` (byte order,
high-byte-first, was already correct — only the *within-byte* bit order
needed correcting).

**A circular-test trap found and fixed in the same pass:** the existing
synthetic test for this function built its "transmitted" CRC bytes as
`hdr[4] = crc>>8; hdr[5] = crc&0xFF` — the *same* convention the decoder
assumed — so the round-trip passed regardless of whether that convention
was correct. This is precisely the failure mode `wifi_frame.hpp`'s own
file header warns about ("a prototype's 'all tests pass' turned out to
mean only that its encoder and decoder shared the same wrong tables").
Fixed the test to model actual transmission (bit-reverse each CRC byte
before packing), and separately found and fixed the **identical** latent
bug in `test_wifi_dsss_rx.cpp`'s own independent synthetic generator, which
had made the same wrong assumption despite being written independently.

**New test added:** `test_wifi_frame.cpp`'s
`plcp_header_crc_byte_order_confirmed_against_real_capture` uses a real
captured vector from this session (`{0x0A, 0x04, 0x30, 0x06, 0x85, 0x90}`
→ SIGNAL=1Mbps, SERVICE=0x04, LENGTH=1584µs, correct CRC=0xA109) — a golden
vector from real hardware, not a self-consistent construction, matching
this file's own stated testing discipline.

**Live confirmation after the fix:** 13 real MAC-keyed devices now exist
in `data/wifi_master/`, decoded from real over-the-air beacons.

### 5.4 Note on an EARLIER (pre-this-session) documented "blocker" that may now be moot

An earlier session's notes (see `CONTEXT.md`) documented live 1 Mbps beacon
decode as blocked by transport capacity — the belief being that the DSSS
despread chain needed a native ~22.222 Msps capture rate (2 samples/chip
exactly), which crashed the X310 process when attempted (see
`config.hpp`'s long comment on this — reverted to 20 Msps). **This session's
work directly contradicts that being a hard blocker**: `wifi_dsss_rx.cpp`
already linearly resamples whatever the actual capture rate is (20 Msps on
this X310) up to the 22 Msps chip grid, and beacon decode is now
**confirmed working** at that resampled rate — 13 real MACs decoded, real
SSIDs recovered (see §7's example). The CRC bug in §5.3, not sample rate,
was the actual blocker all along. Worth flagging so nobody re-invests in
jumbo frames / `net.core.wmem_max` / a 10GbE NIC / switching WiFi to the
B210 chasing a problem that turned out to be somewhere else entirely.

---

## 6. Engineering conventions and hard-won lessons (read before extending)

1. **Never trust a round-trip test where the encoder and decoder share an
   assumption.** Two separate confirmed instances this session alone
   (§5.3). If a "synthetic transmitter" test was written by copying the
   same convention the receiver assumes, it can only prove self-
   consistency, never correctness. Prefer independent construction, and
   prefer a real captured golden vector over any synthetic one when you
   can get one.
2. **Measure against real hardware before fixing, and after.** Every fix
   in §5 was found by adding temporary `#ifdef ...DIAG` stderr
   instrumentation, rebuilding, asking the user to run real traffic, and
   reading the actual numbers — not by reasoning from the code in the
   abstract. Multiple *plausible-sounding* hypotheses were tested and
   rejected this way (§5.3 steps 1–3) before the real cause was found.
   Remove diagnostic instrumentation once a fix is confirmed; don't leave
   `#ifdef DIAG` blocks in shipped code.
3. **A consistent-looking wrong number is more suspicious than a wildly
   wrong one.** The OFDM CFO bug (§5.1) presented as a narrow, plausible
   ~19-20ppm cluster across different real devices — that consistency,
   not randomness, was what gave away a systematic bug (an aliased offset)
   rather than genuine per-device variation.
4. **A structural mathematical limitation is not a bug to "fix."** DSSS's
   inability to resolve IQ imbalance (§4.2) is a real, provable
   consequence of using a real-valued reference constellation — the
   correct response is to document it and leave the fields unset, not to
   force a meaningless fit.
5. **C++ "most vexing parse."** `std::vector<T> name(size_t(expr));` inside
   a function gets parsed as a function *declaration*, not a variable
   definition, whenever `expr` itself looks like a call. Recurred 3+ times
   across this whole project's history (LoRa side too, per `CONTEXT.md`).
   Fix: a named `size_t` intermediate on its own line first.
6. **Keep LoRa and WiFi genuinely separate when asked to.** Explicit user
   instruction this session: a separate fingerprinting engine, not a
   shared one, even where the underlying math (widely-linear IQ fit) is
   identical — re-implement, don't import, matching `lora_phy_std.cpp`'s
   own established precedent for validated-math reuse.
7. **Gate/tolerance constants are placeholders until measured.** Every
   numeric threshold introduced this session (`WIFI_SNR_FLOOR_DB`,
   `WIFI_EVM_CEILING_PCT`, `WIFI_SYNC_CORR_FLOOR`, the master-list matching
   tolerances) was set by analogy to LoRa's numbers or by rough judgment,
   explicitly flagged in comments as unvalidated. LoRa's own gates only
   became trustworthy after real-hardware testing overturned the source
   spec's generic starting numbers (see `fingerprint.hpp`'s own comment on
   `LORA_EVM_CEILING_PCT`'s history) — expect the same process to be needed
   for WiFi's numbers before trusting them for anything consequential.

---

## 7. Current state — what's actually confirmed working right now

- Full test suite: **203/203 passing**, 12 binaries.
- LoRa: unchanged, untouched this session (verified via `git diff` on every
  LoRa file before each commit point).
- WiFi OFDM classification + burst detection: working (pre-existing).
- WiFi OFDM fingerprinting: working *and validated on real hardware*
  post-fix (§5.1) — real CFO/IRR/IQ-eps/IQ-phi/DC values recovered from
  real access points, visible live in the packet list and the
  221 fingerprint-cluster entries in `data/wifi_master/`.
- WiFi DSSS beacon decode (BSSID + SSID): **working end-to-end on real
  hardware as of this session's fix** — 13 real MAC-keyed devices in
  `data/wifi_master/`, decoded from genuine over-the-air 802.11b beacons
  (example real device seen this session: BSSID `3c:52:a1:0b:bf:d7`, SSID
  `Avgarde_airtel`, channel 9 — this exact vector is now also a test case
  in `test_wifi_dsss_rx.cpp`).
- WiFi DSSS fingerprinting: working (CFO + DC offset; IRR/IQ-eps/IQ-phi
  structurally unavailable, §4.2).
- 5GHz WiFi: **not validated at all**. Classification code runs, nothing
  else has been checked against real 5GHz traffic.

## Uncommitted changes as of this handover

The last git commit (`6061c4b`) predates the §5.3 CRC fix. Uncommitted on
top of it right now:
- `src/wifi_frame.cpp` — the `parse_plcp_header()` bit-reversal fix
- `tests/test_wifi_frame.cpp` — corrected test 3 + new real-capture test
- `tests/test_wifi_dsss_rx.cpp` — corrected synthetic generator

All three are one coherent, tested, verified change (§5.3). Commit them
together when ready.

---

## 8. Next tasks

Roughly in the order they were raised or seem highest-value; not a strict
priority ranking — use judgment based on what the user actually asks for
next.

1. **[Just requested by user] Identify each device — names and other
   unencrypted info, beyond just SSID/BSSID.** Concretely, in rough order
   of effort:
   - **Vendor OUI lookup from BSSID** — the first 3 bytes of a MAC address
     identify the manufacturer via IEEE's public OUI registry. Cheap,
     no new DSP, just a lookup table (would need to be bundled — IEEE
     publishes this as a downloadable CSV; check licensing/size before
     vendoring it wholesale, or hardcode a curated subset of common
     consumer vendors).
   - **Surface the beacon's `capability` field** — `BeaconInfo::capability`
     (wifi_frame.hpp) is already parsed but not currently shown anywhere in
     the GUI. Bit 4 (Privacy) tells you if the network is open or
     encrypted — real, unencrypted, useful info that's already being
     thrown away.
   - **Parse additional Information Elements in the beacon body** — only
     SSID (tag 0) and DS Parameter Set (tag 3) are parsed today
     (`wifi_frame.cpp`). Vendor-specific IEs (tag 221) sometimes carry
     device/model hints (e.g. WPS IEs occasionally leak a manufacturer
     name or model string on APs with WPS enabled). HT/VHT capability IEs
     would tell you the AP's 802.11 generation.
   - **Extend beyond beacon/probe-response frames** — `parse_beacon()`
     only accepts those two subtypes (`wifi_frame.cpp`'s subtype check).
     Probe *requests* (sent by client devices, not APs) often carry the
     SSID of a network the client previously connected to, and would let
     you identify client STAs, not just APs — this is a materially bigger
     lift (different frame layout, different fields) but would roughly
     double what's visible.
2. Calibrate the WiFi fingerprint gate thresholds (§6.7) against a larger
   real capture set — right now they're unvalidated placeholders.
3. Resolve whether `cfo_ppm` should participate in WiFi fingerprint-
   cluster matching (§4.4) — test both inclusion and exclusion against
   real multi-session data on this hardware; don't decide from literature
   alone.
4. Consider whether DSSS fingerprinting should run on non-beacon-duration
   bursts too (currently gated to beacon-plausible durations only, as a
   documented scope limit — see the comment in `scanner.cpp`'s WiFi burst
   loop) — would give fingerprint coverage on ordinary data frames, not
   just beacons.
5. OFDM MAC decode — a full 6 Mbps OFDM MPDU receive chain. Big lift,
   explicitly out of scope so far (§4.4). Would unlock MAC-keying for the
   221 currently-fingerprint-cluster-only OFDM devices.
6. 5GHz WiFi validation — nothing has been checked against real 5GHz
   traffic at all yet.
7. Exploratory WiFi fingerprint features never built: spectral regrowth on
   L-STF/L-LTF null subcarriers (OFDM), despreading-gain/correlator-loss
   (DSSS) — flagged lower-confidence in original planning, deferred.
8. Revisit WiFi master-list retention (§4.4) if real field packet volume
   turns out to saturate the simple 1000-reading-per-device cap faster
   than expected — the more elaborate time-bucketed scheme from original
   planning was deliberately not built for v1.
9. LoRa (§3, lower priority unless the user asks): TarangMini payload
   decode is blocked on what looks like an above-PHY encryption/scrambling
   question, not a signal-processing one — no clear next step without new
   information. The 19-way device fragmentation from SF-dependent drift
   has a proposed fix (per-SF-partitioned matching) that was never
   implemented.
10. The non-brute-force LoRa SF/BW estimator (`tools/sf_estimator_spike.cpp`)
    — validated the core idea (two independent measurements can break the
    SF/BW aliasing ambiguity) but found a real, unresolved bug (a spurious
    half-symbol-period correlation peak in the autocorrelation search,
    100% consistent at SF12 in testing) that needs a harmonic-consistency
    check before it could replace the brute force. Not wired into
    production. Read that file's own header comment for the full story
    before touching it.

---

## 9. Session update — persistent Wi-Fi identities and GUI identification (2026-09-16)

**Latest scope requested by the user:** complete persistent Wi-Fi identity
records and identification details, including the GUI; stop after those two
milestones and decide the next task later. **Do not touch the LoRa side.**
Both milestones are implemented. The work described here is currently
**uncommitted** on top of `5d2fb82`; that commit already contains the earlier
PLCP CRC fix. This section supersedes conflicting statements in the historical
sections above. The short update near the top is only a summary.

### 9.1 The identity-persistence gap that was fixed

Previously, `scanner.cpp` called `WifiMasterList::record_reading()` only when
an extracted fingerprint passed its quality gates. Even an FCS-valid decoded
BSSID could therefore disappear on restart if its fingerprint failed. SSID,
beacon capabilities and identification details were not saved in the master
schema at all.

The scanner now records an FCS-valid beacon/probe response immediately through
`record_identity()`, independently of fingerprint acceptance. Accepted RF
readings are still recorded separately and join the same BSSID-keyed entry.
A device with **zero accepted fingerprints** is a valid persistent identity.
`record_reading()` returns its persistent key; packet rows carry that key plus
optional decoded `BeaconInfo`, linking observations to MAC identities or
provisional fingerprint clusters.

Identity records retain:

- BSSID, learned SSID, first/last seen and a decoded-observation count.
- Latest identity observation time and frame source (beacon/probe response).
- Advertised channel and its source, separately from monitored channel center.
  DS Parameter Set takes precedence; HT Operation supplies a fallback. An
  absent advertised channel remains unknown rather than inheriting the tuned
  channel and appearing to be decoded information.
- Beacon interval, capability bits, advertised security/ciphers/PMF, PHY
  capability IEs, and optional WPS identification hints.
- Separate last-observed timestamps and frame sources for the learned SSID
  and retained WPS hints.

A later hidden/omitted SSID does not erase a learned name. A later nonempty name
updates it. Frames that omit WPS preserve prior WPS hints and their original
source/time. A frame containing WPS replaces the WPS fields with that frame's
advertisement; missing attributes are not invented. Older identity observations
do not overwrite newer metadata. Fingerprint clusters are **not automatically
merged into decoded BSSIDs**: equivalence is not established by this work.

### 9.2 Identification details implemented

`wifi_frame.cpp/.hpp` now exposes and parses:

- Capability bits and beacon interval, including the existing Privacy bit.
- WPA and RSN authentication/key-management and cipher suites, including PSK,
  enterprise 802.1X variants, SAE, and OWE; PMF capability/requirement from RSN.
  Unknown suite selectors remain explicitly unknown. The Privacy bit alone is
  labeled **"Privacy set (legacy/unknown)"**, not assumed to mean WEP or WPA.
- HT/VHT/HE capability IEs, presented as advertised capabilities rather than
  proof of certification or a complete hardware-generation determination.
- WPS manufacturer, model name, model number and device name. WPS vendor IE
  payloads are concatenated before parsing attributes, including attributes
  split across IEs. Lengths are bounded; malformed/truncated IEs are flagged.
- Frame source, SSID-presence state and information-element completeness.

`wifi_vendor.cpp/.hpp` adds offline IEEE vendor-assignment lookup:

- **53,917 entries**, downloaded directly from IEEE on 2026-09-16.
- Longest-prefix matching across MA-S (/36), MA-M (/28), and MA-L (/24).
- Canonical MAC normalization; locally administered, group, invalid and
  unknown addresses are handled explicitly instead of inventing a vendor.
- The database is compiled into the application, so lookup needs neither a
  network connection nor a particular working directory at runtime.
- Generated data, snapshot date and provenance are under `third_party/ieee/`.
  `tools/build_wifi_vendors.py` regenerates the table from the three IEEE CSVs.
  See `third_party/ieee/README.md` for download URLs and redistribution notes.

Interpretation matters: an IEEE assignment names the registered organization,
not necessarily the retail brand/model. WPS names are self-advertised hints.
A BSSID identifies a network/interface, not a guaranteed unique physical radio;
several BSSIDs can share hardware. RF clusters remain provisional identities.

### 9.3 Persistence format and compatibility

Wi-Fi storage now uses nlohmann JSON rather than substring-based JSON parsing.
New meta records declare schema 2. The NDJSON format keeps legacy meta and RF
reading lines and adds typed `"identity"` snapshot lines.

- Existing files load without an offline migration. Their fingerprint history
  and first-seen times remain intact. Legacy MAC entries obtain beacon details
  on the next successful decode; previously unsaved metadata cannot be recovered
  from old RF values.
- SSID/WPS strings preserve exact octets in hexadecimal fields alongside escaped
  human-readable values. Quotes, backslashes, NUL and non-UTF-8 bytes cannot break
  the JSON or silently change the stored identity string.
- Identity snapshots compact after more than 100 appended observations. The
  cumulative identity count and latest metadata survive compaction.
- RF retention keeps the existing 1000-reading cap with lazy-compaction slack
  up to 1100. RF compaction also preserves identity metadata and timestamps.
- Compaction uses a temporary file and rename. Write/close/rename failures are
  surfaced through `storage_error()` and displayed in the GUI. In-memory data
  remains available, but a storage warning means persistence is not assured.
- Malformed records are skipped with a visible warning. A partial last line is
  separated from subsequent appends so the next valid observation can reload.

This stores the latest identity summary and cumulative observation count, not
an unlimited history of every name, WPS advertisement or decoded frame.
Fingerprint feature extraction and matching tolerances were not recalibrated.

### 9.4 GUI and code map changes

The Wi-Fi table functions moved from `main.cpp` into
`src/wifi_gui.cpp/.hpp` so the actual production UI can be tested independently
of Scanner and hardware. LoRa GUI functions remain unchanged.

- **Packet table:** adds persistent identity key, decoded SSID, advertised AP
  channel and security. The existing channel/frequency describe monitoring.
- **Wi-Fi Identities & RF Clusters table:** shows network name, vendor assignment,
  security, advertised channel, WPS model, PHY capabilities, first/last seen,
  identity-observation count, retained RF-reading count and identity source.
- **Click an identity:** opens details with vendor lookup provenance, decoded
  metadata, source/timestamps, capability bits, cipher suites, PMF, WPS hints,
  and the latest accepted RF reading with its own timestamp/PHY.
- Missing metadata is shown as unknown/not advertised. An identity with no
  accepted fingerprint is identified explicitly; zero-valued RF measurements
  are not fabricated. DSSS IQ-imbalance fields remain n/a.
- Storage warnings appear above the master table. The old GUI claim that every
  RF reading is retained forever has been replaced with the actual retention
  behavior.

Modified files: `src/wifi_frame.{cpp,hpp}`, `src/wifi_master.{cpp,hpp}`,
Wi-Fi portions of `src/scanner.{cpp,hpp}` and `src/main.cpp`, `CMakeLists.txt`,
and this handover. New files: `src/wifi_gui.{cpp,hpp}`,
`src/wifi_vendor.{cpp,hpp}`, `tests/test_wifi_identity.cpp`,
`tests/test_wifi_gui.cpp`, `tools/build_wifi_vendors.py`,
`third_party/ieee/*`, and `docs/WIFI_IDENTITIES.md`.

### 9.5 Build requirements and completed validation

New required dependency: **nlohmann JSON headers, version 3.9 or later**
(`nlohmann-json3-dev` on Ubuntu). Headers were already installed on this machine.
EGL development headers/library are optional and enable the offscreen GUI test;
they are not a new requirement for running the application itself.

```bash
cmake -S . -B build
cmake --build build -j4

# Existing 12 regression binaries:
for t in test_dsp test_lora_phy test_lora_phy_std test_wifi_phy \
         test_wifi_frame test_wifi_dsss_rx test_wifi_stress test_fingerprint \
         test_wifi_fingerprint test_registry test_lora_master test_wifi_master; do
  ./build/$t || break
done

# New identity/parser/persistence integration checks:
./build/test_wifi_identity

# Optional actual-GUI rendering/click test; no radio or desktop window:
LIBGL_ALWAYS_SOFTWARE=1 ./build/test_wifi_gui /tmp/wifi-ui
```

Results from this implementation session:

- Application and test builds succeeded; `git diff --check` passed.
- **All 14 test binaries passed**: the original 12 (203 checks), the new
  identity suite (**43 checks**), and the new GUI render/interaction test.
- Identity tests cover security/WPS/capability parsing, malformed lengths,
  vendor prefix precedence, rejected fingerprints, identity-only restart,
  exact string-byte persistence, hidden-name retention, compaction, legacy
  files, partial trailing records and storage failures.
- The actual Wi-Fi tables were rendered offscreen, the identity details view
  was opened by a simulated click, and both screenshots were visually inspected.
  These screenshots contain labeled test fixtures, **not live RF observations**.
- AddressSanitizer, UndefinedBehaviorSanitizer and leak checks passed for the
  identity suite. LeakSanitizer could not finish inside the sandbox's ptrace
  environment; the same sanitized test binary passed when rerun outside it.
- LoRa-specific source/header files, the LoRa GUI functions, and shared
  `config.hpp` were compared against HEAD and verified unchanged.
- **No fresh live-radio validation was performed for these changes.**

### 9.6 What the next session should know

Restart the GUI to load the new binary. Existing MAC records can show vendor
assignments immediately; SSID/security/WPS metadata fills in as the existing
receiver successfully decodes new beacons/probe responses. Do not promise WPS
model/device names for devices that do not advertise those fields.

The original §8 identification item is now completed for **existing supported
beacon/probe-response decoding**. Its probe-request/client-decoding extension
remains deferred. This session did not add another PHY decoder, expand the
DSSS beacon-duration gate, tune fingerprint thresholds, change cluster matching,
or validate 5 GHz. The shared 5 GHz OFDM branch already attempts fingerprinting,
contrary to the earlier handover's classification-only claim, but remains
unvalidated.

The initial inspection observed 15 MAC-keyed records and 307 fingerprint
clusters in saved data; these are a historical snapshot, not a current or
verified physical-device count. Do not interpret cluster growth as proof of
additional physical transmitters.

**Stop point:** the user requested these two milestones only and said the next
task would be decided afterward. Candidate follow-ups are live validation of
these identity details, capture/replay tooling, and measured fingerprint
calibration. None has been started or authorized as the next implementation.
Keep LoRa frozen unless the user explicitly changes that scope.

Further implementation notes and protocol references:
[docs/WIFI_IDENTITIES.md](docs/WIFI_IDENTITIES.md).

## 10. LoRa interpretability and offline capture/replay (2026-09-18)

The user explicitly reopened LoRa work after the TarangMini assessment and approved
starting with accurate statuses and capture/replay. This supersedes the earlier
LoRa freeze for this scope; Wi-Fi and the separate bench implementation were not changed.

- `src/lora_observation.*` now supplies shared per-hypothesis interpretation for
  live scans and replay. A valid header is no longer automatically called a decoded
  packet. CRC absent, failed, valid and not checked are distinct. Decoder provenance
  separates the SX-reference implementation (still unvalidated over the air) from
  the project's nonstandard internal codec.
- `src/lora_phy_std.*` retains a valid header and declared payload length when the
  capture is too short for a complete payload. This changes result reporting, not
  the PHY coding/synchronization algorithms.
- `src/lora_gui.*` contains the LoRa packet table, exact payload hex/ASCII tooltip,
  integrity explanations and asynchronous offline replay controls. Live capture
  controls in `main.cpp` request a one-shot save from Scanner's worker.
- `src/lora_capture.*` saves versioned JSON metadata plus portable little-endian
  float32 IQ with an accidental-corruption checksum. Unique directories prevent
  overwrites; bounded loading rejects malformed recordings. Requested settings and
  host timestamps are explicitly distinguished from measured/hardware values.
- Replay does not update live identities, fingerprint logs or master records.
  `tools/lora_replay.cpp` also provides a receiver-independent command-line path.
- Added `test_lora_observation` and offscreen `test_lora_gui`. Observation/capture
  tests and existing LoRa PHY tests passed; production GUI and smoke-tool targets
  built successfully. GUI fixtures were rendered offscreen and visually inspected.
  No real-radio capture or interoperability claim is made by these synthetic tests.

Usage, format details, limitations and next steps:
[docs/LORA_CAPTURE_REPLAY.md](docs/LORA_CAPTURE_REPLAY.md).
The connected TarangMini's LoRaWAN firmware does not match the supplied TarangNet
API manual. Receiver discovery and matching board documentation remain the hardware
blockers recorded in [TARANGMINI_ASSESSMENT.md](TARANGMINI_ASSESSMENT.md).

### 10.1 TarangMini sweep rejection handling (2026-09-18)

The user reported `2b038801` on every sweep send. Updated
`tools/tarangnet_api.py` and `tools/tarangmini_sf_bw_sweep.py` to require the exact
documented TarangNet firmware before writes, select root/router send commands from
readback, validate ACKs and response framing/command IDs, verify settings, and
restore only after an attempted change on supported firmware. Added `--inspect`.
Unsupported LW-S201 identification exits before sweep/flash/restore operations;
it does not invent a LoRaWAN transmit API. Seven mocked tests pass in
`tests/test_tarangmini_tools.py`. No hardware TX was attempted. See
[docs/TARANGMINI_SWEEP.md](docs/TARANGMINI_SWEEP.md) for commands and limitations.
Actual transmission with the currently identified LW-S201 firmware still needs
its matching API documentation; this host-tool fix does not resolve that blocker.

## 11. Legacy OFDM beacon decoding and live Wi-Fi integration (2026-09-19)

The user authorized implementation with the connected USRP, retaining the request
to leave LoRa untouched for this work. Added a native legacy 20 MHz OFDM receiver
(`src/wifi_ofdm_rx.*`): STF/LTF synchronization, CFO correction, channel estimation,
L-SIG validation, pilots, soft demapping, deinterleaving/depuncturing, Viterbi,
descrambling and MAC FCS. Software coverage includes all legacy rates from 6 to
54 Mbps. Modern HT/VHT/HE payload decoding is not included; their advertised
capability IEs can still be read from legacy OFDM beacons.

- Integrated decoding into the production scanner on both Wi-Fi bands, with burst
  context and without the DSSS duration gate. Only FCS-valid beacon/probe responses
  populate decoded identities. Corrected decoded AP channel mapping for 5 GHz.
- Persistent identities save the observation's actual PHY, independently of the
  latest accepted fingerprint. Older records default to DSSS. OFDM identities
  survive rejected fingerprints, compaction and restart.
- GUI packet rows show decode status, OFDM rate and PSDU length. Identity details
  show the FCS-valid source PHY alongside existing SSID/security/vendor/WPS fields.
- Added receive-only `wifi_capture_cli`, offline `wifi_ofdm_replay`, and finite
  Wi-Fi-only production `wifi_scan_smoke`. Replay does not modify live records.
- Real X310 captures decoded `Airtel_kira_7992` on channel 1 and `Avgarde_airtel`
  on channel 36 (including production +1.5 MHz tuning-offset recordings). All
  observed beacon rates in this validation were 6 Mbps. Do not equate software
  coverage of all eight rates with over-the-air validation of all eight.
- End-to-end production scanner runs then decoded/persisted 5 GHz
  `Flo Mobility upstairs` identities and hidden BSSIDs, and 2.4 GHz
  `Airtel_kira_7992`. Both finished with no overflow/error. Saved live identities
  are available in the normal GUI after restart. The radio was released afterward.
- Validation: 109 OFDM checks; 45 identity checks; existing Wi-Fi PHY, DSSS RX,
  frame, fingerprint, master and stress tests; offscreen production GUI rendering
  and popup interaction. GUI and smoke targets build. Three small received IQ
  regression crops retain provenance and exact bytes; zlib CRC and tshark field
  checks agree. An independent GNU Radio PHY comparison was attempted but its
  installed module crashed, so no independent IQ-decoder agreement is claimed.
- X310 setup intermittently timed out; the scanner's existing connection retry
  recovered. Failed/empty captures are excluded from successful results. No radio
  configuration, fingerprint gates, LoRa source, or LoRa tests were changed.

Implementation limits, capture locations, reproducible commands and next steps:
[docs/WIFI_OFDM.md](docs/WIFI_OFDM.md). The next useful task is controlled decode-yield
and RF calibration, to be chosen by the user after this milestone.
