# LoRa explainer — how detection, decoding and integrity checking actually work

Pure explanation of the code as it stands today. No behavior was changed
to write this. Everything below was read directly from the source files
named in each section (not recalled from memory of earlier sessions) —
where a mechanical detail looked surprising, it was cross-checked against
either the code's own comments, `docs/LORA_RECEIVER_UPGRADE.md`, or
`tests/test_lora_receiver.cpp`'s passing assertions before being written
down here, specifically to avoid describing something that only *looks*
plausible.

## 1. Two decoders exist; only one runs by default

There are **two, completely independent** decode paths, dispatched by
`analyze_lora_packets()` in `src/lora_observation.cpp`:

- **Production (default), `laboratory_mode=false`**: `src/lora_receiver.cpp`
  (namespace `rfmon::lora::receiver`), reported in the GUI as decoder
  `"LoRa explicit PHY"`. This is what live scanning and offline replay use
  unless the "Legacy laboratory codecs (nonstandard)" checkbox is on.
- **Legacy laboratory codecs, `laboratory_mode=true`**: tries
  `src/lora_phy_std.cpp`'s SX-reference codec first (`"Legacy SX reference
  (laboratory)"`), then `src/lora_phy.cpp`'s self-consistent codec
  (`"Legacy internal codec (nonstandard)"`). Off by default; production
  never falls back to these automatically.

Both paths, if neither decodes a header, fall back to a bare preamble
detector and report `"Detected only"` — see §9.

One more thing always runs regardless of which decoder is chosen: a
*separate, older* burst detector (`lora::detect_burst()`, also in
`lora_phy.cpp`) feeds the RF fingerprinting system (§11) and the
persistent master list (§12). This is deliberate, not legacy cruft left
behind — see §11's note on why its alignment is never attached to a
decoded packet.

## 2. The scan loop: one capture, every (bandwidth, SF) hypothesis

`Scanner::run_lora_listen_step()` in `src/scanner.cpp` runs once per scan
cycle while the Sub-GHz band is active, on one frequency from
`LORA_LISTEN_CHANNELS_HZ` (`config.hpp`) — or a single locked frequency if
"Lock to frequency" is set in the GUI.

1. Captures **once** at `profile.lora_listen_capture_rate_hz` (500 kHz —
   the largest bandwidth in `LORA_LISTEN_BW_LIST_HZ = {125e3, 250e3,
   500e3}`), for `lora_capture_seconds()` seconds (GUI-adjustable 1–30s,
   default `LORA_LISTEN_DURATION_S = 6.0`, capped at 32,000,000 samples).
2. For each bandwidth in the list, boxcar-decimates the single capture
   down (`decimate_boxcar()`) rather than re-capturing three times, and
   skips a bandwidth if the actual sample rate isn't a clean integer
   multiple of it.
3. For each of `LORA_LISTEN_SF_LIST = {5,6,7,8,9,10,11,12}`, calls
   `analyze_lora_packets(iq, sf, bw_hz, laboratory_mode)` and collects every
   returned row.
4. Every row with `header_valid == true` is pushed to the packet log
   directly. If **none** of the rows for this (bw, sf) decoded a header,
   the fingerprint/burst-detection path (§11) may contribute its own
   `"Detected only"` row instead (see §11 for exactly when).

`analyze_lora_capture()` (used by offline replay and `lora_replay`) does
the same bandwidth/SF sweep over a saved capture instead of a live one.

Each row is one **(SF, bandwidth) hypothesis**, not a deduplicated unique
packet — the same real transmission can and does produce multiple rows
across different SF/BW guesses tried against the same capture.

## 3. Detecting a packet — production path (`lora_receiver.cpp`)

Everything in this section and §§4–8 is `rfmon::lora::receiver::demodulate()`
and `decode_symbols()` in `src/lora_receiver.cpp`. It only supports
**explicit-header SF7–12**; SF5/6 can still produce burst evidence through
the older detector but are never decoded by this receiver
(`if(sf<7||sf>12||...) return {}` at the top of `demodulate()`).

**Preamble.** The whole capture is chopped into non-overlapping
`N = 2^sf`-sample windows and every window is dechirped once, coarsely
(no zero-padding), against the upchirp reference. A candidate preamble
start `i` is accepted only if **all** of the next `min_preamble_symbols`
windows (default 6) have a peak-to-total-energy ratio above
`minimum_peak_ratio` (default 0.5) **and** each consecutive pair's bin
doesn't drift by more than 1.5 bins.

