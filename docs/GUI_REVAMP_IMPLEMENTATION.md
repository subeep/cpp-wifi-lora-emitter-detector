# Native GUI revamp — implementation and handoff

Implemented on 15 October 2026 against the working-tree baseline recorded in `GUI_REVAMP_BASELINE.json`. The user authorised implementation of `gui_plan.md`. This delivery changes presentation, UI-local state and GUI validation. Existing uncommitted integration and ML work was preserved.

## Delivered interface

- Overview, Wi-Fi 2.4 GHz, Wi-Fi 5 GHz, LoRa, Drones, Security, Captures and Settings workspaces. Drones is omitted when its build option is off.
- Persistent acquisition/health/recording context, visible overflow/stall/storage warnings and explicit Monitoring setup. Page navigation is independent of acquisition band selection.
- Calm native theme, readable local sans-serif font with a built-in fallback, responsive navigation, 1–2× display scale and comfortable/compact density. Display preferences are session-only.
- Overview cards use published run/store facts and retained observation counts, never a physical-device count or invented traffic history.
- Wi-Fi and LoRa views expose short primary column sets, every original optional column through the table header menu, retained-data search and complete selected-record detail. Wi-Fi uses clipped visible rows for its bounded packet tail. Frozen packet/hypothesis inspections do not invent capture keys.
- Wi-Fi identity and Security incident inspectors appear alongside their lists on wide workspaces and below on narrower ones, with independent scrolling in the wide layout. Original popup/modal paths remain available to existing callers.
- Security opens on Incidents, preserves the other four views and all evidence/counter/configuration information, and folds routine diagnostics while keeping material warnings visible. Existing baseline actions and reset confirmation retain their scope.
- Drones keeps independent DJI, Wi-Fi Remote ID and offline DJI views. DJI telemetry is grouped without dropping unknown groups. Remote ID has selectable source records, per-message freshness, complete field tables, raw/provenance drawers and existing JSON copy. Authentication remains unverified.
- Captures presents existing LoRa save/replay and a shortcut to the existing offline DJI interval decoder. It adds no new file format, index, export or replay backend.
- Capabilities explain implemented limits and pending validation. A session-only legacy presentation switch uses the same scanner and add-on instances; it is a navigation fallback, not a second processing path or an exact pixel reproduction of the previous interface.

## Scope and information preservation

The field/action map is in `GUI_REVAMP_INVENTORY.md`. All original GUI columns, raw-evidence routes, domain views and operator commands remain reachable. Additional already-published RF/worker measurements are displayed in details; no scientific result is recomputed.

Changes to pre-existing source files are limited to:

- `src/main.cpp`: UI composition, display snapshots and original explicit command bodies.
- `src/wifi_gui.cpp/.hpp`: tables, display filters, selected snapshots and identity inspector presentation.
- `src/lora_gui.cpp`: table presentation, display filters and complete hypothesis inspector.
- `src/security_gui.cpp/.hpp`: incident-first presentation, diagnostic disclosure, ingest-key selection and optional inline inspector.
- `src/drones/panel.cpp/.hpp`: presentation, disclosure and a UI-only offline-tab shortcut parameter.

`src/gui/app_shell.*` contains the new shell, display state, theme, capability descriptions and Overview composition. It has no Scanner, SDR, worker or storage access. CMake edits register GUI files/tests only; option defaults and dependencies are unchanged. Existing GUI tests retain their original assertions, with additional interaction checks; the DJI GUI test also rejects ImGui state errors.

The baseline SHA-256 audit confirms that all other pre-existing `src/` files are unchanged, including scanner/config, decoders, DJI Python/worker code, Remote ID parsing/tracking, security rules, persistence and cyclostationary processing. Earlier scanner/CMake changes present at the baseline belong to the previous add-on integration; they were not reverted or reimplemented. Dataset originals were not edited and no RF equipment was operated.

## Build and verification

Full-feature isolated build:

