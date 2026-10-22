# RF Monitor — complete native UI revamp plan

Updated: 15 October 2026

Status: UI implementation delivered in `build-gui-revamp`; verification and limits are recorded in `docs/GUI_REVAMP_IMPLEMENTATION.md`. Backend changes remain outside scope.

Target: the existing C++17, Dear ImGui, GLFW and OpenGL application

Planned add-on, 20 October 2026: a separate **Model Inference** tab is specified in
[Drone model and inference plan](docs/DRONE_MODEL_AND_INFERENCE_PLAN.md).
The user requested planning this addition, with model/data improvement first, then an
optional inference worker consuming existing acquisition data. The tab will provide
Start/Stop Inference, results, correctly labelled probabilities/scores, and capture/model/
runtime details. It must preserve every existing function. This is a separate future
integration scope, not permission to add backend work to the presentation-only revamp
below. No model training or tab implementation is started by this plan update.

Implementation update, 21 October 2026: after subsequent user authorization, the separate
Model Inference addition was implemented and tested in `build-ml-inference`. It remains
experimental; expanded training and live qualification are incomplete. See
[the inference implementation report](docs/DRONE_ML_LIVE_INTEGRATION.md).

## 0. Implementation handoff — read before changing code

### Task and authority

This section is the implementation brief for GPT-6.1 Sol or another implementing agent. The initial document-editing task authorised this plan only. The user subsequently authorised implementation with “implement the gui_plan”. For future handoffs, begin implementation only when the user explicitly requests it. Once authorised, proceed through the milestones without requesting permission for routine, reversible UI work within these boundaries.

**Implement a presentation-only revamp of the existing native application. Preserve every existing feature, field, command, default and processing result. Do not rebuild the application from scratch or use this work to finish pending backend features.**

Read applicable repository instructions, this entire plan, the current GUI code and the integration documents before editing. Current code is the authority for implemented behavior; this plan is the authority for the requested presentation and scope. Numerical limits and test counts in documents can become stale. If they disagree with current code, document the discrepancy and correct the proposed label rather than changing backend code to match this document. A later explicit user instruction can revise scope; record that revision rather than silently assuming it.

### A. Establish a baseline before edits

1. Inspect `git status --short`, tracked diffs and relevant untracked files. The working tree may already contain completed but uncommitted DJI, Remote ID, scanner, CMake and unrelated ML work. Treat that working tree, not just `HEAD`, as the starting baseline. Do not discard, stage, commit or rewrite someone else's changes.
2. Record the baseline commit, changed-file list, relevant file hashes and build configuration in a handoff/check log. Store only review artifacts in the project; do not copy IQ datasets, secrets or large build outputs into it. A Git diff alone omits untracked files; explicitly account for those files too.
3. Inspect the current CMake options/cache and documented build commands. Use a separate build directory for the revamp, preserving the existing build and running executable. Reproduce relevant options/dependencies; do not copy a CMake cache into another directory or silently change flags to make tests pass.
4. Discover registered tests with CTest and inspect GUI test targets. Run the relevant baseline tests before editing. Record exact commands, options, pass/fail/skip counts and any pre-existing failure. A missing dependency or absent test is not a pass.
5. Capture representative existing screens using offline/synthetic fixtures and complete the field/action ledger from section 11. Include hidden sections and compile-time-disabled features, not only the currently visible screen.
6. Identify whether a monitoring process is already running through read-only inspection. Do not stop/restart it, connect to a USRP, use a transmitter or acquire new RF data for this UI task. Launch any preview isolated and disconnected, with temporary persistence paths supported by the existing harness. If isolation is not possible, use offscreen tests instead of risking production stores.

### B. File and behavior boundaries

| Area | Permitted changes | Forbidden under this handoff |
|---|---|---|
| GUI files and new `src/gui/` modules | Layout, styles, labels, display-only filters, selection, inspectors and wrappers around existing draw functions | Detection, inference, decoding, retention or acquisition decisions in presentation helpers |
| `src/main.cpp` | Carefully bounded UI composition and event wiring | Changes to startup/shutdown order, scanner/worker construction, ownership, default enables, thread lifetime or automatic connection |
| Existing domain panels | Extract/rearrange rendering while reusing the same sources and actions | A replacement decoder/store, copied evolving backend state or a second live processing path |
| `CMakeLists.txt` | Minimal registration of GUI source files and targeted GUI tests, if needed | Dependency upgrades, global compiler changes, changed feature defaults, removal of existing targets/tests or new networking/runtime frameworks |
| Tests and fixtures | GUI behavior/command-parity tests and clearly labelled bounded synthetic display fixtures | Weakening assertions, removing failure cases, changing golden scientific results to fit the new UI or modifying recorded dataset originals |
| Scanner, config, DSP, protocol, worker, storage and security implementation | Read for contracts and behavior | Changes to `src/scanner.*`, `src/config.hpp`, decoder/worker logic including `src/drones/remote_id.*`, rules, thresholds, schemas, clocks, budgets or persistence |
| Documentation | Field/action ledger, validation evidence and current UI usage | Claiming new protocol support, live qualification, identity authentication or complete detection |

A filename containing “GUI” is not proof that every line is presentation code. Existing GUI files can contain file operations, replay launches or callbacks. Preserve their operation, inputs and limits while relocating controls. Conversely, `src/main.cpp` is not blanket permission to edit application lifecycle.

Do not change sample-rate caps, scanning schedules, callback order, observer registration, queue sizes, stale/expiry rules, source keys, decoder versions, FCS/CRC checks, baseline logic or identity merges. Do not add background file discovery, map services, telemetry, auto-updaters or dependency installation. Leave dataset and recording originals read-only.

### C. Interaction contract — verify, do not assume

Create an action ledger with each existing callback, its arguments, prerequisites, scope and expected number of invocations. The redesign must obey this matrix:

| User interaction | Allowed effect |
|---|---|
| Navigate, sort, filter, select, expand/collapse, resize, change density or switch layout | GUI-local state only; zero acquisition/analysis/storage commands |
| Explicit Connect/Disconnect or Monitoring setup action | Exactly the existing operation with the selected arguments and current application semantics |
| Explicit decoder toggle | Only that decoder's existing enable/disable callback; preserve current reset/history behavior |
| Explicit baseline freeze/reset | Existing scoped command, preserving confirmation and selected profile; no extra baseline reset |
| Existing Save/Decode/Replay action | Existing bounded operation once, with the same input checks and isolated result destination |
| Copy evidence | Clipboard-only formatting of available data; no new evidence, file export or backend mutation |

Separate `viewed_band` from the actual acquisition setting. Do not bind the new navigation selection to the old band-changing radio-button callback. Closing an inspector or collapsing a decoder section must not stop a worker. Hiding a page must not suspend processing that currently continues in the background. GUI refresh throttling applies to drawing/snapshot reads only, never the processing lane.

Never call a side-effecting callback just to draw the current state. Avoid duplicate actions caused by rendering a control in both its original and new location. Use stable widget IDs scoped to page/source/message; do not use moving table indices as record identity. Preserve lifetime-safe snapshots; do not keep dangling pointers into replaced snapshots or hold a backend lock while rendering.

Only the current explicit backend response establishes applied state. Show requested/pending/unavailable accurately; do not manufacture success or failure states absent from the contract. Where exact command observation is unavailable, test through UI-local adapters/fakes without changing production backend interfaces.

