# B210 / X310 Emitter Monitor - C++ / Dear ImGui port

A C++ port of a Python prototype (a separate project) that reads IQ
samples from a USRP and reports currently-active LoRa/Wi-Fi emitters via
energy detection. This version adds a live desktop UI (Dear ImGui) with
a switch between three band modes - **LoRa (Sub-GHz)**, **Wi-Fi 2.4GHz**,
and **Wi-Fi 5GHz** - each showing its own persistent device list, plus a
**device selector** to run against either a **USRP B210** (USB3) or a
**USRP X310** (Ethernet) - see "Multi-device support" below for what
differs between them.

**This is energy detection, not protocol decoding.** It tells you
*something is transmitting at this frequency with this bandwidth*, not
*this is device X sending payload Y*.

## Requirements

- A **USRP B210** connected via USB3 (USB2 will work but at reduced safe
  sample rates), **or** a **USRP X310** reachable over Ethernet (see
  "Multi-device support" below for the network setup this needs) - the
  app can be pointed at either one via its device selector, but only one
  physical radio is used at a time.
- UHD + `libuhd-dev` (C++ headers)
- `libglfw3-dev`, an OpenGL dev package, `libkissfft-dev` (single-precision FFT)
- CMake 3.16+, a C++17 compiler

## Building

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

Produces three binaries in `build/`:

| Binary | What it is |
|---|---|
| `rf_monitor_gui` | The live desktop app |
| `test_dsp` | Synthetic-signal correctness check for the DSP pipeline - no hardware needed |
| `band_smoke_test` | Manual hardware validation tool - drives all three bands in turn and prints what each saw (takes ~90s, needs the real B210) |

## Running

```bash
./build/rf_monitor_gui
```

Opens a window with:
- An **SDR device selector** (USRP B210 / USRP X310) - switching fully
  restarts the background scan thread against the newly-selected
  hardware (a different physical radio, so there's no "hot swap" - see
  "Multi-device support" below).
- A **band mode selector** (LoRa / Wi-Fi 2.4GHz / Wi-Fi 5GHz) - switching
  changes which scan plan the background thread is actively running.
  Each band keeps its own device list, so switching away and back
  doesn't lose what was found; the other two bands' lists simply stop
  updating while not selected (only one scan can run at a time - one
  physical radio).
- A **threshold slider** (dB above the noise floor required to flag a
  detection) and a **gain control** (fixed dB, or AGC).
- A **frequency map** (color-coded by protocol guess) and a **sortable
  table** with every field: frequency, bandwidth, power, cycles seen,
  age, last seen, band.

## Architecture

```
src/
  config.hpp        - band/scan-plan constants, all three modes, plus
                       DeviceProfile/device_profile() - the B210 vs X310
                       device args, antenna, gain range, and sample-rate
                       cap (see "Multi-device support" below)
  spectrum.hpp/.cpp  - max-hold spectrogram (KissFFT) + DC/edge guard masks
  detector.hpp/.cpp  - threshold segmentation, DC-gap bridging
  classifier.hpp/.cpp- bandwidth-based protocol guess, per band
  registry.hpp/.cpp  - per-band device tracking with expiry (thread-safe)
  sdr_capture.hpp/.cpp - UsrpCapture: UHD C++ API wrapper for finite IQ
                       capture, device-agnostic (device args/antenna/
                       gain/channel all passed in) - used for both the
                       B210 and the X310
  scanner.hpp/.cpp   - background thread: owns the SDR + 3 registries,
                       scans whichever band is "active," thread-safe
                       getters/setters for the UI, connect retry + a
                       mid-run auto-reconnect if RX stalls
  main.cpp           - GLFW + OpenGL3 + Dear ImGui app, reads Scanner
tests/test_dsp.cpp   - synthetic correctness check (ported from the
                       Python prototype's own test suite, same cases,
                       same tolerances)
tools/band_smoke_test.cpp - manual real-hardware validation across all
                       three bands via the same Scanner API the GUI uses
third_party/imgui/   - Dear ImGui, vendored at v1.92.9
```

Detection pipeline per scan step (same as the Python prototype): take
several short IQ captures, build a max-hold spectrogram (short-time
Hann-windowed FFTs, per-bin maximum across time *and* across captures -
preserves a short/intermittent burst instead of averaging it toward the
noise floor), mask out the DC-guard and Nyquist-edge regions, find
contiguous above-threshold runs, classify by bandwidth, hand the result
to that band's registry (buckets by frequency so the same emitter seen
repeatedly, or by overlapping capture windows, collapses to one entry
with a hit count).

## Scan plan

| Mode | Coverage | Steps | Sample rate (B210) | Sample rate (X310) |
|---|---|---|---|---|
| LoRa (Sub-GHz) | 863-868 MHz (IN865 + margin) | 1 | 5 Msps | 5 Msps |
| Wi-Fi 2.4GHz | 2401-2484 MHz (channels 1-13) | 3 | 56 Msps | 20 Msps (capped) |
| Wi-Fi 5GHz | UNII-1 (ch 36-48) + UNII-3 (ch 149-165) only | 5 | 56 Msps | 20 Msps (capped) |

