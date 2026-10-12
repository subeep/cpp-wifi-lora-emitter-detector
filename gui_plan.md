# RF Monitor — C++ GUI implementation plan

Date: 12 October 2026  
Status: proposed GUI design; no application behavior changed  
Target: the existing C++17, Dear ImGui, GLFW and OpenGL application

## 1. Objective and boundaries

Make the application easy to operate during monitoring, useful for investigating an incident, and honest about what the receiver actually observed. Put attack detection first. Keep LoRa decoding, Wi-Fi identities and experimental link analysis accessible without forcing the operator to read every diagnostic at once.

This is an incremental redesign of the current native application. Reuse its scanner, snapshots, decoders, security rules, persistent stores and drawing functions. Do not introduce a browser frontend, replace Dear ImGui, or require a new DSP architecture.

The first delivery must work with the current receiver and data contracts. Features requiring new metadata, history, indexing or backend behavior are explicitly marked below. A polished display must not imply that an unfinished detector exists.

Non-negotiable behavior:

- Viewing another page must not silently change the radio, band, channel, gain or active recording.
- Show the active acquisition band and channel even when inspecting another band's saved information.
- Distinguish measured waveforms, FCS/CRC-checked protocol observations, claimed identities, suspected incidents and independently validated conclusions.
- Keep missing measurements unavailable; never turn missing data into zero, quiet traffic or a benign verdict.
- Keep live, replay and synthetic-test data separate in both state and presentation.
- No automatic transmitter operation, mitigation, ML model, Remote ID decoder or new named-link acceptance is introduced by the GUI work.
- Preserve existing identity files, event schemas and incident/baseline persistence during the initial migration.

## 2. Recommended application layout

Use one fixed native workspace with a persistent top bar, a narrow navigation rail, a central content area, an optional right inspector and a compact footer. Start without docking; optional docking can come later if the repository's ImGui version supports it and operators need it.

```text
┌ RF Monitor ─ LIVE / REPLAY ─ Device ─ Active band / channel ─ Health ─┐
│ Overview       │ Page title, data source, freshness, filters        │
│ Wi-Fi 2.4 GHz  │                                                   │
│ Wi-Fi 5 GHz    │ Main table / frequency map / selected workflow     │
│ LoRa          │                                   │ Inspector     │
│ Security      │                                   │ Details       │
│ Captures      │                                   │ Evidence      │
│ Settings      │                                   │ Limitations   │
├ Connection │ capture health │ recording │ queue loss │ last update ┤
└────────────────────────────────────────────────────────────────────┘
```

### Persistent top bar

Keep these visible on every page:

1. **Source:** Live receiver, Replay file, or Synthetic fixture.
2. **Radio:** B210/X310, connection state, Connect/Reconnect/Disconnect.
3. **Active acquisition:** band, sweep/fixed mode, selected channel or LoRa lock frequency.
4. **Receiver health:** receiving, processing, stalled, degraded, disconnected or starting.
5. **Open incidents:** count labelled by its scope, with a shortcut to Security.
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
| Captures | Save/replay supported recordings and inspect their provenance | Existing LoRa GUI functions first; other replay types added in a later milestone |
| Settings | Configure acquisition, display preferences and inspect capability/support limits | Reuse existing setters; no arbitrary hardware controls |

No separate “Drones detected” page is justified yet. Keep an **Experimental link analysis** tab under each Wi-Fi page. A dedicated drone inventory should require validated backend identification first.

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

The ordinary `WifiPacketRow` does not contain all security event bytes or an exact capture/frame key. Do not correlate it to a security event by display time, BSSID or approximate frequency. Initially offer a clearly labelled address/time filter. Exact cross-navigation requires the optional correlation fields in section 12.

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
- **Export evidence bundle:** later milestone, to a user-selected local destination, containing only available records and explicit missing-evidence notes.

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

## 8. Captures and offline investigation

### First delivery: existing functions

- Move or mirror Save next LoRa capture and Offline LoRa replay into this workspace.
- Keep shortcuts on the LoRa page so the existing workflow still takes few clicks.
- Display path, source, requested/actual available settings, sample count, duration, overflow, checksums and timing provenance.
- Show idle, queued save, saving, complete, failed and replay-running states using the backend information actually available. Do not invent an in-progress percentage.
- One replay result must never be appended to live identities, baselines, incidents or counts.