### D. Incremental implementation procedure

- Complete G0, then introduce the shell around existing widgets before reorganising their contents. The intermediate app must remain usable.
- Implement one milestone at a time. Keep changes small enough to explain and revert without touching other work. Do not mass-format, rename unrelated files or perform opportunistic cleanup.
- Keep all existing controls operational until their replacement has passed field and action parity. A temporary legacy presentation must share the existing backend objects; never instantiate another scanner or decoder for comparison.
- For each milestone, update the ledger, build affected targets, run relevant tests and inspect rendered results. After integration, run the full applicable regression suite. Do not repeatedly rerun unchanged suites without a reason, but do not defer newly affected regressions until the end.
- If a layout requires unavailable metadata, use an honest unavailable state or preserve the old detail workflow. If it requires a new backend contract, stop that item, document the dependency and continue independent UI work. Do not implement the dependency without explicit scope expansion.
- Preserve the completed state in the handoff log after each milestone so a later agent resumes from evidence instead of repeating or guessing work.

### E. Mandatory verification gates

Use repository-supported commands discovered during baseline setup. Example command shapes are `cmake --build <isolated-build> --target <existing-target>` and `ctest --test-dir <isolated-build> --output-on-failure`; replace placeholders with verified values. List tests first and do not treat a zero-test run as success. Some GUI executables may need their documented offscreen invocation in addition to CTest.

| Gate | Required evidence |
|---|---|
| Build compatibility | Main GUI builds with the relevant existing feature configuration; verify Drones enabled/disabled and cyclostationary enabled/disabled variants relevant to touched code, without changing production defaults |
| Existing behavior | Applicable Wi-Fi, LoRa, scanner-lane, security, DJI, Remote ID and cyclostationary regressions still pass; retain exact existing assertions |
| Command parity | Tests exercise navigation, expansion, sorting, selection and layout switching and observe zero backend commands; explicit controls issue only the expected scoped commands |
| Data parity | Identical fixture data yields identical displayed scientific values and raw evidence; formatting never changes units, precision, null semantics, integrity labels or attribution |
| Information completeness | Every ledger item has a reachable destination and verification evidence; dynamic/raw fields remain inspectable |
| Drones isolation | Independent enable controls, message-specific ages, unavailable replacement locations, identity/session changes, stopped history and offline/live separation remain correct |
| Resource behavior | Compare bounded snapshots/render overhead against baseline on the same setup; no render-thread decoding, new unbounded cache, long-held backend lock or duplicate worker |
| Visual/interaction | Inspect actual screenshots and interactions at normal and high DPI, narrow and wide windows; include busy, disconnected, stale, failed, malformed and missing-data states, long strings and large counters |
| Scope review | Review the diff against the saved working-tree baseline, including untracked files; every changed hunk belongs to UI, its tests or documentation |

Existing tests alone do not prove the new controls are wired correctly. Add meaningful GUI command/selection checks where coverage is missing; use fixtures and callback fakes rather than RF equipment. Test source-string escaping and readable long/raw data without executing or interpreting it. Do not claim full accessibility or live non-interference solely because an offscreen screenshot and unit tests pass.

If a check fails, determine whether it is pre-existing, environmental or introduced. Fix introduced UI regressions before advancing; do not modify backend behavior or suppress the test. Report unrun checks and their reasons explicitly. Completion requires all applicable gates to pass, or a clearly documented limitation accepted by the user; never report a blocked gate as complete.

### F. Rollback and delivery

Rollback means reverting only this revamp's known UI changes or selecting the temporary legacy layout, while preserving the original dirty working tree and all data. Do not use `git reset --hard`, broad `git checkout`/restore, `git clean`, dataset deletion or blanket file replacement. Do not overwrite the working binary or restart a monitor merely to deliver a preview. Use the isolated build until the user explicitly requests deployment/relaunch.

Deliver a concise report with: completed milestones; modified files and their scope; the completed field/action ledger; build flags and exact test outcomes; reviewed screenshot paths; measured performance comparison where available; known limitations; and how to run or roll back the UI safely. Mark any remaining task honestly. Do not create a commit, publish a PR or deploy unless requested.

**Final acceptance question for the implementing agent:** Can every original feature still be used, can every original piece of information still be inspected, and do passive UI interactions leave the receiver, decoders, evidence and persisted data unchanged? Support the answer with the gates above, not confidence alone.

## 1. Objective and boundaries

Make the application easy to operate during monitoring, useful for investigating an incident, and honest about what the receiver actually observed. Keep security incidents, receiver health and decoded drone broadcasts easy to find. Keep LoRa decoding, Wi-Fi identities and experimental link analysis accessible without forcing the operator to read every diagnostic at once.

This is an incremental redesign of the current native application. Reuse its scanner, snapshots, decoders, security rules, persistent stores and drawing functions. Do not introduce a browser frontend, replace Dear ImGui, or require a new DSP architecture.

The first delivery must work with the current receiver and data contracts. Features requiring new metadata, history, indexing or backend behavior are excluded from this revamp and listed only as separate future work. A polished display must not imply that an unfinished detector exists.

Non-negotiable behavior:

- Viewing another page must not silently change the radio, band, channel, gain or active recording.
- Show the active acquisition band and channel even when inspecting another band's saved information.
- Distinguish measured waveforms, FCS/CRC-checked protocol observations, claimed identities, suspected incidents and independently validated conclusions.
- Keep missing measurements unavailable; never turn missing data into zero, quiet traffic or a benign verdict.
- Keep live, replay and synthetic-test data separate in both state and presentation.
- No transmitter operation, mitigation, ML model, new decoder or new named-link acceptance is introduced by the GUI work. Present the existing DJI and Wi-Fi OpenDroneID decoders without changing their processing.
- Preserve existing identity files, event schemas and incident/baseline persistence during the initial migration.

## 2. Recommended application layout

Use one fixed native workspace with a persistent top bar, a narrow navigation rail, a central content area, an optional right inspector and a compact footer. Start without docking; optional docking can come later if the repository's ImGui version supports it and operators need it.

```text
┌ RF Monitor ─ LIVE / REPLAY ─ Device ─ Active band / channel ─ Health ─┐
│ Overview       │ Page title, data source, freshness, filters        │
│ Wi-Fi 2.4 GHz  │                                                   │
│ Wi-Fi 5 GHz    │ Main table / frequency map / selected workflow     │
│ LoRa          │                                   │ Inspector     │
│ Drones        │                                   │ Identity      │
│ Security      │                                   │ Details       │
│ Captures      │                                   │ Evidence      │
│ Settings      │                                   │ Limitations   │
├ Connection │ capture health │ recording │ queue loss │ last update ┤
└────────────────────────────────────────────────────────────────────┘
```

### Persistent top bar

Keep these visible on every page:

1. **Viewed source:** Live receiver observations, retained history, Offline file, or Synthetic fixture. This describes the selected workspace; show ongoing live acquisition separately when viewing offline results.
2. **Radio:** B210/X310, connection state, Connect/Reconnect/Disconnect.
3. **Active acquisition:** band, sweep/fixed mode, selected channel or LoRa lock frequency.
4. **Receiver health:** receiving, processing, stalled, degraded, disconnected or starting.
5. **Incidents:** available count labelled by its scope, with a shortcut to Security. Do not infer a full-store open count from a truncated published list.
6. **Recording:** off, recording, pending reconnect, failed or retention limit reached/rotating, using actual available state.