The requested rate is always clamped to whichever device is actually
connected (`DeviceProfile::max_sample_rate_hz`) - see "Multi-device
support" below for why the X310's cap is so much lower than the B210's.

**5GHz DFS channels (UNII-2/2e, 5250-5725 MHz) are not scanned** -
those require radar-detection compliance and are used far less often by
consumer APs than UNII-1/3; covering them would roughly triple the
number of 5GHz scan steps for typically-quiet spectrum. Extending
`WIFI_5G_CENTERS_HZ` in `config.hpp` covers more if needed.

## Hardware notes

Parameters (sample rates, DC-guard fraction, edge-guard fraction, gain)
are carried over from a Python prototype already validated against this
same B210 (serial 3273AC6): USB3-clean up to 56 Msps, a ~40-50dB
LO-leakage spike sits exactly on every tuned center frequency (`set_rx_dc_offset`/
`set_rx_iq_balance` do not remove it - the DC guard band is mandatory,
not optional), and a filter-rolloff artifact sits near the Nyquist edge
of every capture (also guarded out). See the Python project's README
for the original measurements.

**Fixed gain (40dB default), not AGC**: AGC was empirically too
conservative to surface a real nearby Wi-Fi AP that fixed 40dB gain
found cleanly. AGC is still available via the UI checkbox if the RF
environment is too hot for a fixed gain.

**Real-hardware cross-check**: the same 2.4GHz Wi-Fi APs (channel 5 and
6) that the original Python prototype found are reliably found by this
C++ port too - same frequencies, same rough power levels, tracked with
a growing `hit_count` across many real scan cycles (see
`tools/band_smoke_test.cpp` output).

**5GHz is new and less calibrated than 2.4GHz.** A `band_smoke_test`
run found four recurring narrowband emitters in UNII-1 (5182, 5207,
5213, 5252 MHz, all persistent across every cycle) with measured
bandwidths of 1.7-3.7 MHz - too narrow to clear the `WIFI_5G_CHANNEL_BW_LO_HZ`
(15 MHz) threshold, so they're honestly reported as "Unknown 5GHz
emitter" rather than guessed as Wi-Fi. Unlike the 2.4GHz case (where the
bandwidth threshold was lowered based on a *cross-validated* real AP
seen consistently by two overlapping capture windows), there isn't yet
equivalent confirmation for 5GHz - it's plausible these are a weak/far
5GHz AP (the phenomenon of a weak signal's threshold-measured bandwidth
shrinking well below its true channel width was already observed at
2.4GHz), but it could just as easily be something else entirely.
Treat `WIFI_5G_CHANNEL_BW_LO_HZ`/`WIFI_5G_CHANNEL_BW_HI_HZ` in
`config.hpp` as a first guess, not a calibrated value - narrow it or
widen it based on your own environment.

## Multi-device support (USRP B210 / USRP X310)

Both devices go through the same `UsrpCapture` wrapper (`sdr_capture.hpp`/
`.cpp`) - it's device-agnostic by construction (device args, antenna,
gain, and channel are all just constructor parameters). What differs is
captured in one `DeviceProfile` per device (`config.hpp`):

| | USRP B210 | USRP X310 |
|---|---|---|
| Transport | USB3 | Ethernet (this lab's unit: `addr=192.168.10.2`, factory-default static IP on the 1GigE port) |
| Daughterboard | AD9361 (integrated) | UBX-160 in slot A (channel 0), 10 MHz - 6 GHz, fixed 160MHz analog bandwidth |
| Gain range | ~0-70dB | 0.0-31.5dB (PGA0, per `uhd_usrp_probe`) |
| AGC | Yes | **No** - UHD throws `not_implemented_error`; the UI checkbox is shown disabled instead of silently no-op'ing |
| Max sample rate | 56 Msps (USB3-validated) | 20 Msps (see below) |

**X310 network setup.** This X310 is reached over a direct Ethernet
link, not USB, and needs the host's kernel UDP send buffer raised
first (a standard UHD/X3x0-series requirement, not specific to this
app):
```bash
sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.wmem_max=67108864
```
Confirm connectivity independently of this app with
`uhd_usrp_probe --args="addr=192.168.10.2"` before assuming a problem is
in this code.

**Why the X310 is capped to 20 Msps, not 56.** This app originally
reused the B210's validated 56 Msps figure for both devices
unconditionally - sizing the request to the transport that can carry
it, not the one that's actually connected. At complex sc16 (4
bytes/sample), 56 Msps is ~1.6 Gbps - over 10x what a 1GbE link (~1
Gbps) can physically carry. This was confirmed as a real root cause,
not just a theoretical concern: requesting 56 Msps (silently clamped by
UHD to 50 Msps, still far too high) saturated the link, starved the
RFNoC control channel of bandwidth, and produced sustained "rx xport
timed out getting a response from mgmt_portal" errors specifically in
the higher-rate Wi-Fi bands - severely enough that even the automatic
reconnect's own hardware-teardown path timed out and crashed the
process. 20 Msps leaves headroom under the ~31 Msps physical ceiling of
a 1GbE link. If this X310 is ever moved to a 10GbE (SFP+) link, this
can go back up to 56 Msps (`DeviceProfile::max_sample_rate_hz` in
`config.hpp`).