### Later native GUI adapters

Add Wi-Fi security-event replay, Wi-Fi saved-IQ replay and cyclostationary replay only after introducing bounded asynchronous adapters around their existing C++ APIs/tools. Their CLI availability is not GUI availability.

For each adapter: validate input metadata and limits, run away from the render thread, preserve error/progress state and isolate its result model. Do not assemble shell commands from filenames; prefer library APIs, or explicit process argument vectors when using the isolated helper. Cancellation must be cooperative or process-isolated; do not terminate arbitrary in-process decoder threads.

Replay tabs carry an unmissable **REPLAY** or **SYNTHETIC** badge and file provenance. Live acquisition status remains visible separately if a receiver is running.

A searchable capture catalogue and evidence-bundle export require new bounded indexing/export code. Start with explicit paths and recent-file metadata; do not scan large dataset directories on every render or recursively index the SDA datasets by default. Reading datasets must not modify their originals.

## 9. Settings and capabilities

Separate settings into:

1. **Acquisition:** existing device, active band, channel/lock, gain/AGC, threshold and duration controls.
2. **Recording:** current destination, configured budget, rotation and whether a change waits for reconnect.
3. **Analysis:** optional cyclostationary enable state and supported scope; no controls for nonexistent models.
4. **Display:** font scaling, table columns, follow-live defaults, density and inspector width.
5. **Capabilities:** build flags, executable/worker protocol version, decoder coverage and validation evidence.

Keep requested and active settings distinct. Mark commands as pending until the backend reports the applied state. A UI setting must not silently restart scanning.

The current X310 software profile still caps requests at 20 MSps. Display that accurately, together with a note that higher-rate qualification is separate. Do not add a 200 MSps slider merely because the system now has 10 GbE. A configurable receiver profile and its tests are a separate backend change, not part of the presentation-only milestone.

For basic receiver ranges use the current profile and, where exposed, actual hardware metadata. If an actual value is unavailable, show “requested” or “not reported”; never echo a requested value as measured.

## 10. Visual and interaction rules

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

## 11. C++/Dear ImGui implementation approach

### Reuse first

| Existing implementation | Proposed use |
|---|---|
| `src/main.cpp` | Retain platform/render lifecycle; progressively extract shell and navigation |
| `src/scanner.hpp` | Existing command setters, status and snapshot access; no GUI-owned radio |
| `src/wifi_gui.*` | Packet/identity tables and their existing details |
| `src/lora_gui.*` | PHY, security-evidence and replay widgets |
| `src/security_gui.*` | Security subtabs, baseline commands and incident evidence |
| `src/cyclostationary/panel.*` | Experimental measurement display and worker diagnostics |
| `tests/test_wifi_gui.cpp`, LoRa/panel tests | Offscreen rendering and interaction regression harnesses |

Proposed small GUI-only modules under `src/gui/`:

- `app_shell`: navigation, top bar, footer and responsive layout.
- `ui_state`: selected page/band/record, filters, follow-live and display preferences.
- `overview_page`, `wifi_page`, `lora_page`, `security_page`, `captures_page`, `settings_page`.
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

Initially retain UI preferences only in memory. A later optional GUI-only versioned preferences file can store column layouts, scale and navigation preferences with atomic writes and recovery from invalid files. It must not replace acquisition/security persistence or automatically reconnect/start analysis at launch.

## 12. Data contracts and features requiring extra work

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
| Named drone identity, encrypted replay, LoRa attacks | No validated/implemented capability | Backend work and validation; cannot be solved by GUI design |

Use stable keys: incidents by source/run namespace plus incident ID; frames by their ingest key; identities by store key; DSP selections by epoch/capture/tile; replay rows by replay instance plus retained-row identity. Device timestamps from different sessions must not share a continuous timeline automatically.

## 13. Capability facts the design must preserve

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

Correct stale text during the GUI migration: LoRa hardware interoperability has evidence for tested setups; LoRa RF history is not “every reading forever”; the old claim that all 1 Mbps DSSS reception is blocked at 20 MSps is contradicted by recordings. Refer to actual validation reports rather than preserving stale comments as product text.