```bash
cmake -S . -B build-gui-revamp \
  -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DRFMON_ENABLE_DJI_DRONEID=ON \
  -DRFMON_ENABLE_WIFI_CYCLO_SHADOW=ON \
  -DRFMON_BUILD_CYCLO_OFFLINE=OFF
cmake --build build-gui-revamp -j 4
ctest --test-dir build-gui-revamp --output-on-failure -j 1
```

The isolated build initially selected a Python without NumPy. Its baseline had 40/42 CTests pass; the two DJI failures were interpreter/dependency failures. `/usr/bin/python3` has the existing required dependencies and matches the original build's interpreter family. Selecting it fixed those failures without modifying backend code or installing packages. Its additional available test prerequisites register the full existing suite.

Final qualification:

| Check | Result / scope |
|---|---|
| Registered full-feature suite | 49/49 CTests passed, including scanner, security, LoRa, DJI, Remote ID, cyclostationary and GUI tests |
| Existing Wi-Fi / LoRa GUI checks | Passed; original assertions retained and now registered in CTest |
| Production shell interactions | Real mouse navigation across all pages; setup open/close, display scales and layout switching emit zero fixture commands; explicit fixture action emits one |
| Inline inspection | Production identity, packet and incident views exercised with clicks; zero passive baseline commands; no ImGui recovery errors |
| DJI / Remote ID | Existing worker/reference-vector/tracking/isolation checks pass; production renderer checks pass |
| Build without add-ons | Main GUI links in `build-gui-revamp-minimal` with Drones OFF and cyclostationary OFF |
| Independent optional configurations | Main translation unit passes syntax compilation with Drones OFF / DSP ON and Drones ON / DSP OFF; these mixed variants were not separately linked |
| Source audit | No pre-existing backend source hash changed from this task's baseline |
| Diff formatting | `git diff --check` passes |

The UI test command uses fake display actions, not a radio. The original explicit acquisition callbacks were retained and reviewed against the baseline. No fresh Connect, transmission or live receiver load test was performed. Passing fixture checks is not a claim of over-the-air qualification.

### Intermittent timeout in the unchanged monitor test

Several complete parallel runs passed. One subsequent `-j 4` run timed out in the unchanged `remote_id_monitor` test (48/49 passed). Five isolated repeats passed. The final full serial run passed 49/49. The failed log is preserved as `gui_validation/parallel-intermittent-timeout.txt`.

The current monitor intentionally uses a nonblocking producer try-lock and counts dropped submissions, while this test waits for every submitted frame to increment `examined`. A contention drop is a plausible explanation for the timeout; the failed run did not log its drop count, so the cause is not proven. Neither the monitor nor that test was modified to make the result pass. This remains a test/monitor-contract follow-up outside the presentation change, and parallel-suite stability is not claimed.

## Performance and visual review

A matched EGL/Mesa benchmark rendered the same 1,000 packet rows and identity fixture at 1600 × 1000, using the same theme and software-render environment. After 10 warm-up frames, 120 measured frames with `glFinish` averaged approximately **5.64 ms before** and **3.53 ms after**. The original renderer was compiled from the saved working-tree source and compared with the new renderer. This is a single local fixture benchmark, not a guaranteed frame rate, CPU budget or receiver duty improvement. New default disclosure and row clipping reduce rendering work.

The production shell/Overview fixture additionally averaged around 1 ms/frame locally. High-density RF captures, busy worker combinations and sustained live acquisition remain unqualified. UI snapshot caching is display-only at about 5 Hz; receiver/worker timing is unchanged.

Reviewed fixtures cover disconnected/retained information, material errors, unknown values, long evidence, selection, DPI scaling and optional builds. Screenshots in `gui_validation/` are labelled synthetic and are not evidence of actual attacks or aircraft reception:

- `overview.png`: production shell and Overview cards, with synthetic retained counts.
- `wifi-identities.png`: identity list and inline inspector.
- `wifi-packets.png`: packet table and frozen details.
- `security.png`: incident-first workspace and inline evidence.
- `remote-id.png`: native Remote ID source/message renderer; existing standalone fixture harness uses its own default theme.

Keyboard focus/navigation and Ctrl+F packet/hypothesis search are available. Full native accessibility compliance, every platform/font combination, exact full-screen pixel parity at all DPI settings, and fresh live performance are not claimed. Backend-dependent enhancements excluded by the plan remain excluded.

## Launch and rollback

The existing `build/rf_monitor_gui` was not overwritten, and no running monitor was restarted. To review the new interface, launch the isolated binary in an available desktop session:

```bash
LIBGL_ALWAYS_SOFTWARE=1 /home/sudeep/Documents/newrocktest-cpp/build-gui-revamp/rf_monitor_gui
```

The application starts disconnected; normal existing persistence paths are still used by the production application. The test harnesses use isolated/synthetic sources. Do not run a second connected radio session concurrently with another monitor.

Use Settings → Display preferences → Legacy presentation for the temporary navigation fallback. It preserves backend instances/settings. To return, use the fallback's Display tab → Return to modern workspace. Returning to the original executable also remains possible; decide explicitly whether to stop/relaunch an active receiver.

For source rollback, `GUI_REVAMP_CHANGES.patch` contains only changes from this task's saved baseline, including new shell/test files, and excludes the previous add-on/backend changes. First inspect the patch and run `git apply --reverse --check docs/GUI_REVAMP_CHANGES.patch` against the current tree. Apply only if it still matches and source rollback is explicitly desired. Do not reset/clean the repository or restore all files from HEAD: the starting tree already contained other uncommitted work. The source patch does not delete review documentation, captures or datasets.

## Milestone handoff

G0 inventory/baseline, G1 shell, G2 Security, G3 Wi-Fi/LoRa, G4 Drones and G5 captures/DSP/settings presentation are delivered. G6 software/fixture qualification and scope audit passed as described above. G7 documentation, isolated launch and rollback are delivered; the temporary legacy presentation remains available. Live hardware qualification and removal of that fallback are separate follow-up decisions. No backend enhancement is required to use this UI delivery.


## Sample-rate setting and gain layout — 22 October 2026

Monitoring setup (also shown in Settings) now stages an optional Wi-Fi IQ
sample-rate request in MSps. Both Wi-Fi bands use the same request on the next
Connect/Reconnect. The supported application input range is 1–200 MSps; this
does not promise that UHD or each decoder supports every value. Unchecking
Custom Wi-Fi sample rate restores the original device default. Explicit requests
bypass the historical X310 20 MSps default; LoRa scan/listen rates are unchanged.

The UI shows requested versus actual rate from the latest Wi-Fi capture.
Empty/failed captures clear actual-rate availability. Invalid/nonfinite input
prevents Connect/Reconnect; a running scanner rejects rate reconfiguration until
stopped. AGC and manual gain are on the row immediately below Threshold.
These values are session settings, not a persistent preferences file.

The processing lane uses the requested rate for memory admission and drains to
inline processing if the returned actual rate exceeds its two-step budget.
UHD streamer caching distinguishes the request from its coerced actual rate, so
an unchanged non-exact request no longer rebuilds the stream every capture.
The GUI notes the existing legacy OFDM actual-input range of 20–64 MSps.

Validation: the GUI builds in build-ml-inference. Scanner tests use a fake radio
to cover default/custom rates, both Wi-Fi bands, requested/actual metadata,
coercion beyond the overlap budget, failure clearing, invalid input, stopped-only
configuration and preserved LoRa rates. Scanner Wi-Fi/LoRa, serial lane, GUI shell
and inference regression suites pass. The inference test requires permission to
open its inherited local Unix socket in this environment; its synthetic worker
does not load a model or use hardware. No new hardware-rate qualification is
claimed.