**X310 connections fail intermittently - by design, this app retries.**
Independently of this app (confirmed via repeated `uhd_usrp_probe` runs
against this same unit), connecting to this X310 over Ethernet fails
roughly 2 times in 5 with `uhd::rfnoc_error "Failure to create
rfnoc_graph"` or an `io_error` management-transaction timeout, then
succeeds cleanly on retry. `Scanner::connect_sdr()` retries up to 6
times (~1.2s apart) before giving up and reporting an error, both for
the initial connect and for the automatic mid-run reconnect described
next.

**RX stall detection and automatic recovery.** If several scan
steps/LoRa listen attempts in a row come back with zero samples, the
status line shows a red **"RX STALLED"** warning (distinct from
"Not connected" - the device handle is still alive, but samples have
stopped flowing; observed in practice on the X310 as the stream going
silent mid-run, its physical activity LED included). The scanner then
automatically tears down and recreates the whole UHD device handle from
scratch - `UsrpCapture::capture()`'s own per-call streamer rebuild
alone wasn't always enough to recover. Tearing down a handle whose
control channel has gone fully dark can itself throw from deep inside
UHD's own hardware-teardown path; that's swallowed rather than allowed
to crash the process, since it's the handle being discarded anyway.

**RFNoC streamer reuse bug (X310-only, fixed).** Changing sample rate
(e.g. switching bands) used to reassign `UsrpCapture`'s streamer
directly, which on the X310 left the *old* streamer's RFNoC graph
connections (Radio -> DDC) still live while a new one was requested at
the new rate - producing "Attempting to reconnect output port" errors
that compounded until internal state corrupted badly enough to crash.
Never an issue on the B210, which doesn't use the RFNoC graph. Fixed by
explicitly releasing the old streamer before requesting a new one.

**Longer per-channel dwell time.** `SUB_CAPTURE_DURATION_S` was doubled
(0.15s -> 0.3s) - longer dwell gives intermittent emitters more chance
to land in a capture window, which matters more on the X310 (each
capture has more RFNoC/network round-trip overhead than the B210's USB
path) and also made real over-the-air activity easier to visually
confirm.

## LoRa protocol decode (`src/lora_phy.hpp`/`.cpp`)

A direct C++ port of the Python prototype's LoRa chirp-spread-spectrum
PHY codec (see the sibling Python project's README for the full
explanation of what this does and doesn't give you - PHY-level decode
only, no LoRaWAN MAC/decryption). While **LoRa (Sub-GHz)** mode is
active, the background `Scanner` also runs a LoRa PHY listen step once
per scan cycle (cycling the 3 mandatory IN865 channels), and the GUI
shows a packet table under the device list with a `Decoded`/`Detected`
status per entry - `Detected` means a real burst's preamble was found
but the payload could not be recovered (see
`newrocktest/TARANGMINI_LORA_FINDINGS.md` in the sibling project for why
that gap currently exists for third-party hardware). Validated
bit-exact against the Python codec on both synthetic tests and a real
captured hardware IQ dump - see `PROJECT_STATUS.md` in this directory
for the full validation log and the one still-open item (the UI panel's
live rendering hasn't been visually confirmed yet).

## Known limitations

Same as the Python prototype, plus:
- **No Wi-Fi protocol decode** (802.11 preamble correlation, MAC/SSID
  extraction) - Wi-Fi is energy detection only. LoRa now has real PHY
  decode/detect, see above.
- Two co-channel emitters are indistinguishable from one.
- `power_db` is relative/uncalibrated, not dBm.
- 5GHz DFS channels aren't scanned (see above).
- 5GHz bandwidth-classification thresholds are a first guess, not yet
  calibrated against a confirmed real AP the way 2.4GHz was.
- LoRa decode against real third-party hardware is incomplete (detection
  works, full payload decode doesn't yet) - see
  `newrocktest/TARANGMINI_LORA_FINDINGS.md`.
- **X310 support is scoped to this lab's specific unit and setup**: the
  IP address is hardcoded (`X310_ADDR` in `config.hpp`, not
  auto-discovered via `uhd_find_devices`), only slot A / channel 0 is
  wired up (the antenna this app expects), and the 20 Msps cap assumes a
  1GbE link specifically - see "Multi-device support" above.
- X310 connections fail intermittently and need several retries as a
  matter of course (see above) - this is worked around, not fixed, and
  a connect can take several extra seconds in the unlucky case.