Connection and receiver health are separate: a connected device may be stalled. Recording state must describe the configuration currently in use, not merely the checkbox selection.

The control opening the acquisition editor should be labelled **Monitoring setup**. Selecting a navigation page only changes the view. If a different band is being viewed, show: “Viewing retained 5 GHz data; receiver is monitoring 2.4 GHz channel 6.” Offer an explicit **Monitor this band** action.

### Sizes and scaling

| Window size | Layout |
|---|---|
| 1600 × 900 and larger | Approximately 180 px navigation, flexible centre, 360–440 px inspector |
| Around 1280 × 800 | Approximately 150 px navigation; inspector opens on selection and can be collapsed |
| Below 1100 px width | Compact navigation; inspector replaces the centre temporarily, with Back preserving filters/selection |

Sizes are logical units scaled for DPI, not hardcoded physical pixels. Support 100%, 125%, 150% and 200% scaling. Use a readable default font, tabular/monospaced numeric values where useful, and text labels alongside all status colours.

## 3. Navigation and page responsibilities

| Page | Primary purpose | Initial implementation |
|---|---|---|
| Overview | Answer what is being monitored, whether evidence is healthy, and what needs attention | Compose existing snapshots; no new detection logic |
| Wi-Fi 2.4 GHz | Inspect channel observations, packets, identities and DSP measurements | Reuse current Wi-Fi widgets |
| Wi-Fi 5 GHz | Same tools, with its actual channel plan and band-specific scope | Same reusable Wi-Fi page with a band parameter |
| LoRa | Inspect RF observations, decoded PHY packets, LoRaWAN candidates and emitter clusters | Reuse current LoRa widgets |
| Security | Investigate Wi-Fi incidents and understand coverage/baselines | Promote the existing security panel into a full workspace |
| Drones | Inspect DJI broadcasts, Wi-Fi Remote ID and DJI offline intervals | Reuse the existing three Drones subviews and independent enable controls |
| Captures | Save/replay supported recordings and inspect their provenance | Existing LoRa save/replay and a shortcut to existing DJI offline review only |
| Settings | Configure acquisition, display preferences and inspect capability/support limits | Reuse existing setters; no arbitrary hardware controls |

Keep **Drones** as a first-class page for the existing decoded broadcasts. Label identities as reported and unauthenticated. Keep **Experimental link analysis** under Wi-Fi; waveform compatibility is not a drone inventory. Do not merge broadcast sources, BSSIDs and RF clusters into a single physical-device count.

## 4. Overview page

Use four compact summary cards, not a wall of counters:

- **Acquisition:** active receiver/band/channel, sweep position and latest capture status.
- **Coverage:** sampled and analysed exposure, known gaps, excluded captures and losses. Include run/channel scope.
- **Security:** open suspected incidents, new observations and most recent incident timestamp. No “network safe” score.
- **Observations:** accepted Wi-Fi frames or eligible LoRa packets and last update. Identity counts must say BSSIDs, clusters or retained rows, never unqualified “devices”.

Below the cards:

1. The existing frequency map for the active acquisition mode.
2. A compact incident list: rule, claimed AP/client, band/channel, severity, last observation and evidence state.
3. A collapsible receiver/storage health list ordered by whether it affects observation availability.

Every card opens a relevant filtered page. Zero incidents with poor coverage should show “No incidents observed; coverage degraded,” not a green all-clear banner.

All cards use only published values, with their actual scope; omit an unavailable aggregate and show its reason instead of reconstructing it from a truncated list. Drones gets a compact shortcut showing its two independent decoder states and available retained-source counts, labelled by source type.

Use only existing run totals initially. “Last five minutes” graphs and rates require timestamped bounded history; do not derive them from cumulative totals or the truncated recent-frame table.

## 5. Wi-Fi pages

The two Wi-Fi pages share layout and implementation. Their selected inspection band is independent of the acquisition band.

### Page header

- Band and source badge; latest-data age and run/session identifier in the inspector.
- Actual acquisition channel when this band is active; otherwise a retained-data notice.
- Controls for Sweep / Fixed channel in the explicit Monitoring setup editor.
- A visible link to **Security on this band**.
- Supported channel list and decoder limits available through **Coverage and limits**.

### A. RF observations

Retain the current frequency map and active-emitter table. Recommended default columns:

`Frequency | Channel | Bandwidth | Observation label | Relative power | Sources estimate | Last seen`

Put first seen, cycles observed, band, grouping method and measurement explanations in the inspector. Make it clear that an RF row may be an occupied-frequency group, not a physical device. Preserve last-seen ages; do not remove or relabel old rows as live.

Use labels such as “Wi-Fi-like,” “OFDM structure,” and “Possible narrowband signal.” Do not turn the existing BLE/Zigbee hint into protocol detection. The source-count estimate should say **inferred lower bound**, with co-channel and multi-BSSID limitations.

### B. Packets

Default columns:

`Time | Channel | PHY / rate | Frame type | Decode / FCS | Claimed BSSID | SSID | Length`

Filters: retained text search, channel, PHY, FCS/decode state, management/control/data and security-only decoding. Searching this table must say **Search retained rows**.

Inspector groups:

1. Capture provenance and timing, when supplied by the source.
2. MAC roles, flags and frame-specific fields.
3. Decoded identity metadata and its originating observation.
4. RF measurements and gate failure reasons.
5. Raw bytes, only when retained in the corresponding source.

The ordinary `WifiPacketRow` does not contain all security event bytes or an exact capture/frame key. Do not correlate it to a security event by display time, BSSID or approximate frequency. Initially offer a clearly labelled address/time filter. Exact cross-navigation requires the separately scoped correlation work described under data contracts.

Default table follows newest rows only while **Follow live** is enabled. Scrolling or selecting a row disables auto-scroll, not acquisition. Provide **Return to latest**. Keep selection stable as rows arrive or sort order changes.

Show retention clearly: “Showing retained packet rows, maximum 1,000; not exhaustive capture.” Display burst-limit omissions using available counters; do not invent the number of RF packets missed.

### C. Identities and RF clusters

Default columns:

`Record type | BSSID / cluster ID | Last-known SSID | Advertised security | Vendor hint | Monitored / advertised channel | Last seen`

Use different badges for **Decoded BSSID** and **Provisional RF cluster**. Show raw claimed addresses without implying ownership or authenticated identity.

Inspector groups:

- Identity origin: FCS-valid beacon/probe response or heuristic RF cluster.
- First/last observation and decoded observation count.
- SSID and WPS field provenance, including last-known value age.
- Advertised security, PMF and capability information.
- RF history and gate status, with its own measurement timestamps.
- Retention and storage warning state.

Keep every currently available metadata field accessible in the inspector. Do not place every field in the initial table. Offer **Show related incidents** as an address-based filter with an explicit “claimed-address association” label, not physical attribution.

### D. Experimental link analysis

Keep analysis opt-in. The first line must answer:

`Analysis off / waiting / measured / stale / unavailable — sampled windows, not full-channel coverage`

Present results in this order:

1. **Protocol facts:** existing CRC/FCS-checked decoder observations associated with the same supported capture/window.
2. **Waveform evidence:** CP timing, Barker-11 compatibility, cyclic structure, two-frequency/two-level shapes and chirp/sweep measurements.
3. **Named research candidates:** OcuSync-compatible marker evidence, with “Unvalidated identity” beside the name.
4. **Missing evidence:** continuity, passband qualification, recurrence, independent validation or insufficient sample support.
5. **Advanced diagnostics:** source coordinates, discovery/holdout measurements, thresholds, worker timings, drops, restarts and protocol version.

Use “compatible,” “not supported in sampled window,” “competing patterns,” and “insufficient evidence.” Never use a probability bar for coherence or a green verified-drone badge for a chirp match.

Expose the sample budget and window duration at the actual sample rate. Keep individual windows selectable; never draw them as a single uninterrupted trace. Offline-only filtering, routing and recurrence functions must remain labelled offline until their live backend is implemented.

## 6. Security workspace — first implementation priority

Default to the **Incidents** view. Keep an always-visible coverage strip above it so alerts can be interpreted against receiver health.

### Incident list

Default columns:

`Severity | Rule | State | Band / channel | Claimed AP | Target / scope | First / last observation | Observation count | Evidence status`

Filters: rule, band/channel, claimed address, open/closed/restored, severity and evidence availability. Keep severity and confidence separate. Use “Open incident” for the store's current state, not “Attack definitely continuing”. Show the last observation and current coverage alongside it.

Readable labels mapped to stable backend rule IDs:

| Backend rule | Display label | Required qualification |
|---|---|---|
| `management_disconnect_flood` | Suspected disconnect flood | Observed rate/baseline evidence; client impact unverified |
| `historical_beacon_replay` | Historical beacon replay anomaly | Old/newer/repeated beacon evidence; authentication unverified |
| `management_transaction_replay_anomaly` | Connection transaction replay-like anomaly | Experimental; supported authentication/association exchanges only |
| `eapol_handshake_replay_anomaly` | EAPOL handshake replay-like anomaly | Experimental; no MIC or key-installation verification |

### Incident inspector

Use an inspector for routine investigation instead of requiring a blocking modal. Preserve the existing popup initially as a fallback.

Order the inspector as follows:

1. **Finding:** readable rule name, suspected tier, severity, qualitative confidence and latest observation.
2. **Why it triggered:** actual values versus configured thresholds. Use exact strings for 64-bit replay counters, not floating-point conversion.
3. **Evidence sequence:** original/newer/repeated frames or flood-window measurements.
4. **Observation quality:** receiver profile, gaps, exclusions, caps and source availability.
5. **Other explanations:** retries, delayed traffic, restarts, reconnects, spoofed responses or missing session context as supplied by the rule.
6. **Provenance:** rule/config version, run, radio session, capture/frame references and retained raw-byte excerpts.

For historical beacon replay, show A → newer B → A. For transaction anomalies, show the original request/response, newer request/response and reobserved old request. For floods, show counted units, usable exposure, threshold and target/AP-wide path. Use timeline plots only for actual retained points. Render capture gaps as gaps, never as zero activity.

Actions:

- **Inspect frame:** exact lookup only when an ingest key and retained source exist.
- **Filter related activity:** explicitly an address/context filter, not transmitter attribution.
- **Copy evidence summary:** local clipboard; include source and uncertainty labels.
- Evidence-bundle export is excluded: it needs new functionality. Preserve existing copy actions; a GUI-only formatted clipboard summary may use already-published fields without adding evidence.

Do not add Block, Jam, Deauthenticate or Attack confirmed buttons. Acknowledge/mute/review notes require a separate annotation store and are deferred; they must never modify detector evidence or close an incident automatically.

### Coverage, frame types, recent frames and baselines

Reuse the current five security tabs, with Incidents first:

- **Coverage:** sampled/analysed/usable exposure, known and unknown gaps, caps, queue drops and receiver profiles. Distinguish acquisition loss from decoder rejection and display retention.
- **Frame types:** observed counts by PHY/type/subtype and parsing state; no percent-of-all-air-traffic claims.
- **Recent frames:** bounded frame inspection with explicit FCS, body state, protection and timing. Show that the live snapshot normally retains only 256 recent frames.
- **Baselines:** learned exposure, included/excluded windows, reference statistics, receiver configuration, operator freeze and detector holds. Explain why a baseline is not mature.

Freeze/unfreeze can use the existing command callback. Reset must show the selected baseline/profile and consequences in a small confirmation dialog. Restarted/restored state must remain distinguishable from uninterrupted monitoring.

### Rule coverage matrix

Add a compact “What is monitored?” drawer: enabled, prerequisites, eligible/evaluated count, exclusions, validation scope and unsupported attack categories. Distinguish a rule with zero eligible inputs from a rule that examined traffic and found no anomaly.

LoRa security is evidence monitoring only. Do not include it in an “all protocols protected” summary.

## 7. LoRa page

### Acquisition controls

Show Rotating frequencies / Locked frequency, requested and available actual sample-rate metadata, capture duration and next-capture-save state. Frequency locking must explicitly explain that the wideband energy scan is skipped.

Keep the laboratory-codec toggle in an Advanced section with a persistent **Nonstandard laboratory mode** badge while active. It must never look like a sensitivity or compatibility boost.

### A. RF observations

Reuse the existing map and active-emitter list. When locked outside 863–868 MHz, show the additional view around the lock and mark the original map as retained/not currently scanned. Do not imply simultaneous wideband coverage.

### B. PHY packets

Default columns:

`Time | Frequency | SF / BW | Header | Payload state / CRC | CR | Length | Payload preview`

Expandable inspector groups:

- Decoder and explicit/laboratory mode.
- Normal/inverted IQ, preamble/SFD measures, sync observation and LDRO ambiguity.
- CFO, drift, offsets, uncertainty and FEC disagreements.
- Hex/text payload with nonprintable bytes escaped.
- LoRaWAN structure candidate, if eligible, retaining “MIC not verified”.
- Separate fingerprint measurements and their quality gates. Do not imply an older fingerprint alignment belongs to a particular decoded packet without an explicit association.

Provide filters for frequency, SF, BW, CRC and outcome. Retained summary counts must say “hypothesis rows,” not unique packets/devices. Keep SF5/6 detection-only and unsupported implicit-header modes visible in the scope drawer.

### C. Master emitters

Reuse the persistent table and move detailed RF history into the inspector. Display **Heuristic RF cluster** beside its ID, with last-seen information and retention limits. Remove the inaccurate claim that every reading is kept forever: entries retain approximately 1,000 readings with compaction slack to 1,100.

### D. Security evidence

Reuse current LoRa capture/integrity/coverage information. Keep “Attack rules not implemented” visible, not buried in a tooltip. Candidate LoRaWAN fields must not become authenticated identities or replay verdicts.

## 8. Drones — decoded broadcasts and their evidence

Use three clearly named subviews: **DJI live broadcasts**, **Wi-Fi Remote ID**, and **Offline IQ review (DJI)**. Keep the live receiver strip visible in every subview. Opening this page does not enable either decoder or change radio settings.

### Page structure

```text
Drones                         Live receiver: Wi-Fi 2.4 / channel …
[DJI live broadcasts] [Wi-Fi Remote ID] [Offline IQ review — DJI]
Decoder: enabled / off / waiting / failed       [existing enable control]
Coverage limitation / worker failure / retained-history notice
[Search retained sources] [Freshness filter] [Columns] [Details]
Source table                                  Selected-source inspector
                                              Reported identity + age
                                              Key telemetry + validity
                                              ▸ Detailed fields
                                              ▸ Provenance and raw evidence
▸ Processing diagnostics — queue state / drops / error badge
```