**SFD.** From right after that confirmed run, the code scans forward
(up to `max_preamble_symbols`, default 256) dechirping each window against
the *downchirp* reference, and takes the first window whose downchirp
ratio both clears the 0.5 floor and exceeds that same window's own
already-computed upchirp ratio. That's the point where the signal stops
looking like "more preamble" and starts looking like "the SFD's
downchirps."

**Fine timing/CFO alignment.** This is the part worth being precise
about, because it's the least obvious mechanism in the file. Two *fine*
(8×-zero-padded) measurements are taken: `u`, the bin of a window
comfortably inside the preamble (4 symbols before the coarse SFD guess,
matched against the upchirp reference), and `d`, the bin of a window one
symbol into the coarse SFD guess (matched against the downchirp
reference). Per the file's own top comment, "both slopes resolve
timing/CFO up to an N/2 ambiguity" — `(d-u)/2` is used as a candidate
sample-position correction, tried at three possible half-symbol offsets
(`branch ∈ {-1,0,1}`) and three possible symbol offsets (`symbol ∈
{-1,0,1}`), nine candidates in all. Each candidate position is scored by
checking that a window 3 symbols earlier still looks like preamble *and*
the candidate position plus the next symbol both look like clean
downchirps — the candidate with the highest combined ratio (subject to
all three clearing the 0.5 floor) is `aligned`, the resolved start of the
SFD's first full downchirp. If nothing qualifies, the search resumes
further into the capture rather than getting stuck.

## 4. Frequency-drift correction

Once `aligned` is known, six preamble symbols well before it (offsets
−8 through −3 symbols) are fine-dechirped and fit with an ordinary
least-squares line: `bin ≈ slope·j + intercept`. `intercept` becomes the
packet's reported CFO in bins (`Packet::cfo_bins`); `slope` is bins of
drift **per symbol** (`Packet::drift_bins_per_symbol`), extrapolated
forward for ordinary payload symbols.

Two different correction strategies apply depending on which part of the
packet is being read:

- **Header symbols, and payload symbols under the LDRO hypothesis being
  tried:** a running accumulator tracks drift on the *reduced* (mod-4)
  lattice — each step's delta (wrapped into a 4-wide range) is folded into
  a signed offset that's applied to the next raw reading before decoding
  it. This is a different, symbol-by-symbol adaptive mechanism, not the
  fitted line.
- **Ordinary (non-LDRO) payload symbols:** the fitted `slope`/`intercept`
  line is applied directly by extrapolation (`v - slope*(k+2.25)`), no
  further per-symbol adaptation.

## 5. LDRO — both hypotheses tried, resolved by CRC when possible

LDRO (Low Data Rate Optimization) reduces the payload's usable symbol
alphabet by 2 bits — mandatory whenever symbol duration exceeds 16 ms.
The receiver computes `preferred = (2^sf / bandwidth_hz) > 0.016` and
tries `{preferred, !preferred}` in that order, fully decoding the packet
under each hypothesis (`decode_symbols(corrected, sf, ldro)`).

Critically, **the header always uses SF−2 bits and 4/8 coding regardless
of which payload-LDRO hypothesis is being tried** — LDRO only changes how
*payload* symbols beyond the header are read.

Whichever hypothesis produces a valid header is a candidate; among
candidates, one with a *valid CRC* wins outright, then one with a
*complete payload* wins, otherwise the first valid-header attempt is
kept. `Packet::ldro_ambiguous` is set whenever the winning attempt's own
CRC didn't actually confirm it (`!(crc_on && crc_valid)`) — this is what
shows as `On?`/`Off?` in the GUI's LDRO column, as opposed to a plain
`On`/`Off` when CRC settled the question.

## 6. Header decoding and its checksum