## 14. Delivery milestones

| Milestone | Work | Acceptance gate |
|---|---|---|
| G0 — Inventory and fixtures | Map every current control/field to a destination; collect synthetic and recorded empty/healthy/degraded/incident snapshots | No current field or control silently omitted; source labels and support facts agreed in the plan |
| G1 — Shell and safety of navigation | Add rail, persistent acquisition/health bar and GUI-local viewed-band state; wrap existing views | Navigation causes zero scanner-setting commands; only explicit monitoring actions retune/reconnect |
| G2 — Security workspace | Incident-first layout, filters, evidence inspector, coverage strip and existing baseline controls | Existing flood/beacon and new transaction fixtures render with correct evidence/limits; baseline actions preserve scope |
| G3 — Wi-Fi and LoRa information layout | Default column sets, advanced inspectors, retained-row filters and identity/cluster distinctions | All existing values remain reachable; selections survive updates; source/retention labels correct |
| G4 — Experimental DSP and capture workflows | Integrate measured/unknown/unsupported states; move existing LoRa save/replay workflow | No named-identity promotion; offline results never alter live state; disabled/busy/stale/error cases usable |
| G5 — Optional investigation adapters | Add provenance keys, bounded history/indexing, extra replay types and export only where justified | New contracts tested independently; each function has limits/cancellation/error handling and no unbounded UI work |
| G6 — Qualification and rollout | Resolution/DPI/keyboard checks, recorded replays, software-render performance and receiver-equivalence review | No lost feature, unexplained detector change or accidental radio command; measured rendering overhead documented |

Implement G0–G2 first because attack detection is the active priority. G3–G4 complete the practical UI redesign using existing data. G5 is separately scoped enhancement work, not a prerequisite for a useful first release.

Keep a temporary legacy layout switch until the new pages pass their acceptance checks. Removing the switch is a final cleanup step, not an excuse to maintain two different detection paths.

## 15. Verification checklist and definition of done

The redesign is complete when:

- Every current main-view control and information field has an accessible destination.
- Navigation, filtering, sorting and inspection never connect, retune, transmit, reset baselines or toggle analysis as side effects.
- Snapshot replay reproduces the same detector/identity values; presentation does not recalculate scientific conclusions.
- Tests cover disconnected, healthy, stalled, overflow, cap, queue loss, recording error, restored incident, malformed replay, stale DSP and unavailable evidence.
- Offscreen GUI tests exercise actual selection/inspector actions using existing C++ harnesses. Synthetic fixtures are visibly labelled; screenshots are visually reviewed.
- Tables remain usable at 1280 × 800 and higher DPI, with scrolling and large counters/text. Selection remains stable during live refresh and handles eviction explicitly.
- Evidence values preserve integer precision, units, source clocks and provenance; unavailable values remain distinct from zero.
- Software rendering is measured with the busiest retained tables and incident inspector. UI overhead is compared on the same replay/hardware setup; no guaranteed frame rate or live duty is asserted before measurement.
- The running monitoring setup can be disabled/rolled back to the legacy layout without altering saved identities, baselines or incident records.
- The capability drawer distinguishes implemented behavior, limited hardware evidence, experimental rules and pending work.

Do not require fresh transmissions to approve a layout. Use existing saved recordings and synthetic GUI fixtures first; receiver-performance qualification can be coordinated separately.

## 16. Reference files

- `src/main.cpp`, `src/config.hpp`, `src/scanner.hpp`
- `src/wifi_gui.*`, `src/lora_gui.*`, `src/security_gui.*`
- `src/cyclostationary/panel.*`
- `docs/WIFI_IDENTITIES.md`
- `docs/LORA_RECEIVER_UPGRADE.md`, `docs/LORA_SX1262_VALIDATION.md`
- `docs/HACKRF_WIFI_VALIDATION_2026-10-02.md`
- `docs/WIFI_SECURITY_VALIDATION_2026-10-11.md`
- `docs/WIFI_TRANSACTION_REPLAY_ANOMALIES.md`
- `docs/WIFI_NATIVE_MARKER_INTEGRATION_REPORT.md`

This plan changes presentation and operator workflow incrementally. It does not declare pending receiver, protocol, security or drone-link validation complete.
