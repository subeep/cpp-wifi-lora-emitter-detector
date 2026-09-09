# C++ port status (working notes, most recent work first)

This file tracks implementation/validation status for this project at a
level of detail below what `README.md` carries (README is user-facing
docs; this is a build log for whoever - human or Claude - picks this
back up). See `newrocktest/SESSION_HANDOFF.md` (sibling project) for the
overall two-project goal and cross-project context.

**This project is not a git repository.** There is no commit history to
lean on here - this file and the code comments are the only record of
what changed and why.

## Latest work: X310 support, Connect button, and a real LoRa-codec bug found + fixed

Added a USRP B210 / USRP X310 device selector (`config.hpp`'s
`DeviceProfile`/`device_profile()`, `Scanner::set_device_type()`) and,
per explicit request, changed the device selector from "connects the
instant you click a radio button" to "select a device, then click a
separate Connect button" (`main.cpp` - `scanner_started` state, a
Connect/Reconnect button). Also fixed several real bugs surfaced by
live X310 testing (AGC not implemented on the UBX daughterboard, an
RFNoC streamer-reuse crash on band switches, a destructor throwing
during teardown of a fully-unresponsive connection, and a 56 Msps
request saturating the 1GbE link) - all documented inline in
`config.hpp`/`sdr_capture.cpp`/`scanner.cpp`.

**The most important one, because it's silent rather than a crash: the
LoRa PHY codec (`lora_phy.hpp`) hard-assumes capture sample rate ==
the transmitter's 125kHz channel bandwidth** (each symbol is exactly
`2^SF` *samples* - only true when those two are equal; see
`demodulate()`/`detect_burst()`, neither of which takes a rate
parameter at all). The B210 hits 125kHz exactly. The X310 cannot -
confirmed empirically (`uhd::multi_usrp::set_rx_rate`/`get_rx_rate`
round-tripped against several candidate rates), requesting 125kHz
clamps to ~196.08kHz, which is *not* a clean multiple of 125kHz, so
even decimating that down wouldn't reconstruct a correct 125kHz-
equivalent signal. Requesting **250kHz**, however, lands on exactly
`250000.00 Hz` - a clean 2x multiple. Fix: the X310's
`DeviceProfile::lora_listen_capture_rate_hz` is 250kHz (the B210's
stays 125kHz, unaffected), and `Scanner::run_lora_listen_step()`
decimates the result by 2 with a basic boxcar (moving-average)
anti-alias filter - not naive sample-dropping - before handing it to
the codec (see `decimate_boxcar()` in `scanner.cpp`). The decimation
factor is computed from whatever UHD *actually* returns
(`std::lround(actual_rate / LORA_LISTEN_SAMPLE_RATE_HZ)`), not assumed
from the profile, so it self-corrects if the achievable rate ever
changes.

This was NOT caught by the crash-resilience work alone - the app was
completely stable (no crashes, no RX-stalled warnings, clean cycle
counts) while silently detecting zero real packets, because the DSP
math itself was wrong, not the plumbing. Validated against real
hardware after the fix: a standalone diagnostic
(`/tmp/.../lora_x310_test.cpp`, not part of the repo) receiving at
250kHz+decimate-by-2 while a real TarangMini ST22LR01 transmitted 20
packets detected 4 of them (`DETECTED sf=7 preamble_len=39-40`,
matching the exact signature already validated on the B210 - see
`newrocktest/TARANGMINI_LORA_FINDINGS.md`), then re-confirmed live in
the actual GUI's LoRa packet table with a fresh burst.

## Earlier work: LoRa PHY decode ported to C++, wired into the GUI

Goal (user request): port the Python prototype's real LoRa PHY
decode/detect capability (`newrocktest/rf_monitor/lora_phy.py` +
`newrocktest/tools/lora_listen.py`) into this C++ app, and show the
packet list under the existing device table whenever LoRa (Sub-GHz)
mode is selected.

### What was built