Use distinct labels for receiver state, decoder state and data age. A running worker without accepted broadcasts is not proof that the band is clear. Show a short summary of sampling limitations permanently; place full budgets in an expandable section. Never label retained source count as drones in the area.

### DJI live broadcasts

Default columns: `Reported serial | Reported model | Receive centre | Accepted observations | Last received / age | Data state`.

Selected-source summary shows reported serial/model, checksum status, source and freshness. Display aircraft, controller/app and home coordinates as separately labelled values; do not silently substitute one for another. Unknown/invalid positions remain unavailable. A geographic map, geocoding or track prediction is not part of this revamp; clear coordinates remain useful without new dependencies or services.

Inspector sections retain:

- **Identity and broadcast:** reported serial/model lookup, UUID, sequence, legacy/current format and available payload fields.
- **Position and motion:** aircraft/controller/home coordinates, raw altitude/height and existing conversions with reference caveats, raw velocities, angle, GPS time and validity annotations. Tentative units must remain tentative.
- **State:** raw state bits and tentative interpretations; never promote a bit to a verified flight state.
- **Reception and evidence:** CRC24A/CRC16 results, relative power, sample rate/centre, source-clock time, carrier-bin correction, run/session/capture/sample coordinates or offline path.
- **Raw packet:** full JSON with existing copy action, preserving original numeric precision and unknown fields.

Keep the checksum-versus-authentication qualification next to identity, not only in a help tooltip. Show stopped results as history. Explain that a new start resets them next to the existing enable control. Preserve all existing window/capture/omission/queue/worker counters; a compact warning badge remains visible when the diagnostics section is collapsed.

### Wi-Fi Remote ID

Default columns: `Reported ID | Aircraft category | Source MAC | Transport | Protocol version | Last received | Message freshness`.

This is a table of retained broadcast sources, not deduplicated aircraft. Beacon and NAN source records retain their current keys. Do not merge them by matching reported IDs or associate them automatically with DJI observations or Wi-Fi identities.

The selected-source summary contains the reported ID/category, aircraft location when available, transport/source and a persistent **Unauthenticated broadcast** label. Make position age visible beside position and identity age beside identity. A recently received Location must not make an older Basic ID look fresh. Distinguish acquisition-age estimates, decoder-arrival-only age and timestamps carried inside messages.

Use one expandable group per message family. Collapsed headers show present/missing, count where relevant, last update and stale/invalid status:

| Group | Essential display | Expandable complete detail |
|---|---|---|
| Basic ID — open by default | Reported identifier, ID type, aircraft category, age | All retained ID types, raw codes/bytes, protocol interpretation and provenance |
| Location | Position availability and age | Latitude/longitude, direction/course, speeds, barometric/geodetic altitude, relative height, status, height reference, accuracy codes/bounds and timestamps |
| System | Operator/system location availability | Operator position/type, area parameters, altitude, classification fields, version-specific fields and timestamp |
| Self ID | Description preview | Full escaped description, description type, raw bytes and age |
| Operator ID | Identifier availability | Full identifier/type, raw bytes and age; no inferred owner lookup |
| Authentication | Pages received, explicitly unverified | Page numbers, type, page count/legacy interpretation, lengths, timestamps and full bytes; no signature-valid badge |
| Provenance / frame | Channel, PHY, integrity state | Run/session/capture, sample span, centre/rate, host/device times and uncertainty, after-overflow state, independent FCS result and full MPDU hex |

Retain the complete source JSON copy action. Nulls/sentinels show **Not reported**, **Not available in this version**, or the actual available reason; never display them as zero or carry an old location forward. Receiver accuracy and transmitter-reported accuracy remain distinct.

Keep examined frames, accepted packs, not-decoded frames, queue drops, errors and out-of-order inputs individually accessible. Label ordinary not-decoded Beacons neutrally; that counter is not false-positive detection. Show limits of 64 sources and 32 slots/source, per-message stale threshold and eviction behavior in the retention drawer. Preserve existing five-minute inactivity pruning semantics; UI clocks must not delete records themselves.

### Offline DJI interval review

Use a compact input form for file, sample rate, receive centre and offset, with units and existing bounds beside each input. Keep Decode interval and current busy/error/result behavior. Results use the same DJI field presentation with a prominent **OFFLINE** badge and independent selection. A source path is not a live radio source. No file conversion, automatic metadata inference, full-file playback or new Remote ID replay is included.

## 9. Captures and offline investigation

### First delivery: existing functions

- Move or mirror Save next LoRa capture and Offline LoRa replay into this workspace.
- Keep shortcuts on the LoRa page so the existing workflow still takes few clicks.
- Display path, source, requested/actual available settings, sample count, duration, overflow, checksums and timing provenance.
- Show idle, queued save, saving, complete, failed and replay-running states using the backend information actually available. Do not invent an in-progress percentage.
- One replay result must never be appended to live identities, baselines, incidents or counts.

### Existing DJI offline review

Link to the same Drones → Offline IQ review workspace, preserving its current state. Do not create a second decoding session simply by opening a shortcut. Show path, explicit actual sample rate/centre, interval offset, limits, worker state and results. Keep the existing Decode interval action and read-only file handling. This is bounded interval decoding, not whole-file playback or Wi-Fi Remote ID replay.

Replay views carry an unmissable **OFFLINE** or **SYNTHETIC** badge and file provenance. Live acquisition status remains visible separately. No offline result enters live identities, baselines, incidents or counts.

### Explicitly outside this revamp

Wi-Fi security-event replay adapters, Wi-Fi saved-IQ replay, cyclostationary replay adapters, capture indexing, evidence-bundle export and new cancellation mechanisms are separate functionality projects. CLI availability does not establish GUI availability. Do not add working-looking buttons for these features. The capability drawer may list them as unavailable. Do not scan dataset directories, change dataset originals or create a new acquisition/file-processing worker for layout purposes.

## 10. Settings and capabilities

Separate settings into:

1. **Acquisition:** existing device, active band, channel/lock, gain/AGC, threshold and duration controls.
2. **Recording:** current destination, configured budget, rotation and whether a change waits for reconnect.
3. **Analysis:** independent existing cyclostationary, DJI and Wi-Fi Remote ID enable states and supported scope; no controls for nonexistent models. Explain stop/start retention and reset behavior next to each control.
4. **Display:** font scaling, table columns, follow-live defaults, density and inspector width.
5. **Capabilities:** build flags, executable/worker protocol version, decoder coverage and validation evidence.

Keep requested and active settings distinct. Mark commands as pending until the backend reports the applied state. A UI setting must not silently restart scanning.

The current X310 software profile still caps requests at 20 MSps. Display that accurately, together with a note that higher-rate qualification is separate. Do not add a 200 MSps slider merely because the system now has 10 GbE. A configurable receiver profile and its tests are a separate backend change, not part of the presentation-only milestone.

For basic receiver ranges use the current profile and, where exposed, actual hardware metadata. If an actual value is unavailable, show “requested” or “not reported”; never echo a requested value as measured.

## 11. Visual and interaction rules

