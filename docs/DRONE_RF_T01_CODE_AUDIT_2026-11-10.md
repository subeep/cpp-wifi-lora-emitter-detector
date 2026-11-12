# T01 — code, mode, OFDM and decoder contract audit

10 November 2026. Read-only audit of branch `coordinated-mode`. Nothing was built, run or changed. Line numbers are as of this date. The audit was done by code reading; the five items marked ✔ were re-checked directly against the source.

## 1. Where captured IQ is delivered

- The IQ observer is a `std::function` taking a const reference to the capture vector and its `CaptureRecord` (`src/scanner.hpp:151`). The IQ is valid only during the callback; there is no shared ownership.
- It fires in exactly two places:
  - ✔ `src/scanner.cpp:1678` — Wi-Fi steps, after burst detection, decoding and fingerprinting.
  - ✔ `src/scanner.cpp:1786` — coordinated drone-only visits (`observer_only`), with **no decoding at all**.
- It never fires for sub-GHz or LoRa steps.
- Thread: normally the serial-lane worker. With the lane disabled (`RFMON_WIFI_LANE=0` or the IQ budget exceeded) it runs on the acquisition thread (`src/scanner.cpp:2497`).
- Queues: band mode allows one outstanding step and blocks. ✔ Coordinated mode uses a non-blocking `try_push` with two outstanding visits; a third is dropped and counted in `lane_drops` (`src/scanner.cpp:2157`, `2167`). Dropped visits and empty drone-only captures reach no observer.
- `Scanner` has no file or replay source. Offline paths (`drone::Addon::load`, `ml_inference_probe`, `wifi_cyclo_replay`) call the consumers directly.

## 2. Consumers wired in `src/main.cpp`

Called synchronously in this order (`src/main.cpp:300-302`): `drones.observe`, `model_inference.observe`, `shortrange_monitor.observe`. The frame observer feeds the Remote ID monitor.

| Consumer | How it keeps IQ | Bound |
|---|---|---|
| Drone add-on, native path | Chunks of at most 160 ms, copied | ✔ Queue of 1; later chunks of a long capture are likely dropped (`src/drones/detector.cpp:55`) |
| Drone add-on, legacy path | Up to 4 windows of 20 ms | Queue of 4 |
| `ml::Inference` | Copies selected intervals into one pending slot | 1 job |
| Short-range monitor | Full capture copy | 4 captures / 768 MiB |
| Remote ID monitor | Copies the frame event | 128 events |

## 3. `ml::Inference` admission and protocol

- Rejects a capture that is not contiguous, has a rate outside 20–500 MSps or a non-integer rate, or is shorter than 8 ms. A rejection also bumps the generation, which invalidates current results.
- One pending job, and at least 500 ms between submissions, so at most two jobs per second.
- Intervals: up to three 8 ms intervals spread evenly over the capture. At 20–25 MSps that is about 0.5 M samples, far below the 4 M budget.
- Worker: `socketpair`, length-prefixed JSON followed by raw cf32, one request per interval, 15 s deadline. Schemas `rfmon.ml.iq.v1`, `rfmon.ml.ready.v1`, `rfmon.ml.result.v1`.
- `accepted` is always cleared; results expire after 2 s; history is capped at 100.

## 4. Capture and frame metadata

- Available at acquisition: run, capture sequence, radio session, band, channel, centre and actual RF/DSP frequency, requested and actual rate, gain, antenna, device, samples requested/received, clock, device and host times, overflow marks, timeout, retune.
- Available only after decoding: burst counts by type, DSSS/OFDM counters, events submitted/rejected, `processed`, `processing_s`.
- Every `FrameEvent` field is post-decode. Its key is (run, capture sequence, sample start, PHY).

## 5. Drone decoders that already exist

- Legacy Python DroneID backend (`src/drones/python/backend.py`): OFDM, descramble, turbo, CRC. Reports a `model` from the payload `device_type`.
- Native C++ backend (`drone_native_worker`, linking `third_party/punjdrone_linux`): ✔ **a C++ Zadoff-Chu / DroneID demodulator already exists** (`third_party/punjdrone_linux/src/dji/params.cpp:21`, roots 600 and 147, 15.36 MSps, 15 kHz). It reports `reported_model`, `reported_serial`, correlation, CFO and O4 evidence.
- Remote ID (`src/drones/remote_id.cpp`): ASTM F3411 from Wi-Fi frames. No product-model field; `authenticated` is always false.
- Ownership in `src/drones/addon.hpp` covers the classifier, live service and Remote ID. The native worker is started and stopped unconditionally, outside that tracking.

## 6. Cyclostationary code

- ✔ The OFDM hypothesis bank is Wi-Fi only (`src/cyclostationary/ofdm_structure.cpp:46-60`): 3.2 µs and 12.8 µs useful symbols. **There is no 15 kHz (66.7 µs) hypothesis**, and blind structure discovery caps the lag too low to find one.
- `wifi_cyclo_replay analyze` accepts `--ofdm-useful N --ofdm-prefix N`, which replaces the bank, so LTE-like timing can be tested on a cf32 file without code changes. It is built only with `RFMON_BUILD_CYCLO_OFFLINE` or the shadow option.
- `link_evidence` and `named_link_candidates` are uncalibrated threshold checks; the latter covers OcuSync chirp markers only and never asserts an identity.

## 7. Tests and builds

- Relevant CTest names: `serial_lane`, `scanner_wifi_lane`, `scanner_coordinated`, `capture_provenance`, `coordinated_schedule`, `coordinated_panel`, `ml_inference`, `unified_drone_addon`, `drone_native_worker`, `remote_id_monitor`, `dji_decoder`, `dji_addon`, `gui_shell`, and the `cyclostationary_*` set.
- `build/` is a Release build with the DJI and cyclo-shadow options on and the offline cyclo option off. `ml_inference_probe`, `drone_native_worker` and `wifi_cyclo_replay` are present.

## 8. Where plan section 2 is incomplete

| Plan statement | What the code shows | Consequence |
|---|---|---|
| IQ observer is at the processing tail | True for Wi-Fi steps only; drone-only visits already deliver undecoded IQ | C06 is smaller than written for coordinated mode |
| Audit replay entry points | `Scanner` has none | C06 replay coverage must be built per consumer |
| Exactly-once delivery (C06) | Lane drops and empty captures skip observers today | Needs explicit handling, not only preservation |
| Reuse existing OFDM code for the 15 kHz structural detector | Bank is Wi-Fi only | T07 must add the hypothesis, or use the CLI override first |
| No mention of a C++ ZC demodulator | One exists in `third_party/punjdrone_linux` | C03 should start from it; note its GPL-3.0-or-later licence |
| Ownership tracked in `addon.hpp` | Native worker is outside the tracking | C10 lifecycle work must cover it |

Not audited here: the Python tools under `tools/drone_ml` other than the worker protocol.
