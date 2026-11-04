# Coordinated drone changes: review and verification

3 November 2026. Reviewed commit `59626d907ec3dffbb01735c630b0c7680e106a39` against parent `fecc71a`. The tracked working tree was clean at review start. This review did not change production code, start training, open a radio or operate a transmitter.

**Conclusion:** the coordinated acquisition framework builds and its existing tests pass, but the combined page and coverage semantics are not yet complete. Fix the evidence-routing and provenance issues below before declaring P2/P4 finished or using the page to judge detector recall. Then implement continuous model inputs and qualify presence/unknown decisions with independent data.

## Verification performed now

| Check | Result | Scope |
|---|---|---|
| Main Release build | Passed | Existing `build`, DJI ON, cyclostationary shadow ON; GUI and affected test targets built |
| Registered CTest suite | 73/74 passed initially; isolated remaining test passed outside the sandbox | All 74 tests have passed across the full run and isolated rerun; not one uninterrupted 74/74 run |
| Three new coordinated tests | All passed | Schedule/channelization, production scanner with fake radio, offscreen production panel |
| Existing ML IPC test | Initial and isolated sandbox runs failed; permitted outside-sandbox rerun passed | Diagnostic copy exposed `PermissionError: EPERM` when Python adopted the local socket; not a demonstrated application regression |
| Research model/input tests | 20/20 passed | Hybrid inputs, hybrid model, temporal model and profile-exposure tests |
| DJI/cyclostationary-disabled GUI build | Passed | `build-gui-revamp-minimal` GUI target; not a full test-suite run in this configuration |
| Additional production-API probes | Reproduced the functional gaps below | Eight diagnostic outputs from a separate review harness linked against production objects |
| Whitespace validation | Passed | `git diff --check` |

The first minimal-build command also requested a test target missing from that build's initial generated target list. Its GUI build completed, and a separate GUI-only confirmation succeeded. No disabled-configuration scanner-test result is claimed.

The repository's progress file records earlier live X310 checks. Those are historical evidence supplied with the changes; this review did not repeat them or verify today's network/radio connection. No fresh accuracy, false-alarm rate or live Mini 2 recognition claim follows from these software tests.

Artifacts: [build log](../data/validation/2026-11-03-drone-review/build.log), [full suite](../data/validation/2026-11-03-drone-review/ctest-console.log), [successful ML rerun](../data/validation/2026-11-03-drone-review/ml-unsandboxed.log), [sandbox diagnostic](../data/validation/2026-11-03-drone-review/ml-diagnostic.log), [20 model tests](../data/validation/2026-11-03-drone-review/model-tests.log), [disabled GUI build](../data/validation/2026-11-03-drone-review/minimal-gui-confirm.log), [probe source](../data/validation/2026-11-03-drone-review/review_probes.cpp), [probe results](../data/validation/2026-11-03-drone-review/review_probes.json).

## Findings, ordered by impact

### F1 — High: the combined page drops the native decoder's actual evidence

`coordinated::build_entries()` calls `add_detector()` on `Snapshot::observations`. The native worker emits packet rows in `native_packets` and structure rows in `o4_evidence`; its `events` array remains empty. The detector supervisor stores these in `Snapshot::native_packets` and `o4_latest`, not in `observations`. Therefore the new combined table does not show the native path's real packet/structure evidence, even though the old diagnostic panel can inspect it.

Reproduction against production `build_entries()`:

- One CRC-valid native INFP packet, no legacy-style observation → **zero combined rows**.
- One O4 candidate → **zero combined rows**.

This is not just absence of exact model names: the source evidence itself is missing from the intended central view. The existing panel test populates `native.observations` manually, which does not represent the current native worker's output contract.

The combined builder also has no cyclostationary/link snapshot input. Thus link-identifier rows are not integrated into the shared table either. The drone-only frequency visit intentionally omits cyclostationary processing, as the progress file acknowledges.

Relevant code: [entry construction](../src/coordinated_panel.cpp:56), [native output schema](../tools/drone_native_worker.cpp:39), [supervisor storage](../src/drones/detector.cpp:174).

**Required fix:** add bounded, provenance-preserving adapters for native packets, O4 evidence and eligible link results. Keep unsupported payloads and unvalidated structure candidates visible without promoting them to confirmed identities. Add tests using real native JSON output rather than fabricated legacy observations, including CRC-valid unsupported telemetry.

### F2 — High: admission is presented as completed analysis and used to infer a failed reacquisition

The scanner sets `VisitRecord::analysed = admitted` immediately after enqueueing a visit. The table displays this as “Analysed” and a percentage of acquired time. `freshness()` then treats `valid && analysed` as sufficient evidence that a later visit did not reacquire an entry after a fixed two-second grace period.