### Information hierarchy: complete information, calm presentation

Use four presentation levels consistently. A collapsed section changes visibility only, never processing, retention or data meaning.

| Level | Placement | Content | Default |
|---|---|---|---|
| 1 — Operational context | Persistent shell and page header | Active acquisition, source, decoder state, freshness, material faults and coverage caveats | Always visible |
| 2 — Main task | Map/table and selected-record summary | Identity or observation label, key measurements, finding, integrity and important validity limits | Visible |
| 3 — Detailed explanation | Named expandable sections | Complete protocol fields, measurement quality, history already retained, thresholds and capability limits | Primary group open; secondary groups collapsed |
| 4 — Exact evidence | Raw/provenance/diagnostic sections | Full bytes/JSON, exact counters, source coordinates and worker details | Collapsed, directly reachable and copyable where currently supported |

“Minor” means secondary to the current task, not disposable. Required fields remain available even when empty, with an explicit unavailable state. Do not omit a field merely because a test fixture did not populate it. Preserve previously unknown/raw JSON keys through the complete evidence view.

Rules for collapsible sections:

- Headers use meaningful names and summaries, such as **Location · stale · last received 18 s ago**, not just “Advanced”.
- Worker failures, invalid selected positions, lost coverage, stale summaries and active laboratory mode stay visible above collapsed details.
- Prefer one level of sections, at most two for message pages/raw data. Avoid a chain of nested accordions.
- Provide **Expand details** / **Collapse details** for the selected inspector. Preserve group choices per page/domain during the session; do not carry one source's values into another.
- Keep essential values within one selection; secondary values within one additional expansion. Raw bytes may require a second expansion, with a visible route.
- A local **Find field** filter can search existing inspector labels/values and reveal matching groups. It must say it searches the selected retained record, not disk history or the RF band.
- Use independent table/inspector scrolling. Opening a large raw payload must not push the main task offscreen.

### Modern native visual system

Aim for a restrained instrument workspace: clear typography, quiet surfaces and strong evidence hierarchy. Avoid decorative gauges, gradients, animated radar sweeps or charts with no underlying data.

| Token | Proposed design |
|---|---|
| Background | Charcoal/slate base, slightly raised panels, subtle separators; avoid a box around every value |
| Accent | One blue/teal accent for selection and primary actions; semantic status colours remain independent |
| Typography | Approximately 15–16 logical px body, 20–24 page title, 13–14 secondary labels; readable monospace for raw values |
| Spacing | 4/8/12/16/24 logical px scale; 16–24 page margins; consistent field and section gaps |
| Density | Comfortable default rows around 30–34 logical px; optional compact table density without shrinking warnings or inputs |
| Controls | Labelled buttons, consistent heights, generous click targets, visible keyboard focus; icons supplement text |
| Numeric fields | Right alignment in tables; unit in header/label; exact values available without display rounding |
| Colour | Neutral for observations; amber for uncertainty/degradation; red for faults/severity as appropriate; never colour alone |

Measure text/background contrast, aiming for at least 4.5:1 for normal text and 3:1 for large text and control boundaries. Do not claim native accessibility compliance without testing the actual ImGui stack. Keep all essential operation keyboard reachable, with focus order following the visual order. No hover-only essential information.

Use a compact breadcrumb/title, one-line purpose and consistent action area per page. Keep destructive/current reset actions away from navigation and apply only their existing scope. Search/filter controls belong above their table. Active filters stay visible with **Clear filters**, so an empty filtered table is not mistaken for no observations.

At narrow widths, stack summary fields and replace the centre with the selected inspector using Back; do not squeeze columns into illegible fragments. Keep optional horizontal scrolling for exact wide tables and preserve full values through details. The top bar may wrap into two rows without hiding active acquisition or failure state. UI density and font scaling are display-only preferences.

### Information-preservation inventory

Before implementation, create a field/control ledger from each current drawing function and snapshot. This ledger is the acceptance authority, not the proposed default column lists. They are intentionally short; the inspectors must remain complete.

Each ledger row records: existing screen/function, source field or callback, meaning/unit, current null/error behavior, new default location, expandable destination, raw representation, freshness/source scope, and verification fixture. Record dynamic/unknown JSON keys as a preserved raw-field group. Include tooltips, warnings, hidden-by-build controls and offline result fields.

| Domain | Must preserve | Preferred destination |
|---|---|---|
| Receiver | Device/address, band/channel/lock, gain/AGC, rate, threshold, duration, connection, recording and all status/errors | Persistent essentials; Monitoring setup and Settings for complete controls |
| RF observations | Maps, centres/bandwidth/power, estimates, ages, cycles, grouping and all registry fields | Main observations table plus measurement/retention inspector |
| Wi-Fi packets | All PHY/MAC/identity/decode/FCS/measurement fields and currently retained bytes | Packet table plus protocol, quality and provenance groups |
| Identities | BSSID/cluster distinctions, SSID, WPS manufacturer/model, IEEE vendor, advertised security/PMF/HT/VHT/HE, channels, counts, times and RF history/gates | Identity inspector; no metadata removed to simplify columns |
| Security | All five views, rule IDs, thresholds, confidence/severity, sequences, counters, gaps, baseline state/actions and retained evidence | Incident-first workspace with complete supporting views |
| LoRa | Every RF/PHY hypothesis, SF/BW/CR/length, polarity/LDRO/FEC/CFO/drift/sync, payload, LoRaWAN candidate, history and integrity field | PHY/master/evidence subviews and inspector |
| Cyclostationary | Every measurement, candidate qualification, window/source coordinate, budget, gate, timing and worker diagnostic | Evidence-first display with expandable complete diagnostic groups |
| Drones | Every DJI field/raw payload and Remote ID message/raw frame, timestamps, nulls, counters, independent enables and offline controls | Separate source-specific subviews and full inspectors |
| Files and capabilities | Existing save/replay input/output/error states, retention, build/worker availability and all support limits | Captures, relevant page shortcuts and capability drawer |

No existing value or action is removed because it does not fit a card. If its source exposes no stable identifier, retain the existing detail workflow instead of inventing an exact cross-reference. Aggregate cards may format existing facts but must not add scientific inference.

### Core operator journeys

1. **Start monitoring:** choose existing receiver/settings, explicitly connect, read actual acquisition/health, then inspect any page without retuning. Keep startup defaults unchanged.
2. **Investigate an alert:** open incident → read finding and coverage → inspect sequence/thresholds → expand exact evidence. Preserve exact lookup only where already supported.
3. **Inspect a broadcast:** Drones → explicitly enable the selected existing decoder → select source → read identity/position and their ages → expand message/provenance → use existing JSON copy.
4. **Inspect a waveform:** Wi-Fi → Experimental link analysis → read sampled-window scope → examine measurements/candidates → inspect gates and raw diagnostics. Compatibility never becomes confirmed drone identity.
5. **Review offline data:** choose the existing LoRa or DJI workflow → provide its existing inputs → run its existing action → inspect clearly isolated offline results. Returning to Live leaves acquisition untouched.

### Evidence/status vocabulary

