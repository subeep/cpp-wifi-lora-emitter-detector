# Handoff: port the symbol-timing detector to C++ and run it on every capture

11 November 2026. Self-contained brief for a coding agent. Branch: work on a new branch off `coordinated-mode`; do not commit to it directly.

## Goal

The app should report, for every Wi-Fi-band capture, whether a drone video link is present and which symbol-timing group it belongs to. The method already exists and is tested in Python (`tools/drone_ml/cp_lag_probe.py`). This task re-implements it in C++ inside the app, running on its own worker thread, and proves the C++ numbers match the Python ones.

Out of scope: model naming, the DroneID decoder, sub-band search, any change to how captures are scheduled or delivered, and any GUI beyond one status line.

## Why this detector

On recorded X310 data it assigned a drone-link group to 0 of 263 captures without a drone link and to 7 of 7 containing the Air 3S link, and on public data it never assigned a wrong group. The existing spectrogram classifier, by contrast, gives a drone name to 48% of no-drone intervals. Details: `docs/DRONE_RF_T07_CP_LAG_PROBE_2026-11-10.md`, `docs/DRONE_RF_TIMING_GATE_DIAGNOSTIC_2026-11-10.md`.

## The algorithm, exactly

An OFDM signal repeats the end of each symbol as a cyclic prefix, so its autocorrelation has a peak at the useful-symbol duration. The detector measures that peak against neighbouring lags.

Per block of IQ (complex float, `N = 131072` samples for capture rates of 20–25 MSps):

1. Subtract the block mean.
2. Multiply by a Hann window of length `N` (NumPy `hanning(N)`, i.e. symmetric).
3. Forward FFT; take the squared magnitude of each bin (`power`, length `N`).
4. **Spike cap:** split `power` into consecutive chunks of 256 bins. For each chunk, compute its median and clamp every bin in the chunk to at most `20 × median`. This stops a strong tone from hiding the peak.
5. Inverse FFT of the capped `power`; take the magnitude; keep lags `0 … max_lag`, where `max_lag = min(N/4, round(rate × 150e-6))`. Divide by the lag-0 value. This is the lag profile.
6. For each hypothesis with useful-symbol time `T`, let `lag = rate × T`:
   - skip the hypothesis unless `8 ≤ lag ≤ max_lag − 60`;
   - `near` = `{floor(lag), ceil(lag)}` when `lag` is not an integer, else `{lag}`;
   - `value` = the largest profile value over `near`;
   - `base` = the **median** of the profile over lags `near.first − 50 … near.last + 50`, excluding `near.first − 3 … near.last + 3`;
   - `ratio = value / base`; the block is **flagged** for that hypothesis when `ratio > 4.0`.

Hypotheses (useful-symbol time in seconds):

| Name | T |
|---|---|
| `wifi_312.5kHz` | 3.2e-6 |
| `wifi_ax_78.125kHz` | 12.8e-6 |
| `lte_like_15kHz` | 1/15000 |
| `lte_like_30kHz` | 1/30000 |
| `lte_like_7.5kHz` | 1/7500 |
| `measured_32.8kHz` | 1/32812.5 |

Per capture: take blocks evenly spaced across the capture (first block at sample 0, last ending at the final sample). For each hypothesis, `flagged_fraction` = flagged blocks / blocks used. Then the group, with threshold `0.3`:

- `measured_32.8kHz ≥ 0.3` → `32.8kHz`
- else `15kHz` and `30kHz` both `≥ 0.3` → `15kHz+30kHz`
- else `30kHz ≥ 0.3` → `30kHz`
- else `15kHz ≥ 0.3` → `15kHz`
- else no drone-link group.

The Wi-Fi hypotheses never produce a drone group; they are reported so the UI can show "Wi-Fi timing".

These constants (4.0, 0.3, 256, 20, the hypothesis list) are frozen for this port. Put them in one header with a version string; do not tune them.

## Where it goes in the app

- **New code:** `src/drones/symbol_timing.hpp` / `.cpp` for the pure DSP (no threads, no I/O), and a small monitor class beside it for the queue and worker.
- **Pattern to copy:** `shortrange::Monitor` (`src/shortrange/monitor.hpp`, `monitor.cpp`). Its `observe()` copies the capture into a bounded queue or drops it, and a worker thread does the processing. Do the same.
- **Wiring:** `src/main.cpp:300-302` builds the scanner's IQ observer lambda, which already calls `drones.observe`, `model_inference.observe` and `shortrange_monitor.observe`. Add one more call there, in both the DJI and non-DJI branches. Do not change `Scanner`.
- **FFT:** the project already links KissFFT (`PkgConfig::KISSFFT`, single precision). Use it; create the plans once and reuse them.

## Threading and bounds (required)