- **`src/lora_phy.hpp` / `src/lora_phy.cpp`** - full C++ port of the
  Python codec: `LoRaParams`, `DecodedPacket`, `BurstDetection` structs;
  `modulate()`, `demodulate()`, `detect_burst()` in `namespace
  rfmon::lora`. Same constants as Python
  (`N_PREAMBLE=8`, `SYNC_WORD_DEFAULT=0x12`, `HEADER_CR=4`) - the sync
  word and preamble-measurement fixes from real TarangMini hardware
  testing (see `newrocktest/TARANGMINI_LORA_FINDINGS.md`) are carried
  over, not re-derived.
- **`Scanner`** (`src/scanner.hpp/.cpp`) extended with a LoRa PHY listen
  sub-step: once per scan cycle, **only while `BAND_SUB_GHZ` is the
  active mode**, it captures 2 seconds at 125kHz sample rate on one of
  the 3 mandatory IN865 channels (rotating channel each cycle, not all 3
  every cycle - keeps added per-cycle time to ~2s instead of 6s), tries
  `lora::demodulate()` for every SF in `{7..12}` independently (matching
  the Python tool's behavior exactly - not stopping at the first hit),
  falls back to `lora::detect_burst()` per-SF when decode fails. Results
  land in a thread-safe `lora_packet_log_` (capped at
  `LORA_PACKET_LOG_MAX = 200`, oldest dropped first), exposed via
  `Scanner::lora_packets()`.
- **`main.cpp`** - new `draw_lora_packet_table()` renders a 9-column
  table (Time, Status, Freq MHz, SF, CR, Len, CRC, CFO bins, Payload),
  color-coded status/CRC badges, most-recent-first, `—`/greyed-out for
  fields that don't apply to a "detected"-only row (no payload
  recovered). The main render loop now splits available vertical space
  40% device table / 60% LoRa table whenever LoRa mode is active
  (`show_lora_packets = (active_band == BAND_SUB_GHZ)`), 100% device
  table otherwise - so Wi-Fi 2.4/5GHz modes are visually unchanged.
- **`src/config.hpp`** - added `LORA_LISTEN_CHANNELS_HZ` (the same 3
  IN865 uplink channels as Python), `LORA_LISTEN_SAMPLE_RATE_HZ`
  (125kHz), `LORA_LISTEN_DURATION_S` (2.0), `LORA_LISTEN_SF_LIST`
  (7-12), `LORA_PACKET_LOG_MAX` (200).

### Validation performed

- **`tests/test_lora_phy.cpp`** (new, added to `CMakeLists.txt` as the
  `test_lora_phy` target) - direct C++ port of the Python synthetic
  test suite (10 round-trip cases: multiple SF/CR, noise, symbol-aligned
  timing offset). **10/10 pass, payload_len values exactly match the
  Python results** for every case.
- **Real-hardware cross-check** - a standalone ad-hoc tool
  (`test_real_tarang.cpp`, compiled directly against a raw `.cf32` dump
  of a real captured TarangMini transmission, not part of the CMake
  build) confirmed **byte-for-byte identical results to the Python
  codec**: `detect_burst()` returns `sf=7 start_sample=512 cfo_bins=-30
  preamble_len=40` in both languages, and `demodulate()` correctly
  returns `nullopt` in both (the known-unresolved header issue - see
  `newrocktest/TARANGMINI_LORA_FINDINGS.md` - reproduces identically in
  C++, which is itself a useful confirmation that the port is faithful
  rather than a new/different bug).
- **Full project rebuild** (`cmake --build build -j$(nproc)`) - clean,
  no errors, after all of the above.

### Bugs found and fixed during the port (not present in the Python original)

1. **Test-harness UB**: initial `tests/test_lora_phy.cpp` built payload
   vectors like
   `std::vector<uint8_t>(std::string("hello lora").begin(),
   std::string("hello lora").end())` - the two `std::string(...)` calls
   are two *separate* temporaries, so the begin/end iterators came from
   different objects (undefined behavior). Produced garbage
   (`payload_len=42` instead of 10). Fixed with a small `to_bytes(const
   std::string&)` helper, applied everywhere the test constructs a
   payload from a string literal.