The processing lane may still be busy. A downstream native/model worker may drop the relevant samples even though its parent lane accepted the capture. The single visit flag cannot establish completion or frequency coverage for the detector that generated the original entry.

Reproduction: a valid, admitted visit with no representation of module completion produces **“Not reacquired on later visit”** even when the relevant module is pending or has dropped its work. Moreover, admitted invalid/overflowed captures contribute to `analysed_s`, while `acquired_s` only counts valid captures; the displayed percentage can exceed 100% under that condition.

Relevant code: [coverage counters](../src/scanner.cpp:1521), [documented admission semantics](../src/scanner.hpp:62), [freshness decision](../src/coordinated_schedule.hpp:230), [table labels](../src/coordinated_panel.cpp:155).

**Required fix:** distinguish acquired, valid, admitted, processed, failed and dropped spans per module/view. Use completion and eligible coverage for reacquisition decisions, and pending/insufficient-coverage states otherwise. Keep valid and invalid denominator accounting consistent.

The historical live report itself lists roughly 468M native samples analyzed and 1.09G dropped while parent-lane drops were zero. Its phrase “zero dropped analysis” is therefore too broad: it describes parent-lane admission only. Correct that documentation when implementing the ledger fix.

### F3 — Medium: the combined table retains only one model observation

The builder uses `model.history.front()` and the constant key `model/latest`. Two independent model results from different frequencies/captures produce **one row**, although the model history retains both. A newer 5 GHz result replaces the previously visible 2.4 GHz evidence; selecting `model/latest` can also silently switch the inspector to a different observation.

Relevant code: [model row](../src/coordinated_panel.cpp:100).

**Required fix:** retain bounded observation/event rows with keys based on run/session/capture/view/interval, optionally coalescing explicitly related views. Keep acquisition-based age and history independent of which result completed most recently. Test two bands and out-of-order completion without promoting every tile into an aircraft count.

### F4 — Medium: model rows use the wrong time/frequency and GUI exports lack their sample anchor

The new row uses `r.completed` as last-seen time and `r.center` as evidence frequency, ignoring `selected_offset`. A slow result is therefore aged from processing completion rather than RF acquisition, and an off-center model view appears at its parent center.

The GUI builder does not set `event_device_time_s` for model entries. `export_span()` consequently falls back to exporting the whole retained capture. The probe returned **520 ms** instead of the advertised approximately **100 ms** around the model interval. The live-check tool manually fills the missing anchor before exporting, so its successful small export does not validate the actual GUI path.

Reproduction: a 2440 MHz parent with +5 MHz selected offset is displayed at **2440 MHz**, not the selected view's **2445 MHz**. The row has no device-time anchor. The export-span test used a retained 520 ms capture and the actual GUI-built entry.

Relevant code: [time/frequency fields](../src/coordinated_panel.cpp:107), [export fallback](../src/drone_evidence.hpp:100), [live-check-only anchor](../tools/coordinated_live_check.cpp:178).

**Required fix:** preserve capture acquisition timestamps and exact sample offsets in each result, distinguish parent center from selected-view center, and use the same entry-construction path in GUI and live-check tests. Export the full model interval plus stated context when appropriate, with exact bounds rather than a misleading generic promise.

### F5 — Medium: the schedule editor rejects rates the scanner accepts

The scanner validates a schedule against `max(profile.max_sample_rate_hz, wifi_rate_override)`. The GUI editor instead receives only `profile.max_sample_rate_hz`, which is the historical 20 MSps default for the X310 profile.

Reproduction: a default schedule built for an explicit **100 MSps** override passes runtime validation with 100 MSps but fails the editor's 20 MSps validation. The Apply control is disabled, preventing otherwise accepted higher-rate schedule edits. This is directly relevant to the requested 10 Gb setup; it does not establish that the hardware can sustain 100 MSps.

Relevant code: [runtime validator](../src/scanner.cpp:1364), [editor limit](../src/coordinated_panel.cpp:419), [profile semantics](../src/config.hpp:240).

**Required fix:** use one effective configuration/rate contract for preview and execution, distinguishing a historical default from an actually qualified hardware limit. Add higher-rate override tests without opening a radio.

### F6 — Medium: historical evidence exports receive the current schedule hash

The page passes the current `CoordinatedStatus::config_hash` into the export function. `VisitRecord` and `Entry` do not retain the originating configuration hash. If the schedule changes and an older retained entry is exported, its JSON is labelled with the new schedule's hash rather than its acquisition configuration.

This is a code-path finding, separate from the eight numerical/row probes. It matters when exported evidence is later used to audit coverage or create training data.

Relevant code: [export call](../src/coordinated_panel.cpp:336), [exported hash](../src/drone_evidence.hpp:146), [visit schema](../src/coordinated_schedule.hpp:194).

