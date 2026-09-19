# LoRa handover — Milestone 2 (real-radio PHY decoding)

**Purpose of this file:** a standalone briefing for picking up the LoRa
side of this codebase where this session left it — what was built, what's
proven on real hardware vs. only in synthetic tests, every bug found (with
the reasoning that found it, since a wrong-but-plausible fix was found and
reverted at least twice), and the two major problems still open. Covers
only the LoRa PHY Milestone 2 work (`EXECUTE_NEXT.md`'s task spec). See
[HANDOVER.md](HANDOVER.md) for the general project handover (mostly
Wi-Fi) and [TARANGMINI_ASSESSMENT.md](TARANGMINI_ASSESSMENT.md) for the
hardware/firmware history this session built on.

Scope note: everything here happened on the standards-compliant codec,
`src/lora_phy_std.{hpp,cpp}` — the one that tries to match real
third-party hardware (TarangMini/ST22LR01). `src/lora_phy.{hpp,cpp}`
(this project's own self-consistent codec, validated only via its own
TX/RX loopback) was not touched. **No Wi-Fi file was touched at any
point this session** — verified via `git diff --stat -- src/bench
'src/wifi_*'` before every change.

---

## TL;DR — current state

- **Real LoRa signal decodes end-to-end for the header**, live, with a
  transmission Claude itself both sent and received: preamble detection,
  LDRO (Low Data Rate Optimization), and header checksum validation all
  work and are regression-tested. Confirmed on **7 independent real
  captures** across two sessions, two of which were captured by Claude
  running the RX side, and one of which Claude also transmitted into
  itself (both ends, no user involvement in the TX).
- **Payload CRC has never once validated on real hardware.** Two
  concrete hypotheses were tested live this session and both were
  disproven with real data (not just reverted on suspicion) — see
  [Problem 2](#problem-2-payload-crc-never-validates-unsolved) below.
  This is the single biggest remaining gap.
- **The sync-word value never matches** what this project's own
  formula expects, for a still-unknown reason — a diagnostic bypass
  (`LORA_STD_SKIP_SYNC_CHECK` / a GUI toggle) is required for anything
  past that point to run at all. See [Problem
  1](#problem-1-sync-word-value-mismatch-unsolved).
- Full regression suite (`test_lora_phy_std`, `test_lora_phy`,
  `test_lora_observation`, `test_lora_master`) passes with **zero
  failures** after every change made this session, including after two
  reverted dead ends.

---

## 1. Starting point and TX recovery (2026-09-18)

The user reported LoRa TX had stopped working after reflashing the
TarangMini board's firmware. Root cause: the board had genuinely been
reflashed successfully to `TarangNet_TN_LW_STD_WL_v0_0_6`, but
`tools/tarangnet_api.py`'s firmware whitelist only recognized
`TarangNet_TN_STD_WL_v0_0_4` — the tool's own safety gate was blocking
all writes/transmits on its own firmware, not a hardware problem.
Fixed by widening `SUPPORTED_FIRMWARE` to a set containing both. A real
transmission (module uplink ACK) was confirmed immediately after.

A second, unrelated blocker was found the same day: `uhd_usrp_probe`
reported "no devices found" for the X310 despite a healthy network
path. Cause: a stale process from earlier work was still holding live
UDP sessions to the radio. Killing it fixed discovery immediately —
not a firewall or hardware issue.

Both fixes and the diagnostic steps that found them are recorded in
[TARANGMINI_ASSESSMENT.md](TARANGMINI_ASSESSMENT.md)'s dated sections.

## 2. First real signal capture and tooling (2026-09-19)

No GUI automation was available, so a small finite-capture CLI tool was
built: `tools/lora_capture_cli.cpp` (6-attempt/1200ms UHD connection
retry, matching `Scanner::connect_sdr()`'s established pattern; upfront
duration/sample-cap validation before connecting) and
`tools/lora_capture_crop.cpp` (crops a saved capture to a window,
reusing `lora_capture.cpp`'s validated save/load code rather than
hand-rolling the format). `src/lora_capture.cpp`'s sample cap was
raised from 16M to 32M samples (256MB) after a 60s capture at 500ksps
produced 30M samples that the old cap silently rejected *after* the
capture had already run — the RF data was unrecoverable that one time;
the cap raise and an upfront duration check in the CLI tool together
prevent a repeat.

Four real captures from two TX sessions were saved and cropped into
tracked fixtures:
`tests/fixtures/lora_m2/session-a-2026-09-19-first-signal/cropped/`.
All four showed the identical failure signature at the time: preamble
detected, but synchronization/header never validated.

## 3. Preamble-length measurement bug #1 (2026-09-19, before this session's live work)

**Symptom:** `measure_preamble_length()` terminated at 8-11 symbols on
real captures instead of the true ~40, because it required an *exact*
integer-bin match against a single fixed CFO estimate, and real
residual CFO drifts slowly (~1 bin per 8-9 symbols) across a long real
preamble.

A first fix (a flat, wider tolerance) was tried and **reverted** — it
broke the synthetic SF5/SF6/SF7 regression cases, because a fixed sync
word's own first symbol lands only 2-4 bins from zero at low SF, inside
a tolerance sized for this capture's ~4-bin drift. The working fix:
adaptive tracking where the reference (`target`) updates to the last
accepted symbol, and only the **per-step** delta is bounded
(`PREAMBLE_DRIFT_STEP_BINS = 1`) — this follows arbitrary cumulative
drift correctly while still rejecting a real transition to sync-word
content, which is always a single large jump. This is the version that
shipped, and it correctly measured preamble length on all 4
2026-09-19 fixtures.

At this point, with the sync-check bypassed as a diagnostic, the
header decode was deterministic and reproducible across captures but
its checksum never validated — the leading hypothesis was LDRO, which
had never been implemented anywhere in this codebase.

---

## 4. This session's work

### 4.1 LDRO (Low Data Rate Optimization) implementation

LDRO reduces the usable symbol alphabet from `2^SF` to `2^(SF-2)` bits
— mandatory per the LoRa Alliance regional parameters whenever symbol
duration exceeds 16ms, which SF12/BW125 (this transmitter's config,
~32.8ms/symbol) clearly exceeds. Never modeled anywhere in this file
before. Mechanism, transcribed from the reference implementation's own
source (`LoRaEncoder.cpp`/`LoRaDecoder.cpp`,
https://github.com/myriadrf/LoRa-SDR — this project's chosen reference,
which itself doesn't name "LDRO" but exposes the exact primitive: a
`PPM` parameter, `PPM <= SF`, applied uniformly to header+payload
interleaving):

- TX (`modulate()`): `ppm = ldro ? sf-2 : sf`; after gray-encoding,
  `sym <<= (sf - ppm)`.
- RX (renamed to `demodulate_with_ppm(iq, sf, ppm, skip_sync_check)`):
  before gray-decoding, `b += (1<<(sf-ppm))/2; b >>= (sf-ppm);` —
  round-then-shift, the exact inverse.
- Public `demodulate(iq, sf, skip_sync_check=false)`: tries `ppm=sf`
  (LDRO off, unchanged legacy behavior) first; only retries
  `ppm=sf-2` if that header doesn't validate. A non-LDRO transmitter's
  packets take exactly the same codepath as before this change.
- `StdDecodedPacket::ldro` records which hypothesis actually validated
  — inferred provenance, not a signaled transmitter fact (TarangNet's
  API never exposes an LDRO setting).

**Decisive result:** with the sync-check bypassed, enabling LDRO made
the header checksum validate on all 4 real fixtures, converging on the
**identical** header value (`hdr=27 03 1b` → payload_len=39, CR=4/5,
CRC-on) across two independent capture sessions. Before LDRO, the
checksum never validated and the raw header bits drifted between
sessions. 3 new synthetic `ldro=true` round-trip tests added to
`tests/test_lora_phy_std.cpp` (SF11/SF12, various CR) — all pass,
confirming both the bit-exact round-trip and that the auto-fallback
correctly reports `ldro=true` provenance without a caller ever passing
LDRO explicitly.

Full writeup: [data/lora_m2/2026-09-19-ldro/ldro-implementation.md](data/lora_m2/2026-09-19-ldro/ldro-implementation.md)

### 4.2 GUI diagnostic toggle for the sync-word gate

The user asked to be able to see header decode results live without
permanently weakening the tool's integrity guarantees. Added:

- `Scanner::set_lora_skip_sync_check(bool)` / `lora_skip_sync_check()`
  (`src/scanner.hpp/.cpp`) — mutex-protected runtime state, same
  pattern as the existing `lora_lock_freq_`.
- A **"Skip sync-word check (diagnostic)"** checkbox next to "Lock to
  frequency" in `src/main.cpp`, and a matching one in the offline
  replay panel (`src/lora_gui.cpp`) — both default **off**.
- `bool skip_sync_check` threaded through `demodulate()` →
  `analyze_lora_hypothesis()` → `analyze_lora_capture()`, independent
  of (and equivalent to) the pre-existing `LORA_STD_SKIP_SYNC_CHECK`
  env var used by CLI tooling — either bypasses the gate.
- `StdDecodedPacket::sync_check_skipped` / `LoraPacketRow::
  sync_check_skipped` propagate whether a given row's header validated
  this way. The packet table shows **`Valid*`** (amber, not the normal
  green `Valid`) with a tooltip explaining the header checksum passed
  but was *not* cross-checked against the sync word — so this can
  never be mistaken for a fully-verified decode later.

Verified end-to-end with a standalone test binary: default and
explicit-`false` behavior is byte-identical to before this change;
explicit-`true` with **no env var set** correctly bypasses the gate on
its own.

### 4.3 Live TX/RX validation — Claude ran both ends itself

The user was (rightly) skeptical that a fix validated only against 4
fixtures from one capture session might be overfit. This session ran
**five live capture rounds**, escalating in how much Claude controlled
directly:

1-3. Opened USRP capture windows itself while the user ran the
   TarangMini TX script on their own machine (Claude has no USB
   passthrough to the module's serial port from its sandbox — only the
   USRP is reachable, over Ethernet). Two of these attempts caught
   nothing because the user's TX command failed first (wrong
   directory, then a re-enumerated serial port — the same USB-identity
   flapping documented in `TARANGMINI_ASSESSMENT.md`). The fourth
   attempt caught a real packet.
4. Mid-session, Claude discovered it now had direct read/write access
   to `/dev/serial/by-id/...` (not true earlier in the same session —
   environment access can change). From this point, Claude ran **both**
   `lora_capture_cli` (RX) and `tarangmini_sf_bw_sweep.py` (TX) itself,
   fully self-contained, for two more independent rounds — one used
   for the payload-CRC investigation's gain experiment (§4.5).

The first Claude-run-TX capture exposed **preamble-length bug #2**
(different from §3's bug): a single-symbol *reversal* mid-drift
(`...,-2,-2,-1,-3,-3,...`) made the step=1 tracker's instant
snap-to-last-reading lock onto the wrong reference, cutting
measurement short at 24 symbols instead of 40. Fixed with a
confirm-based tolerance: a symbol landing exactly 2 bins from target
(`PREAMBLE_MAYBE_STEP_BINS = 2`) is judged by what the **next** symbol
does — reverting near the old target means noise (skip it), staying
near the ambiguous symbol's own position confirms a genuine drift step
(accept both), anything else is a real transition (do nothing, exactly
as before). A flat widen to 2 was never tried — already known unsafe
from §3's regression.

A second Claude-run round (both TX and RX by Claude) exposed
**preamble-length bug #3**, a genuine bug in bug #2's own fix: the
"skip as noise" branch advanced `pos` past the ambiguous symbol
without incrementing the returned `count`, silently under-counting the
preamble length by one per skip. Found by simulating the exact logic
in Python against the raw trace *before* touching C++ (fast iteration,
same discipline as everywhere else this session) — fixed by
incrementing `count` in that branch too, since the skipped symbol is
still real preamble airtime the caller must advance past.

**End state:** all 7 real captures across two sessions (4 from
2026-09-19, 3 from this session, one of which Claude both transmitted
and received) show the identical `sync_bins_raw` pattern and the
identical validated header `hdr=27 03 1b`. Two of the new captures are
saved as tracked, re-verified-after-cropping fixtures:
`tests/fixtures/lora_m2/session-b-2026-09-20-preamble-fix/`.

Full writeup: [data/lora_m2/2026-09-20-live-validation/live-validation-and-preamble-fix.md](data/lora_m2/2026-09-20-live-validation/live-validation-and-preamble-fix.md)

### 4.4 Two small diagnostics kept (zero production effect when unset)

- `LORA_STD_PAYLOAD_TRACE` — per-payload-symbol raw bin + dechirp
  ratio, mirroring the header's existing `raw_bins` debug line.
- `LORA_STD_DEBUG`'s existing dump was extended with a `got_crc`/
  `want_crc`/`bit_diff` line — the Hamming distance between the
  received and computed payload CRC, out of 16 bits.

### 4.5 Payload CRC investigation — two hypotheses tested live, both disproven

See [Problem 2](#problem-2-payload-crc-never-validates-unsolved) for
the full detail; summary:

1. **SFD-position CFO refinement** — implemented, regression-tested
   clean, then **immediately falsified live**: the "residual" it
   computed (~1800 bins out of 4096) was implausible, and using it
   broke a header that had been validating reliably. Root cause of the
   wrong premise: dechirping a downchirp against an upchirp reference
   doesn't produce a clean CFO measurement the way same-slope dechirp
   does — confirmed against this session's own earlier trace data,
   where the SFD's dechirp-vs-upchirp peak lands near bin ~2200 even
   on fixtures that otherwise decode correctly. **Cleanly reverted**;
   regression suite re-confirmed clean; header decode restored to
   exactly its prior state.
2. **Higher RX gain** — tested live (25dB vs. the usual 5dB, zero code
   changes, cheapest possible test). Result was the **opposite** of
   the hypothesis: payload dechirp ratios got worse (0.41-0.66, down
   from 0.5-0.77), and a spurious second decode appeared at a
   different SF with the sync word matching exactly for the first time
   all session — a strong signature of receiver saturation from a
   physically close transmitter, not a weak signal. Rules out "just
   add gain" as the fix; the tool's existing conservative default gain
   is closer to correct, not further away.

Full writeup: [data/lora_m2/2026-09-20-live-validation/payload-crc-investigation.md](data/lora_m2/2026-09-20-live-validation/payload-crc-investigation.md)

---

## 5. Major unsolved problems

### Problem 1: sync-word value mismatch (unsolved)

`recovered_sync` never matches `SYNC_WORD_DEFAULT` (0x12) on any real
capture — `sync_bins_raw` consistently reads `(4,12)` or `(5,13)`
(varying by ~1 bin between sessions, same order of variation seen
elsewhere), but the formula `((bin0 >> (sf-4)) << 4) | (bin1 >>
(sf-4))` needs bins in the hundreds to read any nonzero nibble at
SF12 — these small values always compute to `recovered_sync=0x00`.

What's ruled out: LDRO doesn't explain it (its two-stage ppm-shift
algebraically cancels out for sync-word decoding — verified in
`data/lora_m2/2026-09-18-tx-recovery/sync-and-header-investigation.md`
before LDRO was even implemented). Position is confirmed correct via
`LORA_STD_TRACE2` (genuinely upchirp-shaped, right between preamble and
SFD, not still-preamble or already-SFD).

What's suspected but not confirmed: `SYNC_WORD_DEFAULT=0x12` and its
shift formula were validated only against this project's **own**
TX/RX loopback (`lora_phy.hpp`'s self-consistent codec) — never
independently against TarangNet's actual chip. TarangNet is a
proprietary stack layered on LoRa PHY (see `lora_phy_std.hpp`'s
`demodulate_implicit()` comment) and may simply use a different sync
word, or a different bin-to-nibble mapping than this formula assumes.

**Current workaround, not a fix:** `LORA_STD_SKIP_SYNC_CHECK` (env var)
and the GUI toggle (§4.2) bypass this gate for diagnosis. Both default
to leaving the gate enforced.

**Concrete next steps, not yet tried:**
- Stop assuming the sync word value; derive it empirically from the
  consistent real observations instead of comparing against 0x12.
- Replace the exact-equality gate with a bounded-tolerance comparison
  on raw bins (same philosophy as the preamble-length fixes) rather
  than a lossy quantized formula.

### Problem 2: payload CRC never validates (unsolved)

Every real capture this session and the last — 7 total — has a
validating header and a failing payload CRC. Evidence gathered:

- Per-symbol payload dechirp ratio trends down across the packet
  (~0.75 near the header → ~0.5, occasionally ~0.46, by the end) — a
  real, consistent trend.
- Decoded payload bytes are highly **reproducible** across captures: a
  ~17-byte stable prefix and ~15-byte stable suffix match almost
  exactly across 6 independent real captures from two sessions, with
  only a middle section varying (consistent with a legitimate
  per-transmission counter in TarangNet's own framing, not decode
  noise).
- CRC bit-difference (`got_crc XOR want_crc`, popcount/16) across 7
  captures: 5, 6, 7, 7, 9, 10, 11 — centered right around 8/16, what
  pure chance gives for two *unrelated* 16-bit values. This argues
  against a few scattered noise bits (would cluster near 0) and
  against one fixed structural/indexing bug (would reproduce the same
  bit_diff every time).
- This session's own synthetic round-trip test at the *identical*
  packet shape (SF12, LDRO, CR1, 39-byte payload, the same 8-block
  "more" structure) passes perfectly with zero injected noise — the
  FEC/interleave/whitening pipeline itself is not suspected as buggy.

**Two hypotheses tested live, both disproven** (§4.5): SFD-position CFO
refinement (wrong premise, reverted), higher RX gain (wrong direction,
receiver saturation instead).

**What remains plausible, not yet attempted:**
- LoRa's own inherent symbol error rate at CR 4/5 (the transmitter's
  own choice — single-parity Hamming, **detects** but does not
  **correct** a bit error) may simply be non-zero at this link's real
  conditions, even though raw gain/SNR isn't the lever (§4.5 rules that
  out). A soft-decision (LLR-weighted) FEC decode, using the dechirp
  ratio/energy-spread already computed per symbol, could tolerate this
  without needing every symbol decided perfectly — a materially bigger
  change than anything tried this session.
- A genuine sample-**timing** drift (distinct from a bin-domain
  carrier-frequency CFO) between the USRP's clock and the TarangMini's
  own crystal, compounding over the packet's ~3s airtime, would need
  proper resampling/timing-recovery — not a bin-domain correction like
  the (wrong) SFD attempt.
- May or may not be connected to Problem 1 — not established either
  way.

---

## 6. File map (LoRa M2 scope only)

```
src/
  lora_phy_std.{hpp,cpp}   The standards-compliant codec - almost all of
                           this session's work. hpp: 136 lines. cpp: 992
                           lines. Currently 95-line uncommitted diff on
                           top of the last commit (0911971) - see below.
  lora_observation.{hpp,cpp}  Shared per-hypothesis reporting (LoraPacketRow).
                           ldro/sync_check_skipped fields added this session.
  lora_gui.{hpp,cpp}       Packet table + offline replay panel. LDRO column,
                           Valid*/amber marker, skip-sync-check checkbox
                           (both live and replay panels) added this session.
  lora_capture.{hpp,cpp}   Capture save/load format. Sample cap raised
                           16M->32M this session (2026-09-19).
  scanner.{hpp,cpp}        run_lora_listen_step() - the live listen loop.
                           lora_skip_sync_check_ state added this session.
                           NOTE: LORA_LISTEN_DURATION_S = 2.0s (config.hpp)
                           - shorter than one real SF12 packet's ~3s
                           airtime. Not fixed this session; flagged as a
                           likely contributor to why the LIVE GUI rarely
                           catches a complete real packet even though
                           offline replay of longer manual captures does.
  main.cpp                 The "Skip sync-word check (diagnostic)" checkbox.

tools/
  lora_capture_cli.cpp     Finite receive-only capture (6-attempt retry,
                           upfront duration validation).
  lora_capture_crop.cpp    Crops a saved capture to a window + margin.
  lora_replay.cpp          Offline decode of a saved capture, no hardware.
  tarangnet_api.py         Firmware whitelist widened (2026-09-18).
  tarangmini_sf_bw_sweep.py  TX sweep script - docstring updated only.

tests/
  test_lora_phy_std.cpp    12 cases (9 original + 3 new LDRO cases).
  fixtures/lora_m2/
    session-a-2026-09-19-first-signal/cropped/   4 real captures.
    session-b-2026-09-20-preamble-fix/           2 real captures (this
                                                  session, re-verified
                                                  after cropping).

data/lora_m2/   (gitignored - evidence/session logs, not shipped code)
  2026-09-18-tx-recovery/     TX/discovery fixes, first-signal capture,
                              preamble bug #1, sync-word investigation.
  2026-09-19-ldro/            LDRO implementation and real-fixture result.
  2026-09-20-live-validation/ Live TX/RX rounds, preamble bugs #2/#3,
                              payload CRC investigation (both dead ends).

EXECUTE_NEXT.md            The governing Milestone 2 task spec (pasted by
                            the user; not modified, only followed).
TARANGMINI_ASSESSMENT.md   Hardware/firmware history, appended to this
                            session with dated sections.
```

**Uncommitted as of this handover:** `src/lora_phy_std.cpp` only (95
lines changed on top of commit `0911971`) — everything else described
above (the capture tooling, `lora_observation.*`, `lora_gui.*`, the
fixtures) was already committed in `0911971` earlier in this session.
The uncommitted diff is LDRO + the GUI-toggle plumbing on the codec
side + both preamble-length fixes + the two payload-CRC diagnostics +
the reverted SFD attempt (net zero — reverted cleanly, verified via
diff review).

---

## 7. How to reproduce / rebuild / test

```bash
cd build
cmake --build . --target test_lora_phy_std test_lora_phy test_lora_observation \
  test_lora_master lora_replay rf_monitor_gui wifi_test_bench -j$(nproc)

for t in test_lora_phy_std test_lora_phy test_lora_observation test_lora_master; do
  ./$t
done
```

Decode a real fixture offline, seeing the full diagnostic picture:

```bash
LORA_STD_SKIP_SYNC_CHECK=1 LORA_STD_DEBUG=1 LORA_STD_PAYLOAD_TRACE=1 \
  ./lora_replay ../tests/fixtures/lora_m2/session-b-2026-09-20-preamble-fix/capture-1789813719649889822-0
```

Live: connect the X310 (`addr=192.168.10.2`) and the TarangMini's
serial port (`ls /dev/serial/by-id/` — the exact FTDI identity has been
observed to flap between two names across sessions; re-resolve it each
time rather than assuming a cached path). `tools/tarangmini_sf_bw_sweep.py
--inspect` is a safe read-only check before transmitting. The GUI's
"Lock to frequency" should match the module's own reported frequency
(read via `--inspect` — it has drifted unexplained between sessions
before, always re-confirm rather than assuming 865.9 MHz).

---

## 8. Suggested next steps

Roughly in the order they'd naturally come up — not a strict ranking:

1. **`LORA_LISTEN_DURATION_S` (2.0s) vs. real SF12 packet airtime
   (~3.0s+)** — flagged in §6 but not investigated or fixed this
   session. If real, this alone could explain why the *live* GUI
   rarely catches a complete packet even now that the decode logic
   itself works (confirmed via longer, manually-coordinated captures).
   Cheap to check: does raising this constant (with matching capture
   duration/UI implications) increase live catch rate on real traffic?
2. **Payload CRC** (Problem 2) — the actionable next step is almost
   certainly a bigger one than anything tried this session: either
   soft-decision FEC decoding, or genuine sample-timing recovery. Don't
   repeat the SFD-bin-position idea (falsified) or "just raise gain"
   (falsified, wrong direction).
3. **Sync-word value mismatch** (Problem 1) — try deriving the real
   sync word empirically from the now very-consistent real observations
   instead of assuming `0x12`, and/or replace the exact-match gate with
   a bounded-tolerance comparison on raw bins.
4. Whichever of the above lands first should be re-validated the same
   way this session did: live, with Claude (or whoever) running both
   TX and RX when possible, not just replaying saved fixtures — this
   is what caught 2 of the 3 preamble-length bugs this session that
   the original 4 fixtures alone never exposed.

---

## Receive-only interoperable PHY upgrade (current session)

Implemented a new production `lora_receiver` path, shared by live monitoring and replay. The old SX-reference/internal codecs now require an explicit **Legacy laboratory codecs (nonstandard)** opt-in. Existing uncommitted changes to `src/lora_phy_std.cpp` were preserved. Wi-Fi was not changed.

**Verified result:** all six existing cropped hardware recordings now recover the entire expected 39-byte packet with matching physical CRC. A fresh TarangMini/X310 recording also recovered a different 43-byte packet containing `RX_UPGRADE_19SEP_A7`, with CRC valid and sync observation `0x12`. The board read back 865.900 MHz, rate `0x00`, root mode; those settings were verified unchanged. One initial UHD management timeout returned zero samples; the single retry captured 20 seconds without overflow.

The key fixes are two-slope SFD timing alignment, fractional bin/drift tracking, an SF−2/CR4 header block independent of payload LDRO, correct byte-level whitening and physical CRC conventions. Payload length/CR/CRC presence come from the header. Sync is observed, not whitelisted. Multiple packets are recovered per hypothesis. The GUI shows observed sync and unresolved LDRO hypotheses; the LoRa capture window is now adjustable from 1–30 seconds, default six.

Corrections to earlier hypotheses: upchirps multiplied by downchirp references (and the reverse for SFD) are valid dechirping; this implementation does not conjugate again inside that multiplication. Self-roundtrip tests did not prove OTA conformance. CRC bit distance alone did not establish gain/noise as the cause. The successful recorded decodes required no gain increase.

Validation: 101 fixed symbol vectors including a published independent LoRaPHY example; 48 synthesized synchronization cases across SF7–12/all CRs/both LDRO settings/nondefault sync; multi-packet, partial/erased, silence/noise tests; exact-payload regression on six local hardware recordings; fresh hardware replay; observation/capture tests; offscreen rendering and visual inspection with the fresh hardware row. Hardware corpus is locally ignored by Git; portable symbol vectors are separate and available to a clean checkout.

See **[docs/LORA_RECEIVER_UPGRADE.md](docs/LORA_RECEIVER_UPGRADE.md)** for commands, provenance, evidence, unchanged hardcoded assumptions and remaining pitfalls. This is not yet universal LoRa support: implicit headers, inverted IQ, other bandwidths, LoRaWAN MAC parsing/decryption, continuous reception and cross-vendor RF verification remain. Restart the rebuilt GUI and lock to the board's current read-back frequency when testing.
