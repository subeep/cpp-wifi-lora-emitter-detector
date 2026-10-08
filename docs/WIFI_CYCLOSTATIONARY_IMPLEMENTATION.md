# Cyclostationary analysis: DSP foundation and isolated Wi-Fi sidecar

Updated: 5 October 2026. Current scope is cyclostationary/DSP analysis only. The user explicitly deferred model building. No model has been built or trained, and no radio transmission was performed.

## Delivered functionality

The standalone `wifi_cyclo_replay` executable provides three commands:

- `inventory`: enumerate headerless IQ files, report actual sizes/durations under declared parameters, inspect bounded finite-IQ prefixes, retain directory-derived labels and source-recording groups, and flag nominal-length differences and empty directories.
- `analyze`: read a bounded window at a complex-sample offset, optionally translate/filter/decimate a selected band, and export ordinary spectral features, cyclic peaks and held-out OFDM prefix measurements as JSON. Optional `--measure-rois` and `--measure-chirps` add bounded ROI and chirp/instantaneous-frequency diagnostics. `--assess-evidence` adds the transparent experimental DSP preview policy and its chirp diagnostics; no drone-family acceptance is enabled. Explicit `--refine-chirps` adds the offline-only diagnostic documented below.
- `shadow-replay`: read up to four windows without loading the full capture, optionally guided by caller-declared raw burst ranges, then analyze them independently through the same isolated child and validated IPC protocol used by the Wi-Fi sidecar. Native sample rate only; caller-declared capture extent/continuity/hints remain unverified for headerless files.

Implementation files:

| Component | Source |
|---|---|
| Read-only little-endian IQ reader | [iq_file.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/iq_file.hpp) and its `.cpp` |
| Private band selection / anti-alias filtering | [analysis_samples.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/analysis_samples.hpp) and its `.cpp` |
| Spectral-correlation estimator | [spectral_correlation.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/spectral_correlation.hpp) and its `.cpp` |
| OFDM prefix and fine symbol-alpha measurements | [ofdm_structure.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/ofdm_structure.hpp) and its `.cpp` |
| Chirp lag correlation and selected-span frequency slope | [chirp_structure.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/chirp_structure.hpp) and its `.cpp` |
| Deterministic experimental waveform/quality/passband checks | [link_evidence.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/link_evidence.hpp) and its `.cpp` |
| Worker-local energy intervals and ordinary ROI spectra | [roi_measurements.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/roi_measurements.hpp) and its `.cpp` |
| Optional bounded shadow worker | [shadow_worker.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/shadow_worker.hpp) and its `.cpp` |
| Bounded raw burst hint collection and tile selection | [burst_selection.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/burst_selection.hpp) |
| Process supervisor and local IPC | [process_client.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/process_client.hpp) and its `.cpp` |
| Bounded wire protocol and result validation | [worker_protocol.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/worker_protocol.hpp) and its `.cpp` |
| Deterministic distributed sampling | [sample_tiles.hpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/sample_tiles.hpp) |
| Separate DSP child executable | [wifi_cyclo_worker.cpp](/home/sudeep/Documents/newrocktest-cpp/tools/wifi_cyclo_worker.cpp) |
| Optional Wi-Fi measurement panel | [panel.cpp](/home/sudeep/Documents/newrocktest-cpp/src/cyclostationary/panel.cpp) |
| Offline CLI and JSON output | [wifi_cyclo_replay.cpp](/home/sudeep/Documents/newrocktest-cpp/tools/wifi_cyclo_replay.cpp) |
| Independent math, file and CLI checks | [test_cyclostationary.cpp](/home/sudeep/Documents/newrocktest-cpp/tests/test_cyclostationary.cpp), [test_cyclo_cli.py](/home/sudeep/Documents/newrocktest-cpp/tests/test_cyclo_cli.py) |
| Independent ROI bounds, frequency, mixture and resource checks | [test_cyclo_roi.cpp](/home/sudeep/Documents/newrocktest-cpp/tests/test_cyclo_roi.cpp) |

The reader accepts explicitly declared `cf32_le` or `ci16_le` interleaved I/Q. It rejects non-regular inputs, empty/misaligned files, out-of-range offsets and non-finite selected samples. A read is limited to 1,048,576 complex samples; the default is 262,144. It never loads a multi-gigabyte recording wholesale. Source parameters are caller-declared, not inferred from bytes; sample rate, RF center, physical units, sessions and continuity must not be silently invented.

Band selection performs frequency translation on a private copy and a symmetric Hamming-windowed sinc low-pass before integer decimation. It requires an explicit source usable bandwidth, ROI offset, requested passband and decimation factor. It enforces transition space and resource bounds, drops filter transients, and records the first output sample's original coordinate, input step, output rate and analysis center. Output samples are never produced by merely changing a sample-rate label. The requested passband has a transition region; it is not a brick-wall filter or an automatic emitter separator.

The estimator averages windowed FFT cross-products with frame-start phase correction. Its convention is `X(f+alpha/2) * conj(X(f-alpha/2))`. It subtracts the selected samples' mean, normalizes their variance and uses periodic Hann windows. Results include frequency/alpha grid spacing, actual analyzed duration, peak coordinates, squared normalized coherence, spectral flatness, 99% occupied-band edges, DC fraction and amplitude diagnostics. Invalid/insufficient features remain unavailable; alpha zero is ordinary spectral power and is excluded from cyclic peaks.

This first estimator supports an integer-bin, positive-alpha grid. FFT length is even, 32–4096 with factors 2/3/5; at most 1024 frames and 128 alpha separations are allowed. A coprime default hop avoids aliases between the requested alpha bins. Both FIR and cyclic loops enforce separate 64-million-operation ceilings. These are offline safeguards, not acceptance of the future live-worker budget.

The OFDM measurement bank folds `x[n+useful] * conj(x[n])` at the proposed symbol period. The first portion fits prefix alignment by maximizing inside-prefix versus outside-prefix squared coherence. A separate later portion measures that fixed alignment, with one complete symbol of pair-start coordinates omitted between portions so their sample endpoints do not overlap. Both portions need at least eight symbols. The selected-window mean is shared preprocessing; these are separated portions of one recording, not independent captures or a sealed validation set. The reported sample span includes the omitted separating symbol.