**Required fix:** bind schedule/version and full run/session/capture provenance to the original observation. Export that immutable provenance; label the current UI configuration separately if useful. Test exporting an old entry after a schedule edit.

## Important remaining implementation limits

These are not all new regressions; several were explicitly deferred to P6–P9.

- **Offering the full visit is not processing the full visit.** The add-on submits consecutive chunks rapidly to a one-deep native queue. Drops and continuity resets are visible, but the scheduling/throughput problem remains. Fixing UI coverage semantics is necessary before judging progress. Do not simply grow an unbounded queue.
- **Late ML results are still discarded when a later capture changes the worker's source generation.** The old worker invalidates queued/in-flight generations and rejects completed results from them. That preserves its old current-view contract but does not implement the new plan's “finish old capture and attach the historical result” behavior. P6 needs separate observation identity and current-view state. See [source change](../src/ml_inference.cpp:154) and [result rejection](../src/ml_inference.cpp:292).
- **ML still uses sparse 8 ms views.** Continuous same-view temporal input, calibrated presence and unknown rejection are not deployed. A ready process is not a qualified detector.
- **Sub-GHz has energy/LoRa analysis but no qualified drone ML path.** Slow LoRa packet truncation at 512 ms remains a measured-test requirement.
- **The unified page does not yet embed the model's live STFT/complete inference detail or all link results.** Keep these as explicit remaining UI work rather than considering P2 fully done.
- **Longer/wider input and rate changes need runtime qualification.** Existing live notes describe a 1 GbE/20 MSps setup; they do not settle the current 10 Gb connection or passband/compute limits.
- **Actual positive live tests remain missing from the supplied progress report:** no coordinated-mode live DJI, RID or LoRa packet was established there. Ambient Wi-Fi success is useful but is not equivalent to these tests.

## What is needed to move the whole drone workflow forward

### Can proceed without any new connection

1. Fix F1–F6 with regression tests using real backend contracts, delayed/dropped module work, multiple model observations and configuration changes.
2. Revise the coverage ledger and progress wording; separate parent-lane admission from per-detector completion.
3. Implement the versioned observation/result contract and P6 continuous model-input work without changing the deployed checkpoint.
4. Benchmark deadline-aware native/ML dispatch and shared preprocessing on retained offline inputs. Compare actual analyzed time/frequency coverage rather than submitted message count.
5. Finish the unified evidence/STFT/detail UI and replay tests before operating hardware.

### Data and equipment needed for the next qualification stages

| Requirement | Current review status | Needed for |
|---|---|---|
| Dataset drive | `/media/sudeep/Storage/datasets` is absent; `/media/sudeep` and `/mnt` contain no mounted directories in this environment | Reconnect/mount it or provide its new path for broader labelled IQ replay; keep originals read-only |
| Controlled negative recordings | No new verified corpus established by this code review | Presence training and defensible false-alarm measurement; use the new recorder with documented scene/target state |
| USRP receive setup | Not accessed this turn; earlier report describes 1 GbE, while the user reports 10 Gb hardware | Later verify actual interface/image/channel/rate/readback and sustained receive/analysis load, rather than assume either condition |
| ESP32 Remote ID rig | Earlier report says not connected | Controlled positive Beacon/NAN reception through the coordinated schedule and the same GUI evidence path |
| Compatible LoRa source | No fresh positive on-air test in the supplied progress report | Derived-channel correctness under live conditions and slow-packet/dwell testing |
| Mini 2 + controller | Testing may remain postponed | Later verify controller-only/connected/active states and real deployment-domain identity/presence behavior |
| Additional independent drone/background sessions | Needed beyond repeated crops of existing sources | Named-link/model generalization, unknown rejection and mixture robustness |

No new connection is required to fix the software findings. Mini 2 testing does not need to block these fixes or offline model preparation. It does block declaring Mini 2 live recognition solved. Do not start the intended presence-model run until negative labels, independent evaluation scope and the launch caveat report are ready.

Before final qualification, agree the acceptable false drone events/hour, event recall conditions, alert delay and scan-revisit envelope. These determine dwell/model policy selection; a generic “100% accuracy” target would conceal the current coverage and evidence limits.

## Recommended next increment

Complete a **correct shared evidence and coverage layer** first: native/O4/link adapters, stable per-observation ML rows, acquisition-time/frequency provenance, per-module completion and historical export configuration. Then advance P6 and the labelled presence corpus in parallel. This gives the next model a reliable input/evaluation path and makes the Drones page useful for testing instead of hiding successful native evidence or overstating processed coverage.

Application source, datasets and checkpoints remain unchanged by this review. New files are this report and isolated validation artifacts only.