The first 8 symbols after the SFD are read, corrected (§4), rounded,
wrapped into `[0, 2^sf)`, divided by 4 (reducing to the SF−2 grid), and
put through a binary→Gray transform (`x ^ (x>>1)`). These 8 values are
diagonally deinterleaved at width `sf-2` into that many codewords, each
FEC-decoded at CR=4 (the header's fixed, most-robust rate) by brute-force
maximum-likelihood search over all 16 possible nibbles (`unfec()` in
`lora_receiver.cpp`) — ties (which a parity-only code like CR=1 can
produce, since it can detect but not correct a single flipped bit) are
resolved by keeping the raw received bits rather than guessing.

The first 5 resulting nibbles are the explicit header: nibbles 0–1 are
the declared payload length byte, nibble 2 packs coding rate (bits 1–3)
and a CRC-present flag (bit 0). The header is accepted only if CR is in
1–4 **and** a fixed 5-bit checksum computed from those three nibbles
(`header_sum()`, a hardcoded 5×12 binary parity-check matrix) matches the
checksum carried in nibbles 3–4. This exact checksum formula was
cross-checked against the independent `jkadbear/LoRaPHY` reference and
the reference's own published SF12/CR4 test vector — per
`docs/LORA_RECEIVER_UPGRADE.md`, that comparison caught and fixed a real
transcription error in this matrix before it was accepted.

If `sf-2 > 5` (true whenever SF ≥ 8, or SF7 with LDRO active on the
header's own SF-2=5 grid making it exactly 5), the *leftover* nibbles
from this same first block (indices 5 onward) aren't wasted — they're
already the start of the payload, still coded at the header's CR=4,
exactly the same "first block doubles as payload start" convention used
by the legacy SX-reference codec.

## 7. Payload decoding: FEC, packing order, dewhitening, physical CRC

Once the header validates, the receiver computes how many more payload
blocks are needed and checks the capture actually has that many symbols
(`"Header accepted; payload truncated."` if not). Each subsequent block
is `cr+4` symbols wide (the payload's own coding rate) and is
deinterleaved at width `sf` normally or `sf-2` under the LDRO hypothesis
being tried.

**Two details worth being exact about, because they're easy to get
wrong by assuming symmetry with the header:**

- Before the Gray transform, LDRO-payload symbols are reduced the same
  way header symbols are (divide by 4). **Ordinary, non-LDRO payload
  symbols are not reduced — instead they get a `-1` adjustment
  (`(bin + n - 1) % n`) that header/LDRO symbols never receive.** This
  asymmetry is exactly as coded; it isn't simplified away here because
  it's real and it's the kind of detail a wrong guess would silently
  corrupt.
- Nibble pairs are packed into bytes as `data[j] | (data[j+1]<<4)` —
  the **first** nibble becomes the byte's **low** nibble. The header
  packs the opposite way (`nib[0]*16 + nib[1]`, first nibble high). Two
  different packing conventions, both exactly as coded.

**Dewhitening** runs only over the payload's own declared-length bytes —
explicitly *not* over the trailing CRC bytes (`docs/LORA_RECEIVER_UPGRADE.md`
states this outright, and the loop bound `j<p.declared_payload_len`
confirms it in code). It's an 8-bit LFSR seeded at `0xFF`, with feedback
taken from bits 7, 5, 4 and 3 of the current state before each shift —
a different, independently-implemented whitening sequence from the
legacy internal codec's own 9-bit LFSR (taps at bits 0 and 5) in
`lora_phy.cpp`. They are not the same algorithm and shouldn't be assumed
interchangeable.

**Physical CRC** (`payload_crc()`) runs a standard CRC-16/CCITT shift
loop over all payload bytes except the last two, then folds the last two
bytes in directly by XOR rather than through the shift loop — the
well-known LoRa/Semtech convention where the trailing two bytes get
special treatment rather than being pure CRC-16 input. This is validated
against the received two CRC bytes (assembled low-byte-first) to produce
`crc_valid`. If CRC is off, `payload_complete` can still be true; only
`crc_valid` stays unset with a distinct, honest GUI/status label (see
§9) — an absent CRC is never treated as either a pass or a failure.

## 8. Sync-word observation — not a gate

Two symbols immediately before `aligned` (the sync-word symbols) are
re-measured and drift-corrected the same way. Each is divided by 8 and
rounded to a nibble; if **both** land within 1.5 bins of a clean multiple
of 8 **and** each nibble is in [0,16), `Packet::sync_word` is set to the
combined byte (`hi<<4 | lo`). Otherwise `sync_word` stays unset and the
raw (uncombined) bin values are shown instead.

This fixed "divide by 8, no SF-dependent shift" mapping is what the
independent LoRaPHY reference uses, and it's what the real TarangMini
hardware capture in `docs/LORA_RECEIVER_UPGRADE.md` actually decoded to
(`sync observation 0x12`) — a genuinely different convention from the
legacy SX-reference codec's SF-dependent shift (`nibble << (sf-4)`),
which never once matched real hardware in this project (see
`lora_handover.md`'s "Problem 1" for that codec's own unresolved history).

Production **never gates on the sync word at all** — a non-matching or
unquantized observation is retained and shown, not rejected. This is a
deliberate design choice stated directly in the upgrade doc ("Other sync
values do not block header decoding. Sync words do not identify a device
or prove LoRaWAN.") and is the single biggest behavioral difference from
the legacy SX-reference codec, whose sync check *did* gate decoding
(§10) and needed an explicit bypass to get past.

## 9. What each outcome means, end to end

`set_lora_integrity()` in `lora_observation.cpp` derives the row's status/
color from exactly two booleans (`payload_complete`, `crc_on`) plus
`crc_valid`:

| payload_complete | crc_on | crc_valid | Status | Meaning |
|---|---|---|---|---|
| false | – | – | Header only | Not enough samples for the payload; CRC never attempted |
| true | false | – | Payload / no CRC | Bytes recovered; header itself says there's no CRC to check — integrity unverified, not "passed" |
| true | true | false | Payload / CRC failed | Bytes are recovered but do not match the transmitted CRC — must not be treated as real data |
| true | true | true | Payload / CRC valid | Physical bytes match the CRC. This is a *bytes-match* guarantee only — no protocol, vendor, encryption or device-identity claim follows from it |

If no decoder produces a valid header at all for a given hypothesis, but
a preamble was still found (`lora::detect_burst()`), the row is
`"Detected only"` with no CR/LDRO/sync/CRC fields populated.

## 10. The legacy laboratory codecs (off by default)

Selected via the "Legacy laboratory codecs (nonstandard)" checkbox
(GUI) or `laboratory_mode=true` (API), routed through
`analyze_lora_laboratory()` in `lora_observation.cpp`:

1. **`lora_phy_std.cpp` (SX-reference).** Tried first. Implements LDRO,
   the same header-leftover-is-payload-start convention, and a sync-word
   check against `SYNC_WORD_DEFAULT = 0x12` using an SF-dependent shift
   (`nibble << (sf-4)`) — **this check gates decoding** (unlike
   production's observe-only sync in §8) and is why this codec needed the
   `skip_sync_check` bypass (`StdDecodedPacket::sync_check_skipped`,
   surfaced in the GUI as a `Valid*` amber marker meaning "checksum
   passed but sync word was never confirmed"). Laboratory mode always
   passes `skip_sync_check=true`, so this bypass is effectively always on
   whenever this legacy path runs at all.
2. **`lora_phy.cpp` (internal, nonstandard).** Tried only if (1) doesn't
   produce a valid header. A self-consistent codec — its own modulate()
   and demodulate() were designed together and validated against each
   other and this project's own real TX/RX loopback, not against any
   external reference. Uses its own CRC-16/CCITT (a plain one, no special
   trailing-byte XOR), its own 9-bit whitening LFSR, and Hamming decoding
   by brute-force nearest-codeword search — structurally similar in
   spirit to the production receiver's `unfec()` but a separate
   implementation with different parity formulas.

Both legacy codecs, and production, share the same fallback: if neither
decodes a header, `lora::detect_burst()` (also in `lora_phy.cpp`) reports
preamble-only evidence.

## 11. RF fingerprinting — a separate, parallel identity system

`fingerprint::extract_lora_fingerprint()` in `src/fingerprint.cpp` runs
**independently of whichever decoder is active**, fed by
`lora::detect_burst()`'s own alignment (not the production receiver's).
It only ever looks at the preamble, so it works whether or not the
header/payload ever decode:

1. **SNR gate**: computed from one full preamble's worth of energy
   against the one prior symbol's worth of pre-burst noise. Gated out
   below `LORA_SNR_FLOOR_DB = 10.0` dB.
2. **CFO refinement**: the codec's own integer-bin CFO estimate has up to
   ±0.5 bin of residual error; over a long real preamble that residual's
   accumulated phase drift would otherwise wreck everything downstream,
   so a Moose-style differential estimator refines it using the phase
   rotation between consecutive preamble symbols' correlations against a
   synthesized reference chirp.
3. **Widely-linear IQ-imbalance + DC-offset fit**: one 3×3 complex
   least-squares solve (`r' = μ·s + ν·conj(s) + c`) over up to
   `LORA_FP_MAX_FIT_SYMBOLS = 10` symbols, yielding `irr_db`, `iq_eps`,
   `iq_phi_deg`, `dc_dbc`, `dc_ang_deg`.
4. **Fit-quality gate**: `evm_pct` and `sync_corr` are computed regardless
   of the outcome and always recorded (even when gated out), but a fit is
   only accepted if `evm_pct <= LORA_EVM_CEILING_PCT` (75.0) and
   `sync_corr >= LORA_SYNC_CORR_FLOOR` (0.65) — both calibrated from real
   controlled TarangMini data, not the generic numbers a first pass
   assumed (see the constants' own comments in `fingerprint.hpp` for the
   calibration history).

In `run_lora_listen_step()`, this fingerprint's result is **only ever
shown as its own GUI row when no header decoded for that (SF, BW)
hypothesis** (`if (!decoded_present) push_row(...)`). If a header *did*
decode, the fingerprint is still computed, still logged to
`lora_fingerprints.ndjson`, and still fed into the persistent master list
below — it's just not visually merged onto the decoded packet's row,
because its alignment comes from a different detector and attaching it
to the decoded packet's evidence would misrepresent them as the same
measurement (the comment in `scanner.cpp` states this directly).

## 12. The persistent LoRa master list

`lora_master::LoraMasterList` (`src/lora_master.cpp`) only ever receives
*already-gated-in* fingerprint readings (never a rejected one). Matching
against existing devices uses **only** the quadrature/mixer group
(`irr_db`, `dc_dbc`, `iq_eps`, `iq_phi_deg`), each compared against the
median of that device's most recent `MATCH_MEDIAN_WINDOW = 20` readings,
within fixed tolerances (`IRR_TOLERANCE_DB=8.0`, `DC_DBC_TOLERANCE_DB=8.0`,
`IQ_EPS_TOLERANCE=0.02`, `IQ_PHI_TOLERANCE_DEG=3.0`). **`cfo_ppm` is
deliberately excluded from matching** — real controlled testing showed it
swinging tens of ppm burst-to-burst for one confirmed physical device,
most likely receiver-LO variance rather than device identity. If multiple
devices qualify within tolerance, the closest (weighted Euclidean-ish
distance) wins; if none do, a new device is created. Each device is a
single NDJSON file (meta line + reading lines), capped at
`MAX_READINGS_PER_DEVICE = 1000` readings with lazy compaction past 1100,
using atomic temp-file-then-rename writes.

## 13. Capture format and replay

`src/lora_capture.cpp` saves a versioned manifest (`schema:
"rfmon-lora-iq"`) plus raw little-endian float32 IQ, protected by an
FNV-1a64 checksum over the exact byte stream, capped at 32,000,000
samples (256 MB). Every save gets a fresh, never-overwritten directory. A
partial/failed write is cleaned up and leaves no manifest, so a directory
without a `manifest.json` was never a completed capture. Loading
re-validates the same bounds and re-checks the FNV hash before returning
anything. Offline replay (`draw_lora_replay_panel()` in `lora_gui.cpp`,
or the standalone `lora_replay` tool) runs the exact same
`analyze_lora_capture()` sweep as live scanning but never writes to the
fingerprint log or the persistent master list — replay results are
always kept separate from anything that would otherwise look like a live
observation.

## 14. Known, stated limits (not exhaustive here — see the source table)

`docs/LORA_RECEIVER_UPGRADE.md` carries a full limits table; the
highlights, faithfully summarized rather than re-derived:

- Explicit-header SF7–12 only; no implicit-header support (would need an
  out-of-band length/CR/CRC/LDRO profile — not implemented).
- Bandwidth hypotheses are fixed at 125/250/500 kHz via integer boxcar
  decimation, not a real channelizer — adjacent-channel interference can
  leak in.
- Normal-IQ only; inverted IQ (typical of LoRaWAN downlinks) isn't
  handled.
- The drift tracker assumes less than two bins of intersymbol drift and
  a linear drift model for ordinary payload — stronger drift, collisions,
  clipping or heavy interference aren't guaranteed to decode, and no
  BER/sensitivity claim is made anywhere in this code.
- The header checksum is 5 bits — a false-positive "valid" header on pure
  noise is possible, just improbable.
- CRC-valid bytes are a bytes-match guarantee only; nothing here decrypts,
  authenticates, or identifies a vendor/device from payload content.
- Real hardware validation to date is TarangMini at SF12/BW125/CR4/5
  specifically (per the upgrade doc's fresh-hardware test) — other
  SF/CR/LDRO combinations have synthetic/software test coverage
  (`tests/test_lora_receiver.cpp`'s 101 symbol vectors + 48 synchronization
  cases, both passing as of this writing) but not independent hardware
  evidence of their own.