| Status | Meaning | Visual treatment |
|---|---|---|
| Observed | Measured RF or parsed observation | Neutral blue/grey with explicit type |
| Integrity checked | FCS/CRC passed | Check icon plus text; never “authenticated” |
| Suspected incident | A detector's stated conditions matched | Amber label; severity displayed separately |
| Experimental | Development policy or unvalidated compatibility | Persistent outlined badge |
| Unsupported / unavailable | Feature not implemented or required evidence absent | Grey text with reason |
| Stale / degraded | Old result or compromised observation coverage | Age / warning text and appropriate icon |
| Receiver/storage failure | Acquisition or persistence problem | Red warning identifying the affected function |

Do not reserve red exclusively for “confirmed attack”: this application does not establish that conclusion. Do not use colour alone, confidence percentages without calibration, or a single combined security/drone certainty score.

### Tables and inspectors

- Use sortable/resizable tables with a small useful default column set and optional columns.
- Freeze identifying columns where supported; keep advanced values in the inspector.
- Preserve selected record keys through refresh and sorting. If a selected row is evicted, show “No longer retained”; never select the row that inherited its index.
- Show truncated text with inspect/copy access to the full escaped value.
- Distinguish relative power, dBc, Hz, ppm, samples and seconds in labels.
- Separate device-clock timestamps from host wall time. Display wall time with timezone; avoid precision finer than the source supports.
- Keyboard support: Ctrl+F for the current retained-table filter; Escape closes inspector/modal; Enter inspects a selected row where practical.
- Disable unavailable actions with an explanation, not a silent no-op.

### Empty and degraded states

Design explicit states for disconnected, connecting, connected/no samples, healthy/no detections, all candidates rejected, no eligible rule inputs, analysis disabled, worker busy/dropped, stale result, malformed replay, recording failure and missing historical evidence.

Show one primary next action where appropriate: Connect, select a supported channel, inspect capture health, enable analysis or choose a replay file. Do not prescribe a gain increase without accounting for clipping and receiver conditions.

## 12. C++/Dear ImGui implementation approach

### Reuse first

| Existing implementation | Proposed use |
|---|---|
| `src/main.cpp` | Retain platform/render lifecycle; progressively extract shell and navigation |
| `src/scanner.hpp` | Existing command setters, status and snapshot access; no GUI-owned radio |
| `src/wifi_gui.*` | Packet/identity tables and their existing details |
| `src/lora_gui.*` | PHY, security-evidence and replay widgets |
| `src/security_gui.*` | Security subtabs, baseline commands and incident evidence |
| `src/cyclostationary/panel.*` | Experimental measurement display and worker diagnostics |
| `src/drones/panel.*` | DJI live/offline and Wi-Fi Remote ID views, evidence JSON and worker state |
| `tests/test_wifi_gui.cpp`, LoRa/panel tests | Offscreen rendering and interaction regression harnesses |

Proposed small GUI-only modules under `src/gui/`:

- `app_shell`: navigation, top bar, footer and responsive layout.
- `ui_state`: selected page/band/record, filters, follow-live and display preferences.
- `overview_page`, `wifi_page`, `lora_page`, `drones_page`, `security_page`, `captures_page`, `settings_page`.
- `inspectors`: shared evidence/provenance/limitation presentation.
- `capability_catalog`: central human-readable support and validation descriptions.

These are proposed file names, not files created by this plan. Existing draw functions can remain in place and be called by wrappers first. Avoid a large one-shot move or rename of decoder/scanner modules.

### Rendering/data ownership

- Acquisition and analysis threads continue owning backend state.
- Capture each required published snapshot once per UI update and reuse it throughout that frame.
- Do not read files, run decoders, copy entire IQ buffers, sort unbounded histories or wait for workers during rendering.
- Existing immutable security/DSP snapshots remain immutable. Store UI selections and filters separately.
- Cache filtered/sorted row indices until the snapshot or filter changes. If revision counters are not available, use a modest timed refresh initially rather than hashing every field every frame.
- Use `ImGuiListClipper` for fixed-height large tables and bounded inspector excerpts. Wrap long detail text outside the virtualised row list.
- Limit expensive table refreshes to a proposed 4–10 Hz while allowing responsive navigation/rendering. This is a UI target to measure, not a change to receiver timing or a claim of achieved performance.
- Software rendering must remain usable; the current desktop required Mesa software rendering to launch the application.

### Preferences

Initially retain UI preferences only in memory. A separately approved optional GUI-only versioned preferences file could store column layouts, scale and navigation preferences with atomic writes and recovery from invalid files. It must not replace acquisition/security persistence or automatically reconnect/start analysis at launch.

## 13. Existing contracts and excluded functionality

| Need | Available now | Additional work before enabling |
|---|---|---|
| View packets, identities, coverage and incidents | Existing tables/snapshots | GUI composition only |
| Independent navigation versus acquisition | Existing band/channel setters | GUI-local viewed-band state and explicit monitoring actions |
| Exact security evidence references | Incident evidence has ingest keys | Bounded lookup over retained frames/recordings; handle eviction/missing files |
| Exact normal packet → incident navigation | Ordinary packet rows lack full correlation identifiers | Optional run/session/capture/sample/PHY keys; additive contract and tests |
| Five-minute traffic/coverage charts | Primarily counters and bounded retained rows | Timestamped bounded history with missing-interval representation |
| Historical full-packet search | Rotating event files; limited live recent rows | Explicit bounded file indexing/query worker; never claim full history from the UI tail |
| Actual hardware settings everywhere | Some capture metadata, not a uniform GUI contract | Read-only applied-setting fields; unavailable until supplied |
| Incident review notes/acknowledgements | Not implemented | Separate annotation store; no change to detector incident state |
| Export evidence bundle | Underlying records exist in some cases | Availability checks, bounded export and provenance manifest |
| DJI / Wi-Fi Remote ID broadcast identity | Existing reported identifiers, DJI model lookup and Remote ID aircraft categories | GUI composition only; claims remain unauthenticated, not exact-model ML |
| Validated RF named-link identity, encrypted replay, LoRa attacks | No validated/implemented capability | Separate backend work and validation; cannot be solved by GUI design |

**Scope rule:** additional-work cells above are exclusions, not implementation milestones. Where an existing contract lacks a value or lookup, show unavailable or retain its current workflow. Do not extend scanner snapshots, schemas, workers or stores to fulfil a visual design.

Use stable keys: incidents by source/run namespace plus incident ID; frames by their ingest key; identities by store key; DSP selections by epoch/capture/tile; replay rows by replay instance plus retained-row identity. DJI rows retain their existing source/observation identity; Remote ID sources use run/session/transport/transmitter keys and message type/page keys. Device timestamps from different sessions must not share a continuous timeline automatically.

## 14. Capability facts the design must preserve

These are current application limits to seed the capability drawer. Read configurable limits from the runtime when possible; maintain a versioned description for static support. Recheck them when backend code changes.