The bank also evaluates lagged cyclic correlation at the exact proposed symbol rate `Fs/(useful+prefix)`, including rates between the coarse FFT alpha bins. This adds a targeted fine-alpha measurement, not an arbitrary-alpha search or conjugate estimator. Default useful/prefix timings are 3.2/0.8 µs, 3.2/0.4 µs and 12.8/(0.8, 1.6, 3.2) µs, following the [NI WLAN timing table](https://www.ni.com/en/solutions/semiconductor/wireless-connectivity-test/introduction-to-802-11ax-high-efficiency-wireless.html). A timing is skipped if either duration is not an exact integer number of source samples; no rounding or hidden resampling occurs. These shared timing patterns cannot identify a particular WLAN generation, vendor or aircraft.

Custom `--ofdm-useful N --ofdm-prefix N` replaces the default bank with one caller-supplied sample-domain hypothesis. At most 32 hypotheses and 262,144 selected samples are measured, useful lengths are 2–8192 samples, and the prefix must be shorter than the useful length. Unavailable measurements are null in JSON. Selecting the best candidate does not correct for multiple hypotheses; no threshold, drone probability, waveform verdict or named-family acceptance is supplied.

## Worker-local ROI measurements

Every admitted tile now receives experimental time/frequency measurements inside the DSP child. A 128-sample block envelope covers the entire copied tile after whole-tile complex-mean removal. The 20th percentile of block mean energy is a relative background proxy, with a numerical floor at `1e-12` of mean energy. Connected blocks exceeding twice that proxy are retained when at least one exceeds four times it. The strongest eight intervals by integrated mean-removed energy are returned in sample order; total contrast samples, all eligible interval counts and omissions are disclosed. These thresholds are selection heuristics, with no calibrated noise, significance, waveform or drone interpretation.

Bounds have 128-sample resolution (6.4 µs at 20 Msps); partial final blocks are retained. Window-edge intervals are flagged as potentially cropped. A short interval can have insufficient spectrum. When no contrast interval exists, the full tile remains as context for continuous emissions; this state does not establish signal absence. DC-only/constant IQ returns no variation. Global amplitude normalization and per-region RMS normalization keep the measurements finite over tested `1e-25`/`1e25` scaling; they do not calibrate RF power.

Each retained interval has a local-mean-removed ordinary PSD using periodic Hann FFTs of 512 samples. Up to 32 non-overlapping frames are distributed through its range; their count, examined samples and sampled fraction are explicit. The reported 99% power span and up to three strongest connected spectral-energy intervals use a 20th-percentile bin-power proxy with low/high multipliers 4/8. Frequency intervals and omitted counts do not count emitters. Frequency resolution is `Fs/512`; DC-bin membership and Nyquist-edge flags qualify the ranges. Usable analog passband calibration remains outstanding. Neither time nor frequency intervals establish source separation; fading, mixtures, FFT leakage and detector heuristics can fragment one transmission or merge several.

ROI coordinates stay relative to their original tile, with original source sample centers and sample step in CLI output. Filtered `analyze` output maps centers through the FIR delay and integer decimation. SCF and held-out CP continue using the original admitted tiles; the ROI heuristics do not replace their input or add an acceptance gate. Across 13 replay regions / 43 windows, all previous per-tile DSP fields match exactly. No producer-side ROI computation, extra sample copy budget, radio setting or model is added.

The ROI-stage IPC version was **2**. The current version is **4**, including chirp frequency mean used in declared-passband geometry. Parent validation checks counts, thresholds, sample ranges, non-overlap, power sums, PSD frame coverage, frequency bounds and edge/DC flags. Old/incompatible helper versions are rejected before publication. Replies remain capped at 64 KiB; a four-tile fixture with full spectra and maximum retained ROI/band metadata passes round-trip validation. Child memory/CPU limits, queue limits and supervision deadlines remain unchanged. Rebuild the optional application and helper together.

## Isolation from the existing system

New implementation sources live under `src/cyclostationary/`, outside the existing top-level `src/*.cpp` application glob. `RFMON_BUILD_CYCLO_OFFLINE` defaults **OFF**; enabling it alone builds separate offline targets without linking them into the GUI. The additional `RFMON_ENABLE_WIFI_CYCLO_SHADOW` also defaults **OFF**. It explicitly compiles the adapter and panel and links the analysis library into the GUI and scanner test targets. Even in that build, the runtime checkbox starts disabled, with no worker thread or IQ pool allocated.

With the build feature disabled, preprocessing `main.cpp` and `scanner.cpp` produces the same token sequence as the Git HEAD baseline under the default build flags; scanner signature/call formatting introduces whitespace-only differences. The GUI's generated compile/link inputs contain no cyclostationary sources/library. Existing security and identity implementations are unchanged; no storage migration or radio control was added. The installed/normal build was not replaced or launched during validation.

When explicitly enabled, the parent supervisor owns eight preallocated 2 MiB IQ slots. Acquisition stores only a fixed array of four small capture descriptors in the existing inline/lane job. After existing per-capture security/identity work, the adapter reuses that capture's raw burst ranges, irrespective of packet acceptance, and submits bounded copies while the source buffer is alive. Submission uses `try_lock`, drops on contention/full capacity, copies at most 262,144 complex samples, and never waits for queue space or allocates on the producer path. Metadata includes the individual capture's actual sample rate, requested RF center, session/sequence, pre-acquisition epoch and device-time anchor. Tune-center semantics remain requested center; no calibrated RF-center inference is made.

Only the continuous region before the first known overflow is eligible. Unknown overflow position, invalid/out-of-order gap metadata, exceptions or fewer than 2048 continuous samples cause an add-on drop. Resumed data is not joined across gaps. Without useful hints, the existing distributed plan selects up to four independent windows of at most 65,536 samples. For longer prefixes, guidance retains first/last context windows and replaces at most two interior windows with non-overlapping burst-centered windows; unused positions can retain distributed context. Each guided window targets 4096 samples of total padding, then clamps its length to 8192–65,536 samples; long bursts are cropped around their center. Ranges, copied counts and selection origins are explicit and bounded even at maximum `size_t` values. Small prefixes retain their prior distributed selection.

At most 512 evenly indexed raw descriptors are examined from the existing detector's output, including unknown/narrowband bursts. The two guided searches start at eligible descriptor positions near one-third and two-thirds and wrap once to find disjoint windows. This deterministic sampling is biased: existing detector thresholds, minimum burst length, its 4000-range cap, the 512-descriptor sample and window overlap can all omit transmissions. Out-of-prefix or gap-crossing hints are rejected rather than clipped across gaps. Reported/examined/eligible/rejected counts and detector-cap state are shown, without claiming complete activity coverage. No additional full-capture energy scan, new Wi-Fi acceptance gate, source separation or phase concatenation occurs. Short high-rate bursts may still provide insufficient symbols. At 20 Msps the maximum total copied duration remains 13.1 ms; at 56 Msps it remains 4.68 ms. Timing beyond copied windows is not supplied to the DSP child as an activity timeline.

`wifi_cyclo_worker` owns DSP computation in a separate process. The parent background thread only supervises the queue, packs/transfers IQ and validates returned measurements. Each window uses at most 256 FFT frames and the same core estimators as replay; its spectral span can be shorter than its window and the CP bank has its own per-hypothesis span. A failed measurement never publishes a partial result. Only one immutable latest capture result is held internally, containing up to four separate window results; the GUI reads it without copying IQ. An external reader holding an older snapshot retains that result.

The supervisor launches the helper using `posix_spawn` and an inherited local socket, resolving it beside the running executable or in its build's `tools/cyclostationary/` directory. It never searches PATH. Child environment is minimal, standard streams go to `/dev/null`, and all unrelated descriptors are closed: no inherited SDR, UI, recording or network handles. IPC uses read/write on the already-created socket, with SIGPIPE blocked only for each write and the previous thread mask restored; application signal handlers remain unchanged. No network service, Python production dependency or temporary IQ file is needed.

The versioned little-endian protocol explicitly serializes IEEE float32 IQ and float64 measurements. Requests carry at most four windows and 2 MiB IQ; replies are limited to 64 KiB **before payload allocation**. Parent validation checks magic/version/types, request IDs, original offsets/counts, FFT/frame/span/rate consistency, array/string bounds, finite values, coherence/contrast ranges, normalized spectrum sum and declared OFDM timing hypotheses. Truncated, extra, incompatible or malformed data is rejected. This contains child DSP failures; it is not a security sandbox for executing arbitrary untrusted programs.

The child enforces a **128 MiB address-space cap**, a **30 s cumulative CPU lifetime cap**, 32 descriptors, no core dumps and `no_new_privs`, and runs at nice 10 or lower priority. Address space is not a combined parent/child RSS measurement. The supervisor recycles the child after eight completed requests, before normal work reaches the lifetime CPU cap. It uses 2 s startup/measurement I/O deadlines, checking cancellation at most every 20 ms during polling. Failure permits at most three automatic restart attempts per enable session; exhausting them disables analysis and clears results. Ordinary input/measurement errors and selection cancellation do not spend the transport-fault allowance. Startup failure is visible through failure counters; fresh work is required to attempt a permitted restart.

Band/fixed-channel changes invalidate queued/results; an epoch obtained before acquisition rejects stale captures. Cancellation terminates the partially-used child connection. Shutdown stops admission, discards queued work, cancels current IPC, kills/reaps only the owned child, joins the supervisor and releases the pool. Reaping uses a 250 ms polling bound; tests cover ordinary crashes and hangs. OS/kernel stalls in process creation, filesystem lookup or reaping are outside this software timing guarantee. The child sets a Linux parent-death SIGKILL and validates the original parent PID after setting it, closing the startup race if the parent has already died. Normal input service also exits when its parent socket closes.

The collapsed optional Wi-Fi panel shows measurement age, sampled coverage, queue/drop/failure and process supervision counters, a window selector, original sample ranges, CP candidate measurements and a cyclic peak. It presents drone activity and link family as **unknown**. It never changes packet classifications, identities or security decisions, and creates no analysis persistence by itself. Runtime replay artifacts remain under the separate `data/wifi_drone_analysis/` directory.

**Process isolation and bounded supervision are implemented and tested, but accepted live operation remains gated.** Parent allocation/copy/IPC/result parsing still share host resources; process isolation cannot eliminate CPU or memory-bandwidth contention. Receiver off/on/off comparisons, deployment-host copy latency and memory accounting, real supported-link activity evidence and soak gates remain outstanding. Software equivalence and synthetic fault containment do not establish receiver performance acceptance. The default and normal application build remain unchanged.

## Build and use

Run from the repository root:

```bash
cmake -S tools/cyclostationary -B build/cyclostationary-offline -DCMAKE_BUILD_TYPE=Release
cmake --build build/cyclostationary-offline -j 2
ctest --test-dir build/cyclostationary-offline --output-on-failure
build/cyclostationary-offline/wifi_cyclo_replay --help
```

Build the separate experimental GUI for development, retaining the normal build:

```bash
cmake -S . -B build/cyclostationary-integration-check \
  -DRFMON_BUILD_CYCLO_OFFLINE=ON -DRFMON_ENABLE_WIFI_CYCLO_SHADOW=ON
cmake --build build/cyclostationary-integration-check \
  --target rf_monitor_gui test_scanner_wifi_lane test_cyclo_panel -j 2
ctest --test-dir build/cyclostationary-integration-check \
  -R '^(cyclostationary_.*|scanner_wifi_lane)$' --output-on-failure
```

The GUI target builds `wifi_cyclo_worker` automatically. Keep the helper beside the application, or retain the generated build layout. Missing/incompatible helpers disable analysis rather than starting DSP in the GUI process. To run the full process fault suite, build all optional targets, including `test_cyclo_process` and `cyclo_fault_worker_1` through `_4`, before CTest.

The experimental binary is `build/cyclostationary-integration-check/rf_monitor_gui`. In a Wi-Fi view, expand **Drone / link analysis (experimental cyclostationary DSP)** and use **Enable optional DSP analysis**. This is a manual development opt-in; no receiver was opened during these checks. Disable/re-enable clears stale results. With the build switch OFF, this panel and producer hook do not exist.

The expanded panel additionally shows energy/context intervals, retained counts, energy shares, PSD coverage, 99% power spans and qualified spectral interval offsets. Its existing scroll area leaves the ordinary Wi-Fi tables available.

Inventory the existing local corpus. This prints JSON; it preserves original files:

```bash
build/cyclostationary-offline/wifi_cyclo_replay inventory \
  --root /home/sudeep/Pictures/DroneDetect_V2 \
  --format cf32_le --sample-rate 60000000 --center 2437500000 \
  --expected-samples 120000000
```

Native-rate cyclic measurements from a small window:

```bash
build/cyclostationary-offline/wifi_cyclo_replay analyze \
  --input /home/sudeep/Pictures/DroneDetect_V2/CLEAN/AIR_FY/AIR_0010_00.dat \
  --format cf32_le --sample-rate 60000000 --center 2437500000 \
  --samples 262144 --offset 0 --fft 480 --alpha-bins 32 --include-spectrum
```

Select a 20 MHz passband around 2.437 GHz from the same recording, with an output rate of 30 Msps. The declared 28 MHz source passband comes from the publisher description; hardware calibration remains unverified:

```bash
build/cyclostationary-offline/wifi_cyclo_replay analyze \
  --input /home/sudeep/Pictures/DroneDetect_V2/CLEAN/AIR_FY/AIR_0010_00.dat \
  --format cf32_le --sample-rate 60000000 --center 2437500000 \
  --samples 262144 --fft 480 --alpha-bins 32 \
  --roi-offset -500000 --roi-bandwidth 20000000 \
  --source-bandwidth 28000000 --decimate 2 --include-spectrum
```

`--offset` is always measured in original-file complex samples. The output retains the original rate under `input` and the effective rate/center under `preprocessing`. Peak offsets are relative to the analysis center. Report schemas are `rfmon.cyclo.inventory.v1` and `rfmon.cyclo.features.v1`; estimator and preprocessing versions are explicit.

Replay the native-rate distributed budget over a declared one-second DroneDetect region without loading that second wholesale:

```bash
build/cyclostationary-offline/wifi_cyclo_replay shadow-replay \
  --input /home/sudeep/Pictures/DroneDetect_V2/CLEAN/AIR_FY/AIR_0010_00.dat \
  --format cf32_le --sample-rate 60000000 --center 2437500000 \
  --capture-samples 60000000 --include-spectrum
```

The `rfmon.cyclo.shadow_replay.v1` report includes per-window offsets/counts, total sampled fraction, process profile and independent measurements. `--offset` selects the original capture start; `--capture-samples` is clipped to actual EOF. `--continuous-samples` can conservatively restrict the eligible pre-gap region but must not exceed the available capture. No timing/continuity is inferred from raw bytes. This reproduces selection, protocol and fixed analysis limits; it does not emulate live queue contention, capture scheduling, receiver gaps or ROI filtering. Public 60/120/200 Msps data is still not evidence of supported operation on the unchanged 20 Msps receiver.

Optional `--burst-hints FILE.json` accepts a regular JSON file of at most 64 KiB containing `{"ranges":[{"start":450000,"length":12000}],"detector_capped":false}`. It allows at most 512 nonnegative integer ranges, relative to the selected capture offset, not absolute file indices. Hints are caller-declared and unverified; empty/invalid/gap-crossing ranges cannot introduce IQ outside the eligible prefix. Without this option, replay remains distributed. The live adapter collects hints from the existing detector; replay does not run a second detector.

Reproduce the controlled received-Wi-Fi selection check, preserving originals and deleting temporary derived IQ:

```bash
python3 tools/cyclostationary/validate_burst_selection.py \
  --tool build/cyclostationary-offline/wifi_cyclo_replay \
  --output data/wifi_drone_analysis/burst_selection_2026-10-03
```

## Validation and actual-data results

`shadow-replay` always includes `roi_measurements` per tile. For direct or filtered `analyze`, opt in with `--measure-rois`; at most 65,536 **analysis** samples are allowed, with no silent truncation. For example:

```bash
build/cyclostationary-offline/wifi_cyclo_replay analyze \
  --input tests/fixtures/wifi_ofdm/avgarde-5g-offset.cf32 \
  --format cf32_le --sample-rate 20000000 --center 5181500000 \
  --samples 12260 --measure-rois
```

Replay the same native-rate data as the process-stage baseline and check unchanged per-tile DSP fields:

```bash
python3 tools/cyclostationary/validate_roi_replay.py \
  --tool build/cyclostationary-offline/wifi_cyclo_replay \
  --baseline data/wifi_drone_analysis/process_sampling_2026-10-03 \
  --output data/wifi_drone_analysis/roi_measurements_2026-10-03
```

New tests pass in both the standalone and opt-in root builds. They cover:

- Known two-tone cyclic frequency and spectral midpoint; an independent absolute-time direct DFT checks the FFT estimator's conjugation and frame-phase correction.
- Stationary complex noise, periodically modulated noise, single-tone leakage, DC-only input, amplitude scaling and non-finite values.
- File byte order, complex-sample offsets, truncated/misaligned files, bounded reads from a sparse 8 GB file, explicit missing values and provenance.
- FIR passband frequency/amplitude/phase and input-coordinate mapping; the tested out-of-band tone is attenuated by more than 46 dB before decimation. This is a specific test case, not a universal attenuation guarantee for every filter configuration.
- Source-passband boundaries, transition guards, short filter inputs, alpha/hop aliases and memory/operation limits.
- Independently synthesized QPSK OFDM recovers prefix phase and held-out evidence under noise/carrier offset; wrong timing, stationary noise, single tones, constant input and fractional timing are checked. Strong tone lag correlation is distinguished from localized CP contrast and symbol-alpha structure.
- Shadow-worker bounded copying, unmodified source IQ, overload drops, stale epochs, known/unknown/invalid overflow positions, lifecycle races, restart and contained non-finite-input failures.
- ImGui panel rendering for disabled, waiting, measured, insufficient-symbol, overflow, other-band and stale-epoch states.
- Native child crash/hang containment, bounded stop of a five-second hung request, restart exhaustion, recovery on the next healthy capture, routine recycling, original descriptor exclusion and actual kernel resource limits.
- Guided selection over 1000 randomized range sets, maximum `size_t` indexing, 512-descriptor limits, gap rejection, non-overlap and unchanged IQ budgets. Two synthetic short CP transmissions between the unguided windows are recovered independently using raw hints; source IQ remains unchanged.
- Independently injected energy bounds, unaligned burst edges, partial final blocks, frequency signs, tone-energy shares, continuous two-tone mixtures, DC/scaling, sub-FFT abstention and time/frequency output caps. CLI checks filtered original-sample mapping and requested RF coordinates. Protocol checks malformed ROI ranges/coverage/thresholds/flags and maximum retained metadata inside the unchanged reply cap.
- Portable request/result round trips, 500 fixed-seed arbitrary result payloads, wrong IDs/coordinates, non-finite/out-of-range values, invalid versions and over-limit/truncated/trailing messages.
- A synthetic signal present only at the end of a long capture is recovered by its own final tile; the first tile stays noise-like. Distributed ranges are tested through maximum `size_t`, and a sparse 8 GB file is sampled without whole-file allocation.

The opt-in and default-off GUI builds succeed. The radio-free scanner test passes both default-off and with the sidecar runtime enabled: captured schedule/cycles, packet rows, registries, identity counts and the ordered security recording match the sidecar-disabled run exactly. The existing GUI render/identity/security interaction test also passes. Earlier selected regressions passed `test_wifi_phy`, `test_wifi_frame` and `test_wifi_ofdm tests/fixtures/wifi_ofdm` (26 OFDM checks). No receiver performance or hardware-in-the-loop test was run.

AddressSanitizer/UndefinedBehaviorSanitizer checks cover the DSP math, parent queue/lifecycle and parent protocol/supervision tests, using the Release helper and fault fixtures for process tests. An instrumented child needs a large virtual shadow address space incompatible with the enforced 128 MiB address-space cap, so its limit is never weakened for these tests. LeakSanitizer itself fails under this environment's ptrace supervision; parent runs use `ASAN_OPTIONS=detect_leaks=0`, and leak checking is not claimed. Earlier-stage [software validation record](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/ofdm_structure_2026-10-03/software_validation.json).

| Actual-data exercise | Result and artifact |
|---|---|
| DroneDetect directory audit | 390 files; 374.2 GB; 390 finite 4096-sample prefix probes; 150 exact nominal lengths, 238 longer and 2 shorter; two empty condition folders. [Inventory](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/offline_2026-10-02/dronedetect_inventory.json) |
| First DroneDetect measurements | Eight windows from four `AIR_FY` recordings, across clean/BT/Wi-Fi/both conditions; two positions per recording. About 0.11–0.14 s per process and 7.5–7.6 MiB peak RSS in these runs. Each measures about 4.12 ms of continuous samples. [Summary](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/offline_2026-10-02/first_measurements_summary.json) |
| Completed Zenodo files | Six files totalling 2.64 GB; all full-file MD5s and sizes match the [publisher record](https://zenodo.org/records/4264467). Parameters and conservative same-model grouping retained. [Verified manifest](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/offline_2026-10-03/zenodo_verified_local_manifest.json) |
| Additional measurements | Twelve native-rate windows from those six files at 120/200 Msps, plus three short received ordinary Wi-Fi beacon fixtures at 20 Msps. Native drone windows took about 0.11–0.12 s and under 8 MiB peak RSS here. [Summary](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/offline_2026-10-03/cross_source_measurements_summary.json) |
| Filtered band measurements | Three DroneDetect windows, downmixed by −0.5 MHz and decimated 60→30 Msps with a 57-tap filter; about 0.06–0.07 s and 10.8–11.0 MiB peak RSS in these runs. Original sample centers and discarded transients recorded. [Summary](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/offline_2026-10-03/band_selection_measurements_summary.json) |

This is **26 bounded real-data measurements**, not a detection-accuracy experiment. Ordinary Wi-Fi has nonzero cyclic features too. A short ordinary beacon crop here reached about 0.68 squared coherence, while several drone-labelled wideband windows had much lower peaks. Record lengths, averaging and receiver conditions differ, so these scores cannot rank drone likelihood or quantify the effect of interference. The result reinforces the need for waveform-specific measurements and independent confusable validation.

The second DSP stage additionally measured four filtered DroneDetect `AIR_FY` prefixes (clean/BT/Wi-Fi/both) and all three received ordinary-Wi-Fi fixtures. All ordinary fixtures' highest held-out CP contrast occurs at 3.2 µs + 0.8 µs, with contrasts 0.922–0.948. The four DroneDetect prefix contrasts under this bank are only 0.00024–0.01082. They remain unknown; this small bank, start-of-file selection and unresolved mixtures do not establish absence of other OFDM timings or drone links. The four filtered runs each took about 0.07 s and 10.8–11.0 MiB RSS here; the short Wi-Fi runs were below the timer's 0.01 s resolution. These are unmatched controls, not precision/recall or matched impairment comparisons. [Seven-window report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/ofdm_structure_2026-10-03/summary.json).

The process/distributed stage additionally ran **13 capture regions / 43 independent windows**: four native DroneDetect one-second regions, six verified native-rate Zenodo files and three ordinary Wi-Fi crops. Each wideband run copied only 262,144 samples distributed over four windows, took about 130–143 ms for its measured process round trip and 0.13–0.15 s wall time, and the timer reported about 9.6–10.6 MiB peak RSS. That timer metric is not the sum of supervisor plus child RSS or the live scanner's memory footprint. The short Wi-Fi crops took about 6–8 ms round trip. [Report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/process_sampling_2026-10-03/summary.json).

All four standalone suites and six relevant opt-in root suites pass. Final parent sanitizer checks and changed scanner/panel/process checks also pass. [Process-stage software validation](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/process_sampling_2026-10-03/software_validation.json).

The broad recordings sample only about 0.22–0.44% of the declared region, whereas each short Wi-Fi crop is sampled in full. One Inspire 2 5 GHz tile produced substantial CP contrast (~0.716) under the 12.8/0.8 µs timing hypothesis; several other drone-labelled files produced much weaker values. These are selected maxima across tiles/hypotheses, without multiple-testing calibration, source separation, validated link modes or matched operating conditions. A shared timing match is not a WLAN-generation or DJI-family verdict. All classifications remain unknown/insufficient evidence.

The raw-burst stage embeds three received ordinary-Wi-Fi crops into seeded low-power noise between the prior distributed windows. Six process runs / 24 windows compare guidance with unchanged sampling. Unguided maximum CP contrast is about 0.000894; guided contrasts are 0.922–0.947, with 210,404–212,964 copied samples rather than 262,144. Guided round trips are about 105–111 ms here versus 130–137 ms unguided. This is a controlled selection check with known constructed hints, not a detector-recall, drone-classification or live-performance benchmark. All verdicts remain unknown. Fixture hashes before/after match; derived IQ is discarded. [Report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/burst_selection_2026-10-03/summary.json).

## Current limits and next DSP work

The ROI stage passes five standalone suites and seven relevant opt-in root suites. The normal default-off GUI still builds unchanged; parent ROI/queue/protocol sanitizer checks use the Release helper and fault fixtures with their original resource cap. [Software validation record](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/roi_measurements_2026-10-03/software_validation.json). The latest [ROI replay report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/roi_measurements_2026-10-03/summary.json) covers 13 regions / 43 windows: 70 returned energy/context ranges, 124 returned spectral intervals and 154 omitted contrast ranges under the cap. Native wideband round trips were about 125–139 ms; short ordinary Wi-Fi crops about 6–8 ms. These are measurement/protocol checks, not receiver-performance or detector-accuracy acceptance.

Controlled beacon-in-noise replays also return one energy interval around each guided ordinary-Wi-Fi crop, rounded to the disclosed block boundaries. Their full-tile CP values remain unchanged. [Controlled report](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/roi_measurements_2026-10-03/controlled_wifi/summary.json). A large interval count or strong CP value does not establish drone activity. Fragmentation in some wideband windows motivates null/impairment calibration before interpreting these intervals as events.

The burst-guidance software checks pass: four standalone suites, six relevant opt-in root suites, the default-off scanner equivalence suite and parent ASan/UBSan shadow/process checks using absolute paths to Release children. Default-off main/scanner preprocessor tokens match the Git HEAD baseline. The optional and default GUI builds succeed; neither was launched against a radio. [Stage validation record](/home/sudeep/Documents/newrocktest-cpp/data/wifi_drone_analysis/burst_selection_2026-10-03/software_validation.json).

Classification fields deliberately remain unknown/insufficient evidence with `model_status: not_trained`. There is no validated waveform/family acceptance policy, named drone catalogue, activity tracker or model. The experimental worker/panel displays measurements and the uncalibrated waveform evidence preview described below. Remote ID remains deferred.

Full-file DroneDetect checksums, physical-unit independence, exact link generations/modes, hardware passband calibration and internal sample continuity have not been established. The inventory is a prefix probe, not proof of full recording integrity. Separate recordings and gaps are never concatenated into an invented timeline.

The coarse FFT grid and targeted OFDM symbol-alpha measurements are not measured uncertainty bounds or calibrated significance tests. ROI measurements are experimental selection hints. Three fixed chirp research measurements and a selected-span frequency diagnostic are now implemented; the complete generic chirp/CSS, DSSS, FSK and video waveform banks remain pending. No conjugate cyclic estimator, arbitrary fine-alpha search, automatic source separation or slow hop/activity measurement is implemented. Optional band selection can suppress unwanted spectral regions, but it does not establish a single transmitter. Wideband offline results cannot be advertised for the unchanged live 20 Msps profile.

Latest identification-stage evidence: **399 recordings / 1,587 windows**, all seven local DroneDetect model groups plus Zenodo and ordinary Wi-Fi controls; all prior per-tile fields match the 13-region/43-window ROI baseline exactly. Three selected chirp maxima motivate further research; family labels stay withheld because source-mode and independent unit/session truth are missing. See the [reference validation record](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_REFERENCE_VALIDATION.md) for equations, checks, results and R1–R6 milestones. The measurement-only chirp stage used protocol version 3; the current version is 4, with unchanged resource/reply caps. No radio was opened; the user's Mini 2 test is deferred.

Next analysis work: extend preliminary quality/passband checks with receiver calibration and robust confusable/impairment rejection, route private band preparation to eligible regions, expand verified reference timing/waveform measurements, and calibrate null/impairment rejection. Broader activity context, emitter separation and qualified event tracking remain pending. Measure receiver off/on/off, total tail-adapter latency (including hint collection), producer-copy and combined resource budgets on each advertised profile before live acceptance. ML remains deferred. The [integration plan](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_DRONE_ADDON_PLAN.md) remains the broader roadmap; this delivery does not mark full M1/M2/M5/M6 or release gates complete.

## Experimental waveform evidence and source DC qualification

The new `experimental_dsp_v1` policy evaluates already computed measurements. It exposes generic CP-OFDM and linear-chirp patterns, observed quality failures and caller-declared passband geometry, while retaining unknown drone/link axes. The parent computes the bounded checks once after validated IPC decoding; no IQ DSP is added to the producer or GUI. The cached panel shows consistent patterns and expandable per-hypothesis checks. Live usable analog passband and float ADC rails remain unknown. These heuristic checks do not validate receiver quality, probability, family identity, activity, repetitions or source separation. See the [policy thresholds, benchmark misses and identification milestones](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_REFERENCE_VALIDATION.md).

`analyze --assess-evidence` requires ≤65,536 analysis samples and can use `--usable-bandwidth HZ` for a native caller-declared passband. With band selection, the existing source/ROI width declarations supply the context instead. Optional `--remove-source-dc` records/subtracts the source-window complex mean before downmixing on the private copy; all band-selection flags are required and the option defaults off. Existing preprocessing remains identical when this option is omitted. Removed mean, filter extent and original sample centers are explicit. No receiver setting or original file is changed.

Current protocol version **4** rejects older helpers; rebuild opt-in application and worker together. Resource caps remain unchanged. The 399-recording / 1,587-window preservation audit reports identical existing SCF/CP/ROI/chirp values, selection, classification and IQ hashes, with only additive evidence and selected-span frequency-mean fields. Eight standalone and all 22 registered opt-in integration/regression suites pass (including ten relevant cyclostationary/scanner suites), plus default-off scanner equivalence and ASan/UBSan math/chirp/evidence/parent-process checks. Both GUI variants build without being launched or replacing the normal build. The 96-case software benchmark is reproducible using `tools/cyclostationary/benchmark_evidence.py`; Python remains an offline validation dependency only.

## Offline chirp refinement diagnostic — 5 October 2026

`analyze --refine-chirps` adds `offline_dechirp_refinement_v1`. It is an explicitly requested offline research measurement, limited to 65,536 analysis samples. It is rejected by `shadow-replay`, unused by the live worker, and does not change IPC version 4, `experimental_dsp_v1`, family acceptance or the GUI. The new refinement function is absent from the worker's symbol table.

For each of the three existing nominal templates, five fixed slope factors (0.998, 0.999, 1, 1.001, 1.002) produce a bounded coarse search. Selected spans are dechirped and checked using full/half-span Hann spectra and unwindowed half-energy balance. Each template is capped at 25 spans / 75 FFT calls. The dechirped power band includes a declared engineering echo allowance; it is not a measured channel model. The diagnostic `shape_consistent` heuristic requires full-band fraction ≥0.8, both half fractions ≥0.7 and energy balance ≥0.25. These morphology thresholds are not calibrated drone acceptance criteria. Unavailable quantities are null; coordinates, search counts and approximate declared passband geometry are explicit.

Independent direct-DFT checks and synthetic gain/CFO/noise/clock/echo controls are implemented in `test_cyclo_refinement`. CLI tests check source/filter coordinates, nulls, the sample cap, unchanged classification and shadow-mode rejection. After this addition, nine standalone CTest suites and all 23 registered opt-in integration/regression suites pass; the default-off GUI build and scanner/lane check pass. ASan/UBSan refinement checks pass with leak detection disabled. Both GUI configurations build; no GUI or receiver was launched. These results qualify the offline software, not live RF performance.

`tools/cyclostationary/benchmark_evidence.py --refine` adds diagnostics to the 96-case baseline; `--fresh` supplies an additional 87-case grouped software/control exercise. The refinement handles some tested noise/clock/echo cases that the frozen legacy chirp checks miss, but also finds shapes in quality-invalid or passband-incomplete cases. It never overrides those qualifications or becomes a classification input. Reports are linked in the [reference-validation record](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_REFERENCE_VALIDATION.md).

`tools/cyclostationary/validate_labelled_capture.py` provides read-only, bounded-window offline recording evaluation with source preservation checks and streaming feature output. Exhaustive offline coverage is reported separately from the live capture/tile budget. The supplied Mini 2 and nearby target-off recordings were examined without opening a radio, changing originals, decoding Remote ID or training a model. The user has now parked further Mini 2-specific work. The [next-step milestones](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_NEXT_STEPS.md) prioritise general candidate qualification, cyclic measurements, calibration and waveform breadth.

## Broader cyclic and waveform measurements — 5 October 2026

The optional child now measures ordinary/conjugate cyclic autocorrelation at four lags, bounded coarse/fine cyclic-rate discovery and a separately centred holdout, plus raw-copy envelope/frequency-state morphology. The native panel exposes these in an expandable section. Protocol **5** adds strict validation of fixed partition/lag/search limits, finite scores, source bounds and internally consistent pattern flags; rebuild the opt-in application and helper together. Earlier protocol-4 statements above describe the previous stage. Existing SCF/CP/ROI/chirp calculations and `experimental_dsp_v1` checks remain unchanged. New patterns do not become classifier inputs or named identities.

Offline `analyze --expand-waveforms` exposes the same features; `--prepare-candidates` adds at most two private filter/decimation proposals under an explicit declared width. Preparation retains raw context, records geometry and source-coordinate mappings, and respects a total 64-million FIR-operation cap. It remains offline-only and is rejected in shadow mode. Unknown passband means no preparation; usable analog receiver bandwidth is never guessed from sample rate.

The [coverage/qualification record](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_WAVEFORM_COVERAGE.md) defines equations, fixed heuristics, input/resource limits and actual W1–W8/B1–B2 capability status. A 107-case / 77-group benchmark preserves every legacy JSON field and source hash; it records useful periodic/FSK/OOK/BPSK/spread-spectrum feature controls alongside noise, tone/mixture confusables and material impairment misses. Ten standalone and 24 registered opt-in regression suites pass; affected suites are checked after final changes, with default-off scanner equivalence and ASan/UBSan waveform/candidate checks. Nine full-copy-budget software process runs complete within existing deadlines at declared 20/60/200 Msps. No receiver or application was launched, and live performance/soak gates remain pending. ML, Remote ID and further Mini 2-specific work stay deferred.

## General CP timing and Barker-11 compatibility — 5 October 2026

The isolated optional child adds `bounded_ofdm_barker_discovery_v1`. Its general CP search uses linear lag autocorrelation, bounded symbol-period/prefix proposals, discovery-only phase fitting and held prefix/outside/symbol-cycle checks. Its fixed Barker-11 branch uses a squared-signal carrier/alias estimate and bounded integer chip/code-phase search with complete supported words and competing-phase measurements. At most six FFTs, 32 timing hypotheses and 32 chip/carrier hypotheses are admitted per tile, with four retained proposals per kind. It does not change legacy DSP, the evidence policy or classification. Protocol **6** is current; rebuild application/helper together. Strict parent parsing validates bounds and flags without IQ processing.

Offline `--discover-structure` exposes only this additive measurement; expanded analysis also includes it. The panel's new expandable section shows measured/consistent candidates and the unresolved drone/link/modulation meaning. Candidate-band preparation and chirp refinement remain absent from the live helper. The [coverage ledger](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_WAVEFORM_COVERAGE.md) records detailed algorithms, limitations, qualification and reproducible commands.

The 159-case / 83-group structure benchmark preserves every baseline field and reproduces all 107 historical waveform/preparation cases exactly. Clean/noisy CP-geometry and compatible Barker controls demonstrate useful support; discovery-only and tested ordinary/noise controls are rejected. Three received Wi-Fi crops match CP structure and two impaired spreading cases also match CP morphology, demonstrating competing explanations. No family acceptance, calibrated probability, generic DSSS/exact-OFDM classifier, ML or Remote ID is delivered.

Eleven standalone and 25 registered opt-in CTests pass, with affected suites rerun after test additions. Default-off GUI/scanner lane and ASan/UBSan structure checks pass (leak detection disabled). Nine full-budget four-tile worker process checks at declared 20/60/200 Msps take about 231.5–252.8 ms within unchanged deadlines/resource limits. Neither monitor nor radio was launched; live receiver and soak gates remain open.

## Separate waveform review and GUI summary — 5 October 2026

`expanded_measurement_review_v2` derives bounded rejection/ambiguity/support metadata after strict child reply validation. It reuses the frozen observed-quality checks, retains every blocker, distinguishes generic cyclic/shape diagnostics from experimental CP/code structure, and marks coexisting CP/Barker patterns unresolved. It leaves raw measurements, `experimental_dsp_v1`, classification, wire protocol 6 and receiver/security/identity ownership unchanged. No producer/GUI IQ processing is added.

The optional Wi-Fi section now leads with a six-group review summary. Support/rejection checks, timing/chip/carrier rates and code-phase margins are expandable; old reference/region/chirp tables and worker diagnostics remain available. Both Wi-Fi bands are tested. The pane is 200–480 pixels with scrolling and retains collapsed/default-off behaviour. A software-rendered ImGui fixture preview was visually inspected without launching the monitor.

A first 695-case development run exposed coloured-noise CAF confusables. V2 keeps these as diagnostics, without changing raw DSP thresholds; a frozen-rule evaluation on fresh noise seeds preserves all legacy fields in 695 cases and all previous structure fields in 159 cases. Zero of 128 fresh stationary-noise groups becomes a CP/code structure result, with a nominal 0–2.91% Wilson interval limited to the tested software generator. Raw coloured-noise matches remain; receiver/drone significance is uncalibrated. Clock/coverage misses, pulsed-noise cycles and ordinary Wi-Fi structure remain explicit. The [coverage record](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_WAVEFORM_COVERAGE.md) links reports, GUI access and reproduction commands.

Twelve standalone and 26 registered opt-in CTests pass, with affected suites rerun after v2/final GUI changes. Default-off and opt-in GUIs compile, and metadata review tests pass ASan/UBSan with leak detection disabled. Nine current full-budget worker checks take about 224.2–241.7 ms within unchanged limits; live non-interference/soak qualification remains open. No radio was opened, model trained, Remote ID decoded or Mini 2 pilot resumed.

The usual local `build/rf_monitor_gui` has also been rebuilt with `RFMON_ENABLE_WIFI_CYCLO_SHADOW=ON` so the requested GUI panel appears on the next launch. This is a local build-cache opt-in, not a change to source build defaults or runtime enablement. The main GUI/worker are built together and the usual-build scanner-lane/panel checks are run. The current monitor is not restarted and analysis remains disabled until checked in the Wi-Fi section.


## General linear-sweep expansion (5 October 2026)

`bounded_linear_sweep_discovery_v2` adds bounded discovery and a separate held check for generic up/down sweeps. Eight guarded principal phase increments are averaged before slope fitting; discovery chooses a partial span/slope with overlapping continuation support. Held positions are searched with slope/span frozen and only a nuisance carrier centre refitted. `linear_sweep_review_v1` is a separate supervisor shape review using existing quality/passband checks. No IQ processing runs in the GUI/producer, and legacy measurements/reviews/evidence/classification remain unchanged.

Protocol **7** validates the new finite fields, geometry, counts, ordering and pattern flags inside the unchanged reply/process ceilings. Rebuild application and worker together. The usual local GUI and nested helper are rebuilt with its existing opt-in build cache; source flags and runtime checkbox remain OFF by default. The panel adds a seventh review row and expandable sweep measurements/reasons in both Wi-Fi bands.

The 923-case / 225-group development report preserves all older fields and 695 historical reviews. Raw sweep matches are 54/54 clean, 54/54 at 30 dB SNR, 23/54 at 15 dB and 48/54 with the tested echo. No new matches occur in the tested stationary noise or ordinary Wi-Fi controls; alias-reset chirps can match locally. Thirteen standalone / 27 opt-in tests, usual-build scanner/panel checks and ASan/UBSan sweep/review checks pass. Fifteen four-tile worker runs take about 226.6–248.7 ms. Detailed methods, limits, artifacts and incremental S1–S4 qualification milestones are in the [coverage ledger](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_WAVEFORM_COVERAGE.md). This is experimental shape coverage, not validated CSS, drone identity or live receiver acceptance. ML, Remote ID and further Mini 2 testing remain deferred.


## Local cyclic-background support (6 October 2026)

`local_caf_background_v1` measures local spectral prominence and phase/energy support for the existing discovery-selected CAF rates/lags. Separate first/last support windows retain their own means. Same-sign coarse reference bins and four contiguous portions per window are used; no selected rate/lag is refitted. `cyclic_background_review_v1` adds supervisor metadata reasons using the frozen observed-quality/passband checks. Raw measurements, prior reviews/evidence and classification remain unchanged.

Protocol **8** validates exact reference/cache/partition counts, finite ranges, derived ratios, block bounds and support flags within the same copy/reply/process/deadline limits. At most 16 extra FFTs and eight diagnostics are produced per tile. No producer or GUI IQ processing is added. Offline `--check-cyclic-background` exposes the feature, and existing expanded-review flags include it on their existing analysis copy. Candidate-bank work is not multiplied automatically.

The usual GUI and matched helper are rebuilt. Both Wi-Fi bands show supported/raw cycle counts and an expandable background-check section. The checkbox and source flags remain default-off; the current monitor is not restarted. A synthetic ImGui preview was inspected.

The 1,617-case development report preserves baseline JSON fields/checksums, all 923 prior sweep outputs and 695 historical reviews. In 160 stationary Gaussian seed groups, 53 have raw matches and none pass the new check. Targeted periodic-line controls pass, while tones, pulses, translations and mixtures demonstrate remaining confusables. These are heuristic measurements, not calibrated significance or drone/link acceptance. Fourteen standalone / 28 opt-in tests, final affected-unit/panel checks, usual/default-off scanner tests and ASan/UBSan background/review checks pass. Instrumented parent/protocol checks use matched normal helpers to retain the 128 MiB child limit; a fully instrumented child cannot initialize under that cap. Fifteen full-budget helper checks take about 242.8–281.9 ms. Detailed methods, provenance, misses and C1–C4 milestones are in the [coverage ledger](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_WAVEFORM_COVERAGE.md). Hardware/field/family gates, ML, Remote ID and further Mini 2 testing remain deferred.

## Revision R — bounded contiguous burst expansion (8 October 2026)

One strongest eligible retained contrast-energy region per tile now receives separate local CAF/background, CP/Barker, morphology and sweep measurements. The central contiguous crop is capped at 16,384 samples; regions under 2,064 samples are skipped, counts/cropping are disclosed and separate regions/gaps are never joined. Whole-tile outputs/reviews and unknown drone/link classification remain unchanged. Native Wi-Fi views add a separate burst-local review. Protocol 9 validates selection and nested metrics with the unchanged 64 KiB reply/128 MiB child/two-second deadline caps; local PSD fractions use float32 only. A four-tile metadata stress reply occupies 60,756 bytes.

The 686-case / 134-group software report preserves all earlier fields and 131 historical cyclic-background results. None of 384 gated Gaussian controls across 32 conservatively grouped seeds passes local CP/Barker/sweep/background support. Clean/noisy CP timing is 6/9 per variant; 2,000 ppm code/timing controls miss. Barker matches also match CP and remain ambiguous. All 27 sweep variants match. Tone controls still pass cyclic checks; no probability, modulation or drone-family accuracy is claimed. Nine maximum-copy/crop helper controls take 247.884–265.997 ms. There are 15 standalone and 29 opt-in CTests, plus usual/default-off scanner checks and sanitizer unit/instrumented-parent checks with normal capped helpers. The coverage ledger records exact methods, misses, artifacts, BR1–BR4 and reproduction commands.

No radio/monitor restart, new acquisition connection, transmitter use, ML, RID or resumed Mini 2 pilot is involved. Offline timing/clock coverage can proceed without connections; later receiver-background and live off/on/off qualification need the existing USRP and labelled acquisition conditions. Live field, soak, activity-history and independent family gates remain open.

## Revision S — discovery-selected timing grids (8 October 2026)

`discovery_selected_timing_grid_v1` now checks retained CP/Barker proposals on nine bounded fractional sample-step grids. It uses one scope per tile (selected burst, otherwise whole tile), at most two proposals per kind, and one discovery-selected winner per kind. Grid/phase/geometry/carrier are frozen before held checks; no held score selects a candidate. Discovery-only scaling, private linear interpolation, fixed margins, raw-scope quality review and explicit mapping/counters avoid altering existing IQ, integer results or identity. The GUI adds Timing drift refinement. Protocol 10 keeps all process/copy/deadline caps; the combined metadata stress reply is 61,796 bytes under the unchanged 64 KiB ceiling.

The 1,196-case / 161-group report preserves all earlier fields and all 686 historical burst results. Fresh CP 96+24 controls improve from 30/60 to 60/60; Barker-8 from 20/60 to 60/60; Barker-16 from 24/60 to 56/60. Off-grid cases still miss, CP/code frequently coexist and two additional synthetic sweep cases pass generic CP checks. None of 640 Gaussian controls across 48 conservative source groups passes refined CP/code checks. These are uncalibrated pattern measurements, not physical clock, drone or link accuracy. A fixed proposal context's held-data replacement and direct source-pair/chip-word oracles pass. Sixteen standalone / thirty opt-in integration CTests, usual/default-off scanner checks and ASan/UBSan unit/instrumented-parent checks pass. Twenty-four full-budget software helper requests take 229.428–328.406 ms with matched normal capped workers.

Methods, artifacts, limitations and TR1–TR4 are in the coverage ledger. No radio, transmitter, monitor restart, model training, RID decoding or resumed Mini 2 pilot is involved. Offline timing/code controls require no new connection; receiver/background/performance qualification and independent family gates remain open.