- `observe()` runs on the scanner's processing thread (or the acquisition thread when the Wi-Fi lane is disabled). It must only copy and enqueue: no FFT, no allocation proportional to work done, no blocking. Use `try_lock`; on contention or a full queue, drop and count.
- Do not copy the whole capture. Copy only the blocks that will be analysed: at most `max_blocks` blocks of 131072 samples (default 24, about 25 MB at 8 bytes per sample), evenly spaced.
- Queue: at most 4 captures pending. Count drops by cause (queue full, lock contention, invalid input).
- Reject and count, without processing: empty captures, non-finite samples, a sample rate outside 20–25 MSps, captures shorter than one block.
- The worker owns all FFT state. Stopping must join the worker cleanly; no detached threads.
- Each result carries the capture's key from `CaptureRecord` (run id, capture sequence, radio session), centre frequency, sample rate, device and host timestamps, blocks used, per-hypothesis flagged fraction, the group, processing time, and the constants' version string.
- Keep a bounded history (for example the last 200 results) behind a mutex for the UI to read. A result older than a few seconds must not be shown as current.

## What to show

One line in the existing Drones view or status area is enough: latest group per centre frequency, with age, and the drop counters. Label it experimental. Do not remove or alter the existing model-inference panel in this task.

## Tests (required)

1. **Unit tests for the DSP** in `tests/test_symbol_timing.cpp`, registered in CMake/CTest:
   - synthetic OFDM with a 64-sample symbol and 16-sample prefix at 20 MSps flags only `wifi_312.5kHz`;
   - synthetic 15 kHz and 30 kHz OFDM resampled to 20 MSps (non-integer lags) flag only their own hypothesis;
   - complex Gaussian noise flags nothing;
   - a 30 kHz link plus a tone 40 dB stronger is still flagged, and the tone alone flags nothing.
   The Python versions of these are in `tests/test_drone_ml_cp_lag_probe.py`; mirror them.
2. **Parity with Python on recorded IQ.** `data/drone_ml/2026-11-11/cpp_port_reference/reference.json` holds, for five recorded 20 MSps captures, the Python ratios for eight blocks each (with their start samples), the per-capture flagged fractions and the group. Write an offline tool or test that reads the same `.cf32` files and compares:
   - per-block ratios within 2% relative, or 0.05 absolute, whichever is larger;
   - identical flags except where the Python ratio is within 3% of 4.0;
   - identical group for all five captures (expected: two `30kHz`, three none).
   `data/` is not in git; the test must skip cleanly when the files are absent.
3. **Monitor tests:** queue full drops and counts; invalid input is rejected; start/stop/restart is clean; a result is delivered once per accepted capture.
4. Run the existing `scanner_wifi_lane`, `scanner_coordinated`, `ml_inference`, `unified_drone_addon` and `gui_shell` tests; they must still pass.
5. Thread or lifetime changes should also be run under ThreadSanitizer. On this machine it needs `setarch -R`, and GCC 11's libtsan gives false reports for `condition_variable::wait_for` with a steady clock.

## Measurements to report

- Processing time per capture for 24 blocks, on this machine.
- `observe()` copy time, worst case.
- With the monitor enabled versus disabled in a short replay or live run: any change in Wi-Fi lane waits or drops. Do not claim live timing from synthetic input.

## Done means

All tests above pass; parity holds on the five reference captures; the app shows the group for live or replayed captures; drops are visible; and a short report lists changed files, commands run with results, and anything not verified.

## Files not to touch

Being changed by another session:

- `tools/drone_ml/cp_lag_probe.py`, `tools/drone_ml/uplink_bursts.py`, `tools/drone_ml/x310_session.py`
- `tools/drone_ml/enrolment_experiment.py`, `descriptor_naming.py`, `link_filtered_identity.py`, `same_family_diagnostic.py`
- `tests/test_drone_ml_cp_lag_probe.py`, `tests/test_drone_ml_x310_session.py`
- everything under `data/drone_ml/2026-11-10/` and `data/drone_ml/2026-11-11/` (read `cpp_port_reference/reference.json`; do not write there)
- `docs/DRONE_RF_*_2026-11-10.md`, `docs/DRONE_RF_*_2026-11-11.md`, `docs/DRONE_RF_MODEL_IMPLEMENTATION_PLAN.md`

Leave alone for this task:

- `src/scanner.cpp`, `src/scanner.hpp` (capture delivery is a separate task)
- `src/ml_inference.*` and `tools/drone_ml/live_worker.py` (the existing classifier path)
- `third_party/punjdrone_linux/`, `tools/drone_native_worker.cpp` (the DroneID decoder is a separate task)
- `src/cyclostationary/` (do not extend its Wi-Fi-only hypothesis bank; the new detector is separate code)
- the datasets under `/media/sudeep/Storage/` and the recordings under `data/air3s_o4/` (read-only)
- `~/Documents/air3s_iq_recording/` and `~/Documents/upgraded_ui/`

Allowed to edit: new files under `src/drones/`, `tests/`, `tools/`; `CMakeLists.txt` (add targets only); `src/main.cpp` (the observer lambda and one status line); the Drones panel file for the status line.

## Notes

- The X310 is single-owner. If a live run is needed, close the GUI first; a production scanner sweep appends to `data/wifi_master`, so use `build/coordinated_live_check`, which uses a temporary data root.
- The first open of the X310 after another process releases it often times out; retry.
- Linuxbrew's `ld` shadows the system linker; if a manual link produces an empty binary, use `/usr/bin/c++ -B/usr/bin`.