| Area | Current scope |
|---|---|
| Monitoring | One selected band/radio path at a time |
| Wi-Fi 2.4 GHz | Channels 1–13; four one-second captures per step; 52 seconds capture time per sweep before overhead |
| Wi-Fi 5 GHz | Channels 36/40/44/48/149/153/157/161/165; 36 seconds capture time per sweep before overhead; no DFS/6 GHz scan plan |
| Wi-Fi PHY | 1 Mbps DSSS and legacy 20 MHz OFDM 6/9/12/18/24/36/48/54 Mbps; no HT/VHT/HE/EHT payload decoding |
| Wi-Fi observation limits | 1,000 packet rows; normal 400-burst and extended 4,000-burst security budgets per capture |
| LoRa | Explicit PHY SF7–12, BW125/250/500, CR4/5–4/8; SF5/6 detection-only; 200 retained hypothesis rows |
| LoRa capture | 500 kS/s packet-listen request; 1–30 seconds, default six; file cap 32 million complex samples |
| Identity | BSSID is a claimed network/interface; RF clusters are provisional; approximately 1,000 readings retained per persistent entry |
| Wi-Fi security | Flood/beacon hardware evidence exists for a limited conducted OFDM case; connection/EAPOL rules remain experimentally software-tested |
| Security retention | Live recent snapshot 256 frames; internal recent buffer 4,096; 1,000 incident store/200 published; default 64 MiB rotating event recording |
| Cyclostationary | Opt-in; four independent windows of up to 65,536 samples, total 262,144; two-second worker deadline; no validated named family |
| LoRaWAN | Structural candidates only; no MIC validation, decryption or replay detection |
| DJI live | Independent opt-in; four distributed 20 ms windows/capture, at most 2 million complex samples/window; queue 4; 128 retained observation rows; checksum-checked reported identity, not authenticated identity |
| DJI offline | Raw little-endian cf32, explicit rate/centre/offset; at most 40 ms or 2 million samples per request; separate result state |
| Wi-Fi Remote ID | FCS-valid Beacon vendor IE / supported NAN follow-up; versions 0–2; Basic ID, Location, Authentication, Self ID, System, Operator ID; no Bluetooth or signature verification |
| Remote ID retention | Queue 128 frames; 64 sources, 32 message slots/source; per-message stale after 10 seconds; five-minute inactivity pruned on a subsequent supported pack; stop retains history, re-enable resets |
| Drone validation | Recorded/reference-vector and software tests exist; fresh integrated live reception and sustained-load qualification remain pending |

Correct stale text during the GUI migration: LoRa hardware interoperability has evidence for tested setups; LoRa RF history is not “every reading forever”; the old claim that all 1 Mbps DSSS reception is blocked at 20 MSps is contradicted by recordings. Refer to actual validation reports rather than preserving stale comments as product text.

## 15. Delivery milestones

Each milestone is a UI-only change, reviewable independently. Do not implement this plan as one large replacement of the current application.

| Milestone | Deliverable | Acceptance gate |
|---|---|---|
| G0 — Information and action inventory | Enumerate every rendered field, tooltip, raw-data view, control and empty/error state; map old location to new destination; capture baseline screenshots | No unmapped information or command; supported/unsupported distinctions verified against current code |
| G1 — Visual foundation and shell | Spacing/type/colour tokens, reusable status/field/section components, responsive navigation, persistent acquisition strip | Clear at 1280 × 800 and 200% scale; navigation sends zero backend commands; settings unchanged |
| G2 — Security workspace | Existing five views, readable incident evidence, coverage strip and baseline controls | All rule evidence and exact counters retained; current freeze/reset scope and callbacks unchanged |
| G3 — Wi-Fi and LoRa | Primary columns, optional columns, detail groups, existing maps, stable selection and retained-row search | Field/action inventory complete for both bands and LoRa; source, gates, retention and lab-mode labels accurate |
| G4 — Drones | DJI live, Wi-Fi Remote ID and existing offline interval view with separate source tables and message inspectors | Every normalized/raw field accessible; no source merging; individual message freshness/nulls preserved; controls remain independent |
| G5 — DSP, captures and settings | Existing cyclostationary panel organized by evidence; existing save/replay controls and capability drawer | No new scientific conclusions, replay types, indexing or exports; offline/live separation preserved |
| G6 — Visual and functional parity | Busy/empty/error fixtures, long-text/DPI/keyboard checks, software-render measurement and command/output parity checks | All inventory rows signed off; no new backend behavior, automatic action or unexplained output change |
| G7 — Completion and cleanup | Final screenshots, operator guide, UI-only rollback and removal of temporary duplication after approval of parity | One consistent interface; every existing function still reachable; no changes to saved data formats or processing |

G0 and G1 establish the shared rules. Implement Security, Wi-Fi/LoRa and Drones against those same components, then finish capture/settings consistency. Keep a temporary legacy layout option while verifying parity; both layouts must call the same existing backend paths. Do not automatically reconnect, restart analysis or reset observations when switching layouts.

Backend enhancements in the contracts table are deliberately absent from these milestones. Completing the revamp does not depend on implementing them or on fresh drone transmissions.

## 16. Verification checklist and definition of done

The redesign is complete when:

- Every current control, information field, tooltip, raw value and error has an accessible destination, verified against the inventory. No field disappears because it is rare, advanced or unavailable in a fixture.
- Navigation, filtering, sorting and inspection never connect, retune, transmit, reset baselines or toggle analysis as side effects.
- Snapshot replay reproduces the same detector/identity values; presentation does not recalculate scientific conclusions.
- Tests cover disconnected, healthy, stalled, overflow, cap, queue loss, recording error, restored incident, malformed replay, stale DSP and unavailable evidence.
- All collapsed sections preserve important warning badges; message-level freshness, invalid/null positions and incomplete evidence remain clear. Find-field and expand/collapse actions never modify backend state.
- Drones tests cover independent decoder enables, source/session separation, stopped history, per-message stale/null fields and offline isolation; no broadcast count becomes a physical-aircraft count.
- Offscreen GUI tests exercise actual selection/inspector actions using existing C++ harnesses. Synthetic fixtures are visibly labelled; screenshots are visually reviewed.
- Tables remain usable at 1280 × 800 and higher DPI, with scrolling and large counters/text. Selection remains stable during live refresh and handles eviction explicitly.
- Evidence values preserve integer precision, units, source clocks and provenance; unavailable values remain distinct from zero.
- Software rendering is measured with the busiest retained tables and incident inspector. UI overhead is compared on the same replay/hardware setup; no guaranteed frame rate or live duty is asserted before measurement.
- The running monitoring setup can be disabled/rolled back to the legacy layout without altering saved identities, baselines or incident records.
- The capability drawer distinguishes implemented behavior, limited hardware evidence, experimental rules and pending work.

Do not require fresh transmissions to approve a layout. Use existing saved recordings and synthetic GUI fixtures first; receiver-performance qualification can be coordinated separately.

## 17. Reference files

- `src/main.cpp`, `src/config.hpp`, `src/scanner.hpp`
- `src/wifi_gui.*`, `src/lora_gui.*`, `src/security_gui.*`
- `src/cyclostationary/panel.*`, `src/drones/panel.*`, `src/drones/remote_id.*`
- `docs/DJI_DRONEID_INTEGRATION.md`, `docs/WIFI_OPENDRONEID_INTEGRATION.md`
- `docs/WIFI_IDENTITIES.md`
- `docs/LORA_RECEIVER_UPGRADE.md`, `docs/LORA_SX1262_VALIDATION.md`
- `docs/HACKRF_WIFI_VALIDATION_2026-10-02.md`
- `docs/WIFI_SECURITY_VALIDATION_2026-10-11.md`
- `docs/WIFI_TRANSACTION_REPLAY_ANOMALIES.md`
- `docs/WIFI_NATIVE_MARKER_INTEGRATION_REPORT.md`

This plan changes presentation and operator workflow incrementally. It does not declare pending receiver, protocol, security or drone-link validation complete.
