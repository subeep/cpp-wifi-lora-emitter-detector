# Native marker integration — 11 October 2026

## Delivered

The bounded prefix/tail and full-span diagnostic now runs in the isolated C++ cyclostationary worker. Both Wi-Fi panels use the shared panel implementation and display prefix compatibility, joint symbol compatibility, coherence, span support and search effort alongside existing candidate/blocker information. Offline `analyze --assess-evidence --check-marker-structure` exposes the same measurements; `shadow-replay` exercises the child-process transport.

Three existing chirp profiles are supported: +135.2e9 Hz/s (9 MHz), +270.2e9 Hz/s (18 MHz), and −135.3e9 Hz/s (9 MHz). A passing result means compatibility with the tested physical structure. It does not establish OcuSync identity, an exact prefix length, a drone model, or router/phone/drone category.

The diagnostic searches at most 99 prefix alignments per eligible profile, within one contiguous window of at most 65,536 samples. It checks a nominal 66.67 μs span, a 4 μs prefix/tail comparison, both halves, energy balance, shifted controls, 34 span blocks, and surrounding contrast. Existing unvalidated candidate and identity decisions remain separate. No thresholds were relaxed to fit recordings.

The live worker evaluates retained native windows. Filtered offline successes do not imply that live filtering/routing will recover the same marker. The existing optional add-on and isolation limits remain; no radio, scheduler, model training, Remote ID or dataset-original changes were made for this update.

## Verification

- All 41 root integration tests passed. The subsequently expanded CLI test also passed, including a positive synthetic prefix marker through the actual child process and exact direct/transport diagnostic agreement.
- Five native sample rates (20, 25, 100, 120 and 200 MSps), all three profiles, missing-prefix rejection, bounded inputs and malformed worker measurements are covered.
- All 54 retained development candidate evaluations agreed between the native and Python prefix/joint decisions; 50 passed joint structure. Raw/refined views can repeat a marker: these are not 54 independent observations, and 50/54 is not detection accuracy.
- The comparison covers retained candidate windows from the Leuven, Tampere, DRFF and RFUAV replay sets. It is a port regression conditioned on earlier candidate selection, not a full-search false-positive or recall assessment.
- One native Matrice recording window completed through the isolated child in 61.74 ms processing round trip (about 70 ms command wall time), under the existing 2,000 ms deadline and 128 MiB address-space limit. This single-window check is not live throughput/endurance qualification.
- The GUI preview was rendered and visually checked; new compatible/not-supported states and identity limitations are visible.
- The default build with the add-on disabled completed successfully.

Reproducible development artifacts: `data/wifi_drone_analysis/native_marker_validation_2026-10-11/{comparison.json,worker_positive.json,worker_run.json}`. The comparison records the replay executable SHA-256. Runner: `tools/cyclostationary/verify_native_marker_structure.py`. Original IQ checksums were verified on workspace derivatives before comparison; originals were not opened or modified by the comparison.

## Delivery and activation

Matched binaries were built in `build/cyclostationary-integration-check/`: `rf_monitor_gui` and `tools/cyclostationary/wifi_cyclo_worker`. Worker protocol is now version 11; app and helper must be deployed together. The primary `build/` binaries were not replaced and the running monitor was not restarted. This report describes implemented and tested source/staged binaries, not activation in an existing process.

## Remaining acceptance gates

The complete named-link segment remains open. No named drone-link family currently passes the independent acceptance gate. Remaining work is explicit:

1. Independently audited family/mode truth, recording continuity, physical-unit/session grouping, and untouched evaluation recordings. Existing exposed recordings remain development-only; filename/model labels do not prove link mode.
2. Full-search evaluation against ordinary Wi-Fi, Bluetooth, other radios, mixtures and unrelated chirps, including misses and unknowns. Measure precision/recall and false alarms on independent units/sessions with a frozen pipeline.
3. Live bounded candidate filtering/routing and timestamp/continuity-aware recurrence history. Offline segmented recurrence exists; the native integration does not establish 20 ms recurrence across separate tiles or captures.
4. Receiver passband, dropped-sample, freshness, load and endurance qualification on the intended USRP configuration. The 10 GbE transport upgrade does not itself qualify those software/receiver properties. Mini 2 live testing remains postponed.
5. Separate documented signatures and validation for Lightbridge and other proprietary link families. Current chirp compatibility must not be advertised as their identification.

Additional coding cannot substitute for missing independent evidence. Exact-model ML and Remote ID remain deferred as requested. This update closes the native prefix/span integration package, not these remaining family-identification and live acceptance gates.