2. **CMake linker error on `band_smoke_test`**: that target lists its
   sources explicitly (not via the `file(GLOB ...)` the main
   `rf_monitor_gui` target uses) and was missing `src/lora_phy.cpp`,
   causing `undefined reference to rfmon::lora::demodulate/detect_burst`
   once `scanner.cpp` gained a dependency on it. Fixed by adding the
   file to that target's source list in `CMakeLists.txt`. **If you add
   another new `.cpp` file that `scanner.cpp` (or anything else
   `band_smoke_test` links) depends on, check that target's explicit
   list too - it will NOT pick up new files automatically.**
3. **Python/C++ modulo semantics** (caught proactively while writing the
   port, not discovered as a runtime bug): Python's `%` always returns a
   non-negative result; C++'s `%` truncates toward zero and can return
   negative values for negative operands. Several CFO-relative bin
   calculations in the Python source rely on the always-non-negative
   behavior. Added a `pymod(a, n)` helper in `lora_phy.cpp` and used it
   everywhere the Python source used `%` on a value that can go
   negative. Not exercised by the synthetic test suite in a way that
   would have caught a mistake here (all synthetic CFOs happened to work
   out non-negative) - this was a "read the Python carefully" catch, not
   an empirical one, so treat it as slightly less battle-tested than the
   items above.

## UI panel: now visually confirmed live (previously the one open item)

`draw_lora_packet_table()` was confirmed rendering and populating
correctly in the actual running app (user-provided screenshot),
switched to LoRa (Sub-GHz) mode, real B210 hardware, real TarangMini
ST22LR01 transmissions. This closes what was previously this file's
"currently unverified" section.

This environment has no `xdotool`/root access to script UI clicks, so
verification was done by asking the user to click the mode button and
share a screenshot/description - not by an automated screenshot
pipeline. If a similar visual check is needed again, that is still the
approach (ask the user), not `xdotool` or X11 screen-grab tooling (PIL
`ImageGrab`, raw `xwd`, and the GNOME D-Bus screenshot method were all
tried against this host's session and each failed/was denied - see
git history around this note if that needs re-litigating).

## Frequency lock feature (added after the first live verification)

The first live test (real TarangMini connected, `lsusb`/`/dev/ttyUSB0`
confirmed) initially showed **nothing** in the LoRa table even with
real packets transmitting. Root cause, found by reading the module's
own TarangNet config API live: it's configured for **866.9 MHz**, not
any of the 3 generic `LORA_LISTEN_CHANNELS_HZ` entries - so the
round-robin scanner was only listening to it 0/4 cycles by chance (the
4th channel, 866.9 MHz, was added to `config.hpp` after this was found;
see `newrocktest/TARANGMINI_LORA_FINDINGS.md`). Even after adding it as
a 4th channel, the module was only listened to on ~1 cycle in 4,
producing sparse/inconsistent detections.

Added `Scanner::set_lora_lock_freq(std::optional<double>)` /
`lora_lock_freq()` (mutex-protected alongside `active_band_` etc.) and
a UI checkbox + MHz input ("Lock to frequency") shown only in LoRa
mode, in `main.cpp` right below the mode selector. When locked,
`Scanner::run()`'s LoRa listen step uses the locked frequency every
single cycle instead of rotating `LORA_LISTEN_CHANNELS_HZ` - confirmed
live: with the lock enabled at 866.9 MHz, a 15-packet real burst from
the TarangMini produced 8 consistent "Detected" rows (vs. sparse/zero
before locking), and the *wideband energy-detection* table (the
separate, coarser scan - see `draw_device_table`) also picked it up as
a "LoRa-like (BW~125kHz)" entry, since the radio now dwells on that one
frequency instead of only passing through it briefly.

Unlocking (`nullopt`) resumes the normal 4-channel rotation; nothing
about the default (unlocked) scanning behavior changed.

## Known limitations carried over from the Python prototype

Same list as `newrocktest/README.md`'s "Known limitations", plus the
TarangMini header-decode gap documented in
`newrocktest/TARANGMINI_LORA_FINDINGS.md`. `README.md` in this directory
still says "No protocol decode... energy detection only" under its own
"Known limitations" - **that line is now stale** as of this LoRa PHY
port and should be corrected (energy detection is still true for Wi-Fi,
but LoRa now has real PHY decode/detect).
