# B210 Emitter Monitor - C++ / Dear ImGui port

A C++ port of a Python prototype (a separate project) that reads IQ
samples from a USRP B210 and reports currently-active LoRa/Wi-Fi
emitters via energy detection. This version adds a live desktop UI
(Dear ImGui) with a switch between three band modes - **LoRa (Sub-GHz)**,
**Wi-Fi 2.4GHz**, and **Wi-Fi 5GHz** - each showing its own persistent
device list.

**This is energy detection, not protocol decoding.** It tells you
*something is transmitting at this frequency with this bandwidth*, not
*this is device X sending payload Y*.

## Requirements

- USRP B210 connected via USB3 (USB2 will work but at reduced safe sample rates)
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
  config.hpp        - band/scan-plan constants, all three modes
  spectrum.hpp/.cpp  - max-hold spectrogram (KissFFT) + DC/edge guard masks
  detector.hpp/.cpp  - threshold segmentation, DC-gap bridging
  classifier.hpp/.cpp- bandwidth-based protocol guess, per band
  registry.hpp/.cpp  - per-band device tracking with expiry (thread-safe)
  sdr_capture.hpp/.cpp - UHD C++ API wrapper for finite IQ capture
  scanner.hpp/.cpp   - background thread: owns the SDR + 3 registries,
                       scans whichever band is "active," thread-safe
                       getters/setters for the UI
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

| Mode | Coverage | Steps | Sample rate |
|---|---|---|---|
| LoRa (Sub-GHz) | 863-868 MHz (IN865 + margin) | 1 | 5 Msps |
| Wi-Fi 2.4GHz | 2401-2484 MHz (channels 1-13) | 3 | 56 Msps |
| Wi-Fi 5GHz | UNII-1 (ch 36-48) + UNII-3 (ch 149-165) only | 5 | 56 Msps |

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

## Known limitations

Same as the Python prototype, plus one new item:
- No protocol decode (LoRa dechirping, 802.11 preamble correlation) -
  energy detection only.
- Two co-channel emitters are indistinguishable from one.
- `power_db` is relative/uncalibrated, not dBm.
- 5GHz DFS channels aren't scanned (see above).
- 5GHz bandwidth-classification thresholds are a first guess, not yet
  calibrated against a confirmed real AP the way 2.4GHz was.
