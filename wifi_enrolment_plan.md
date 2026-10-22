# Wi-Fi enrolment and persistent master-list plan

Date: 15 October 2026  
Status: proposed; implementation awaits user approval  
Scope: Wi-Fi 2.4 GHz and the application's supported 5 GHz channels only  
Platform: existing C++17, Dear ImGui, GLFW/OpenGL application

## 1. Intended result

Maintain one persistent Wi-Fi inventory whenever the application is receiving and decoding supported traffic. Each entry brings together its observed digital identity, RF measurements, history, operator approval and security context. A newly observed identity appears in red with **NEW DEVICE — UNREVIEWED**. Existing identities update without losing their original enrolment reference or previous changes.

Automatic discovery and operator enrolment are separate. Saving a previously unknown transmitter does not approve it. A device seen yesterday can still be unapproved today. An approved identity can still raise a change or attack warning.

The inventory is an evidence-based defence aid. It reports new observed identities and suspicious changes; it cannot guarantee that every physical device nearby is detected, establish ownership from a beacon, or stop an intruder by itself. No active countermeasures are included.

## 2. What already exists, and what we should build on

This review uses the current source and the GUI implementation delivered on 15 October, rather than treating the earlier GUI proposal as unimplemented. Existing unrelated working-tree changes are outside this plan.

| Area | Current implementation | Enrolment upgrade |
|---|---|---|
| Persistent identity store | `WifiMasterList`, loaded at startup from `data/wifi_master/*.ndjson` | Extend this store; do not create a competing list of identities |
| Identity discovery | Decoded FCS-valid beacons/probe responses in the scanner's identity path, independent of RF-quality acceptance | Retain that behavior and add discovery/review state and provenance |
| Digital details | BSSID, SSID, advertised channel/security/ciphers/PMF/capabilities, beacon interval, WPS hints, vendor assignment, timestamps/counts | Add field history, role/address provenance, approval and operator labels |
| Name retention | Last-known SSID/WPS details survive later omissions, with separate source/time | Preserve this distinction; an omitted field is not a change to an empty value |
| RF readings | CFO, image/IQ/DC estimates and quality measurements for accepted extractions | Add validity, context, robust summaries, reference versions and controlled adaptation |
| RF retention | About 1,000 readings per record, lazy compaction after 1,100; identity snapshots also compact | Keep bounded raw history and add longer-lived summaries and change records |
| Anonymous RF matching | Provisional `WIFI-FP-*` clusters; recent 20-reading medians and placeholder tolerances; CFO excluded | Keep separate from enrolled identities; never use this matcher to auto-approve devices |
| Current GUI | Native shell, separate Wi-Fi pages, persistent identities table and inline inspector | Extend these into the master-list workflow; retain existing details |
| Security | Separate coverage, baselines, incidents and replay/flood evidence | Link relevant evidence without changing existing detector rules or granting trust |
| Other modules | Cyclostationary, drone broadcast/Remote ID and model-related work exist separately in the repository | Leave their pipelines and outputs independent; no model dependency for enrolment |

Important existing gaps:

- A decoded MAC currently receives green text in the identity table. This indicates a decoded address, **not approval**. Change that visual meaning in this module.
- There is no persisted operator approval, acknowledgement, new-device lifecycle or accepted RF reference.
- Exact MAC matching currently routes accepted readings to one record; it does not establish that the physical transmitter is unchanged.
- The existing RF-cluster tolerances are starting values, not independently validated Wi-Fi authentication thresholds.
- Identity summaries primarily expose the latest digital state and latest RF reading, not a chronological change history.
- The master store writes on the caller's path. Additional continuous history must not add unbounded receive-path file I/O.
- The ordinary identity path has a 400-burst-per-capture limit. Further decoded security-only traffic is not presently enrolled through that path. Some shorter DSSS traffic is also intentionally security-only.
- Several older comments/documents still describe DSSS-only decoding. Current scanner integration also has legacy OFDM decoding. Use current code and validation reports when writing capability text.

## 3. What one inventory entry means

Use an internal stable record ID, with claimed addresses stored as identifiers and evidence. For the first release, a BSSID remains the discovery lookup key for AP/network-interface records. An SSID is never a unique key.

Maintain three explicitly typed record classes:

1. **AP / network interface:** discovered from supported beacons/probe responses. A router can advertise several BSSIDs, and two unrelated transmitters can advertise the same SSID.
2. **Observed transmitting interface:** later milestone, based on a usable transmitter address from another decoded frame. It may be a station/client, AP or an undetermined role. Do not call it a phone or drone without separate evidence.
3. **Unidentified RF candidate:** a provisional RF cluster without a decoded transmitter identity. Keep these in a separate view; their count is not a physical-device count.

If the same address is observed with incompatible RF/context evidence, preserve a conflict episode under the claimed identity. Do not overwrite the reference or invent a confirmed second physical device. Comparing simultaneous emissions requires actual coverage/timing evidence; different sequential channel observations alone cannot prove simultaneity.

Use one store across both Wi-Fi bands, with separate measurement profiles by context. Different addresses on one dual-band router are not automatically merged. A later optional operator asset label can group interfaces without erasing their evidence or allowing approval to propagate automatically.

## 4. Enrolment, presence and warning states

Keep these as independent fields rather than one overloaded colour/status enum.

| Dimension | States | Meaning |
|---|---|---|
| Review/approval | Unreviewed, acknowledged/unapproved, approved, explicitly disallowed | Operator policy; persisted across restarts |
| Presence | Observed recently, not recently observed, coverage unavailable | Observation freshness, never proof that a device has left |
| RF profile | Unavailable, collecting, comparable, drift review, conflicting, context changed | Whether measurements support comparison under the current conditions |
| Alerts | New identity, returned unapproved identity, digital change, RF divergence, related security incident | Separate events with evidence and acknowledgement |

### New-device workflow

1. On the first eligible decoded observation of a previously unseen identity, create the record immediately and show a red **NEW DEVICE — UNREVIEWED** badge. Show whether it has only one observation or has been seen repeatedly.
2. Persist its first-seen time, source evidence and discovery-event ID. Repeated beacons update that entry instead of generating a new alert each time.
3. **Acknowledge** clears the unread notification, but leaves a red **UNAPPROVED** badge. **Approve/enrol** is a separate explicit action with an operator label/note and audit entry.
4. Allow approval when digital evidence is available but RF quality is insufficient. Display **Approved identity / RF profile unavailable or collecting**; do not imply RF verification.
5. Subsequent routine observations retain approval. Material changes create independent warnings; acknowledgement of a warning does not accept a changed RF reference.
6. A disallowed record remains visible with **DISALLOWED — OBSERVED** when seen again. This is a monitoring label, not radio blocking.
7. Reappearance can produce one deduplicated **KNOWN BUT UNAPPROVED — RETURNED** episode after a configurable, coverage-aware absence interval. Never label an already stored identity as first-ever new again.

At upgrade, import legacy entries as **PREVIOUSLY OBSERVED — REVIEW REQUIRED**. Do not silently approve them, and do not generate a first-discovery flood for the imported inventory. The operator can review a selected batch and approve it with one explicit action. There is no automatic “trust everything during the first scan” mode.

Red indicates attention needed, not proven maliciousness. Add text/icons so colour is not the only cue. Approved rows use a restrained approval badge; a red incident/conflict badge remains visible even on an approved row.

## 5. Data to save and expose

### 5.1 Digital identity and provenance

Preserve all currently decoded fields, with these groupings:

- Stable record ID; claimed BSSID and, when available, transmitter address; observed role and address source; locally administered/group-address flags.
- Exact SSID bytes and safe display text; latest advertised and last-known names with independent observation times. Show SSID history rather than silently replacing a name.
- First/last observed time, first/last session, observed bands/channels and per-band recency. Separate monitored frequency from an advertised channel.
- Advertised security, cipher/AKM suites, PMF, capability bits, beacon interval and advertised PHY capabilities. Retain raw selectors/structured values where the parser already provides them; add missing parsing only as a separately tested extension.
- WPS manufacturer/model/device-name hints and their provenance; vendor-registry assignment with lookup source. These are advertisements/assignments, not verified owner/model information.
- Observation counts, accepted/rejected RF counts, independent captures and sessions represented. Distinguish lifetime counts from retained raw records.
- Operator alias, owner/location notes if supplied, approval state/time, reason and audit history. A local operator label is not an authenticated login identity unless access control is later implemented.
- Change events storing old/new values, first/latest occurrence, count, source frame and review status. Incomplete IEs must not imply a security downgrade or erase previously valid fields.

Do not promise IP address, hostname, serial number, operating system, location, application content or exact device category from beacon decoding. Show unavailable fields honestly and extend parsers only when a supported source provides the information.

### 5.2 RF / silicon-related measurements

Call these **RF hardware measurements** in the GUI, with a note that receiver hardware and propagation can affect the estimates. They are not a cryptographic silicon ID.

| Measurement | Store/display | Intended use and limits |
|---|---|---|
| CFO | Latest value, ppm, robust centre/spread, trend and reference deviation | Oscillator behavior; supporting evidence, never a sole identity key |
| Image/IQ estimates | Existing image ratio, IQ amplitude error and IQ phase error, with units/validity | Compare compatible OFDM observations; do not fill unavailable DSSS estimates with zero |
| DC estimates | Relative DC level and phase with estimator version/context | Supporting observations, sensitive to the capture/receive chain |
| Quality | SNR or SNR proxy, EVM, sync correlation, sample count, gate outcome/reason | Determine measurement eligibility; not identity features by themselves |
| Context | PHY/rate, channel/bandwidth, actual sample rate, receiver/session, RF chain/antenna, gain/AGC and available clock/calibration state | Decide whether observations are comparable |

The current `irr_db` implementation stores `20*log10(abs(nu/mu))`, typically a negative image-to-signal ratio. Retain its numeric convention and label it explicitly; do not silently present it as a positive rejection ratio or reverse historical values.

DSSS currently cannot estimate the three image/IQ fields using its real-valued training representation. Legacy zeros in those fields are **unavailable**, not perfect hardware. Its SNR estimate is also a different proxy from the OFDM pre-burst-noise measurement. Treat each PHY separately.

Preserve a measurement-validity mask or optional values. Distinguish unsupported, not attempted, rejected, non-finite and valid measurements. Store rejected-attempt counts/reasons and bounded examples so poor reception does not look like a stable profile. Do not add PA nonlinearity, spectral regrowth or transient estimators in this enrolment upgrade.

### 5.3 Exact source linkage

Use capture provenance already available in the security event contracts: run ID, radio session, capture sequence, sample offset, PHY, source kind, clock domain and timing uncertainty. Reuse the same observation ID for the identity, RF reading and related event.

Only join an RF extraction to an identity from the same decoded burst. Never attach the closest anonymous burst by time, power or RF similarity. For general frames, use the actual transmitter-address semantics, not the destination or BSSID indiscriminately. Frames without a usable transmitter address do not enrol the receiver as though it transmitted.

Legacy records without this provenance remain readable and are marked legacy-context. Their timestamp is not upgraded into sample-accurate receive time. Live, offline replay, imported datasets and synthetic fixtures have separate stores/namespaces; replay must not create live “new device” alerts or update approved live profiles.

## 6. Continuous updates without learning away an intruder

Maintain three layers for each identity and comparable RF context:

1. **Observation history:** accepted/rejected evidence, latest values and bounded summaries. Update as new eligible data arrives, including anomalous observations.
2. **Enrolment reference:** a versioned snapshot accepted by the operator, with evidence span, sample diversity and estimator version. Preserve previous versions.
3. **Adaptive operating profile:** a robust, slowly changing estimate derived from eligible observations. It may track benign variation within limits; it does not erase or automatically replace the enrolment reference.

This separation meets the continuous-update requirement without treating every new sample as trusted training data.

### Eligibility and context

- Record a valid digital observation even when its RF extraction fails. RF failure cannot make a newly decoded identity disappear.
- Aggregate at the capture/window level before adaptation, limiting the influence of one high-rate transmitter. Deduplicate repeated processing by observation ID without discarding genuinely repeated over-the-air transmissions.
- Only consider finite, supported, quality-gated measurements with a same-burst identity association. Use capture health/continuity and overload information when judging readiness.
- Separate profiles at least by PHY, channel/band and receiver chain/configuration; record estimator version. Sample-rate, gain mode, calibration or hardware changes may require a new comparison context.
- Reconnecting or moving a receiver triggers a context check/settling period. A changed receiver or unknown calibration must not silently retrain every enrolled device.
- Unknown/mixed sources, active conflicts, large unexplained changes or relevant security incidents pause trusted adaptation while continuing to record observations. Missing/delayed incident input means the security context is unavailable, not clear.

### Robust statistics and drift

For each comparable context, calculate robust centre/spread, quantiles, accepted fraction, effective capture count and time coverage. Use median/MAD or another documented robust estimator, with a nonzero noise floor determined by calibration. Angular values need circular statistics. Do not count strongly related image/IQ estimates as independent evidence.

Start with simple deterministic statistics; no new ML classifier is required. Compare new window summaries against both the accepted reference and the current operating profile **before** updating the latter. Store per-feature deviations and the decision reason rather than an unexplained “identity confidence %”.

Allow small consistent changes to update the operating profile with a time-based, bounded learning rate. Bound both the rate of change and total deviation from the accepted reference. A packet-count-based update would let a burst flood accelerate learning and is unsuitable here.

Handle outcomes explicitly:

| Observation | Action |
|---|---|
| Insufficient accepted captures or unsupported measurements | Keep collecting; identity remains visible; no RF match verdict |
| Compatible repeat observations | Update summaries and, if enabled and eligible, adaptive profile |
| Gradual CFO change with otherwise consistent evidence | Track drift within calibrated bounds; retain original reference |
| Sudden persistent multi-feature change in comparable conditions | Freeze adaptation; show **RF PROFILE CHANGED — REVIEW** |
| Two distinct recurring measurement groups for one claimed address | Preserve both groups as conflict evidence; show possible identity conflict, not confirmed spoofing |
| Long gap, receiver change, new channel or changed acquisition conditions | Show **CONTEXT CHANGED / COMPARISON PENDING** and collect a context-specific profile |
| Material change accepted by operator | Create a new reference version and audit event; preserve old reference/history |

Do not extrapolate a drift correction over long unobserved gaps or increase tolerance without limit. Repeated small changes also need cumulative-change monitoring to resist gradual contamination. Reference-update candidates should have a review/quarantine interval; keep enough input provenance to exclude a candidate if a related incident arrives later.

Both transmitter and receiver frequency references contribute to measured CFO. Receiver warm-up or environmental changes can therefore resemble device drift. Initially record those conditions and require context review; automatic common-mode receiver correction is a later experiment requiring independently stable references. NIST describes frequency drift as potentially environmental, bidirectional and nonlinear. [NIST frequency terminology](https://www.nist.gov/pml/time-and-frequency-division/popular-links/time-frequency-z/time-and-frequency-z-f).

Published Wi-Fi fingerprinting research also treats CFO as time-varying supporting evidence and identifies propagation effects as a challenge. Its reported accuracy is not transferable to our estimator or deployment. [Xie, Peng and Zhang, IEEE INFOCOM 2025 paper](https://arxiv.org/abs/2412.07269).

**Initial rollout:** collect summaries and show comparison diagnostics first. Automatic trusted adaptation stays off until multi-session calibration/evaluation supports it. Digital inventory and alerts can ship before that gate. Numerical drift tolerances, learning time constants and maturity thresholds must be versioned calibration outputs, not inherited from the current anonymous-cluster matcher.

## 7. GUI layout and operation

Extend **Wi-Fi → Identities & RF clusters** into **Wi-Fi → Master list & enrolment** in both existing band pages. The same global store backs both views. Default to the selected band's observed identities, with explicit **This band / All Wi-Fi** filters; show per-band ages and do not use only the last observed frequency to decide membership.

```text
Master list & enrolment           This band / All Wi-Fi     Search
New [n]  Unapproved [n]  Approved [n]  Review changes [n]  Storage health

Status             Name / SSID      Address          Seen       RF state
NEW DEVICE         Unknown AP      aa:...            2 s ago    Collecting
APPROVED           Office Wi-Fi    bb:...            8 s ago    Comparable
PROFILE CHANGED    Lab hotspot     cc:...            5 s ago    Review

Selected identity: digital details | RF profile | history | security
Actions: Acknowledge | Approve/enrol | Mark disallowed | Add note
         Freeze adaptation | Review reference update | Export evidence
```

The diagram uses illustrative rows, not actual detected devices.

### Main table

Default columns: attention/approval badges, alias/SSID, BSSID/transmitter address, observed role, band/channel, last observed age, RF profile state and related incident count. Secondary columns expose vendor, WPS hints, security/PMF, first seen, observation/capture counts, accepted fraction and current measurements.

Pin new/unreviewed and changed entries above ordinary rows by default. Provide filters for approval, presence, band/channel, SSID/address/vendor, RF readiness and incidents. Preserve selected record IDs while updates arrive; do not move keyboard focus to a new row. Use clipped visible rows and cached sorting/filtering.

### Inspector

1. **Identity:** complete decoded fields, approval/alias, current versus last-known values and exact source times.
2. **RF profile:** reference/current centre, spread, recent value, deviation, validity, sample/capture counts, context and last update reason. Plot CFO and selected IQ/DC history using bounded summaries. Show capture gaps; never draw invented continuous history.
3. **History:** discoveries, name/security/channel changes, RF events, acknowledgements, approvals and reference versions. Distinguish an operator action from an RF observation.
4. **Security:** linked incidents and evidence scope. Joining by BSSID alone is labelled a claimed-address association; exact observation links use capture IDs when available.
5. **Coverage/storage:** last useful monitoring opportunity, active versus viewed band, decode limitations, queue/disk errors and pending persistence.

Use a non-modal new-device notification and an unread counter. Deduplicate by discovery event; no pop-up for every beacon. Red row tint/badges remain readable in the existing theme. Acknowledgement never clears a separate attack/conflict warning. All actions operate on explicitly selected records; browsing/filtering must not retune the USRP or change acquisition.

Add an Overview card for unreviewed Wi-Fi identities and a shortcut to the filtered master list. New-identity events have their own event type; do not inflate existing attack-incident counts by calling all unknowns attacks. Reuse the Security inspector only through an additive, typed integration.

## 8. Acquisition, persistence and C++ integration

### Data path

```text
Existing USRP capture → existing Wi-Fi decode + RF extraction
    → same-burst enrolment observation + capture outcome
    → bounded enrolment worker → extended Wi-Fi master repository
    → immutable inventory snapshot → existing native GUI

Existing security pipeline remains independently fed.
```

The enrolment consumer must run while receiving regardless of which GUI page is open. It consumes existing decoded results; it does not start a second receiver or duplicate IQ decoding. Do not replace the existing frame observer used by other add-ons: introduce a bounded additional consumer or explicit fan-out that preserves its callback.

Use the full supported decoded-frame stream for inventory discovery, including security-only paths, rather than reading the GUI's 1,000-row packet tail. For AP discovery beyond the old identity limit, parse the already-decoded beacon/probe response without adding RF extraction to every extra burst. Those identities can legitimately have no RF profile. Capture/decode limits still apply and must be reported.

For trusted learning, stage observations until their capture outcome is known. Permit immediate provisional discovery display, then finalize its health/provenance. Missing capture completion cannot qualify a learning window. A bounded subscription to incident decisions may defer learning, but enrolment must never block the security worker.

### Persistence strategy

Extend the current repository behind a versioned adapter. For the first delivery, retain NDJSON compatibility and a single writer; a database rewrite is not required.

- New typed records store discovery/review transitions, metadata changes, measurement context, profile summaries and reference versions. Use stable event IDs and per-record revision ordering so replay/recovery is idempotent.
- Read old schema/reading/identity lines without modifying the originals during initial validation. Test migration against copies first; retain a recoverable pre-upgrade backup when activating the new schema.
- Persist event/state transitions together in a complete record so a crash cannot leave an approval with no corresponding audit decision. Complete records after a damaged tail remain recoverable.
- Use atomic replacement and durable flush for reference/approval commits and compaction checkpoints. Report an action as saved only after writer acknowledgement. Routine observations can be batched, with a visible last-durable-save watermark and pending count.
- Continue showing in-memory observations after a disk failure, clearly marked unsaved. Failures do not silently grant approval. On restart, pending unsaved actions must not appear committed.
- Keep a bounded recent raw-reading tail; aggregate older measurements by context/time. Preserve first/last seen, lifetime counters, approval state, accepted-reference versions and change-event summaries through compaction.
- Define configurable journal/archive quotas and visible retention periods. Coalesce repeated identical events; never silently discard the current approval/reference or present retained history as complete lifetime evidence.
- Restrict writing to one application instance per store. Another instance can report the conflict/use read-only access; concurrent uncontrolled append/compaction is unacceptable.
- Treat SSIDs/WPS strings and input files as untrusted bytes: bounded parsing, safe path keys, escaped rendering, and safe exports. CSV exports must not turn advertised text into spreadsheet formulas.

### Bounds and load

Use bounded queues, batches and profile windows. Coalesce routine repeats while prioritizing first discoveries, operator commands and changes. Expose every admission/drop/overflow counter. Reserve capacity does not guarantee capture under an unlimited address-flood attack; if exhausted, show **INVENTORY INCOMPLETE — INPUT OVERLOAD** and retain existing approved records. Never silently recycle them to admit new addresses.

Specify and benchmark queue bytes, record capacity, per-context raw history, archived-history limits and display snapshot size in the first implementation milestone. Keep these software-work limits separate from the receiver sample rate; this module does not introduce a new RF sample-rate cap.

### Suggested source responsibilities

| Existing/proposed area | Responsibility |
|---|---|
| `src/wifi_master.*` | Existing identity compatibility and authoritative master repository; versioned persistence |
| Proposed `src/wifi_enrolment.*` | Observation ingestion, approval/discovery lifecycle, bounded worker, snapshots and commands |
| Proposed `src/wifi_profile.*` | Validity/context-aware summaries, reference comparison and adaptation policy |
| `src/scanner.*` | Publish same-burst inputs and capture outcomes, including decoded security-only paths |
| `src/security/wifi_frame_event.*`, `wifi_mac_frame.*` | Reuse capture provenance and address semantics; additive adapter where needed |
| `src/wifi_gui.*` | Master table, badges, filters, profile/history inspector and explicit commands |
| `src/main.cpp`, `src/gui/app_shell.*` | Existing navigation/Overview integration; no decoder logic |
| `tests/` | Isolated persistence, identity, profile, worker and GUI fixtures |

Preserve existing public behavior while introducing an adapter; avoid simultaneously keeping the old direct write path and the new worker writing the same observation. Keep legacy GUI fallback on the same repository instance. LoRa storage, profiles, detectors and UI remain unchanged.

## 9. Client visibility: required extension, with honest limits

Beacon enrolment chiefly inventories APs/hotspots. A station that never advertises a beacon needs supported frames it actually transmits to become visible.

After the AP workflow is stable, ingest usable transmitter addresses from supported FCS-valid management/data frames, including decoded security-only traffic. Reuse the existing MAC parser's address-role logic. Associate observed station-to-BSSID relationships as evidence with timestamps, not permanent ownership. Frames mentioning another device only as a receiver must not create a claim that it was heard transmitting.

MAC randomization can produce multiple observed identities for one device. Same SSID, vendor, RF similarity or association history cannot automatically collapse these into a physical identity. Maintain uncertainty and optional manual labels. No encrypted payload decryption or unsupported HT/VHT/HE/EHT decoding is added here.

This milestone adds useful client visibility, but the product label remains **observed Wi-Fi interfaces**, not **every nearby phone/router/drone**. A silent device, an unsupported transmission, an unmonitored channel or a weak/overlapped frame can remain unseen.

## 10. Milestones and acceptance gates

| Milestone | Deliverable | Completion evidence |
|---|---|---|
| E0 — Baseline and contracts | Inventory current identity/RF paths, schema, bounds and source keys; write migration/compatibility fixtures | Existing behavior captured; legacy copies load; no writes to production data; numerical adaptation thresholds explicitly unresolved |
| E1 — Persistent discovery and enrolment | Extend identity state, discovery events, acknowledge/approve/disallow actions, audit and startup recovery | New address is saved once; restarts retain state; imported entries stay unapproved; acknowledging never approves; disk errors visible |
| E2 — Master-list GUI | Band/all-band table, red new/unapproved badges, filters, inspector, Overview notification | Real GUI fixtures exercise discovery, selection, review and persistence status without receiver commands |
| E3 — RF evidence and history | Validity/context metadata, exact joins, reference snapshots, robust summaries and bounded plots | DSSS unavailable values remain unavailable; legacy context handled; no cross-PHY/channel/receiver mixing; all prior measurements retain their meaning |
| E4 — Drift and conflict review | Shadow comparisons, cumulative-change checks, freeze/rebase workflow; gated adaptation capability | Held-out multi-session testing supports chosen limits; unexplained changes cannot rewrite references; learning remains off when evidence is insufficient |
| E5 — Wider decoded-interface coverage | AP discovery beyond the GUI identity tail and supported transmitting-client records | Full decoded-stream fixtures cover security-only paths and correct TA/RA/BSSID semantics; no receiver-only or randomized-address auto-merges |
| E6 — Integration and qualification | Security evidence links, overload/long-run evaluation, recovery and deployment documentation | Existing regression suites pass; measured additional load and capture/queue loss; bounded memory/storage; known limitations published |

Recommended delivery order: **E0 → E1 → E2 → E3**, then complete **E4/E5 → E6**. E1/E2 provide immediate unknown-identity defence value. E4 has a software-complete diagnostic stage and a separate hardware-qualified adaptation gate; do not hold the basic inventory hostage to long-term RF testing or claim that testing finished early.

## 11. Validation plan

### Software/offline checks

- Restart, partial-write, malformed-record, read-only directory, disk-full, writer-lock and migration recovery; no approvals lost or silently invented.
- Same BSSID repeats, hidden SSIDs, legitimate renames, missing/malformed IEs, advertised-channel changes, locally administered addresses and repeated ingestion of one burst.
- One SSID on multiple BSSIDs; one address with conflicting profile groups; approved and unapproved identities sharing similar RF values; approval does not propagate.
- Single-observation new device, repeated sightings, acknowledgement, approval, disallow, return after monitoring gaps and imported historical records.
- Exact same-burst association; security-only discovery; invalid FCS; no transmitter address; address-role permutations; offline/live isolation.
- Steady measurements, gradual CFO drift, abrupt shifts, slow cumulative contamination, correlated IQ features, angular wrap, outliers, poor SNR and non-finite estimates.
- Receiver reconnect/configuration changes, stale/missing capture outcomes, delayed security warnings, queue saturation and a flood of changing addresses.
- Stable GUI selection during refresh, colour/text semantics, all-band membership, accessible optional fields and no acquisition actions from navigation.

Run existing Wi-Fi decoder/fingerprint/master/identity/security/scanner-lane and GUI tests alongside the additions. Use isolated temporary stores and existing recordings read-only. A documentation review is not evidence these future tests have passed.

### Receive-only hardware checks, when available

No drone is needed. Use a labelled set of known APs/hotspots and, for E5, clients that transmit supported frames. Compare recordings with independently recorded device addresses/configuration and distinguish randomized addresses.

1. Establish fixed-channel baseline sessions on 2.4 GHz and supported 5 GHz, then sweep-mode sessions. Record actual coverage rather than assuming continuous monitoring.
2. Revisit on separate days, after receiver/device warm-up, restart and ordinary environmental changes. Add a second same-model transmitter where possible; this is important for testing uniqueness rather than mere stability.
3. Change receiver gain/AGC, antenna/position and receiver hardware deliberately in labelled sessions to measure receiver/context effects. Do not pool these into the same reference without evidence.
4. Introduce an additional known but unenrolled AP/hotspot and verify first-observation latency, red notification, approval behavior and restart recovery.
5. Measure new-identity detection conditional on supported decoded frames, end-to-end RF reception yield, duplicate records, false change alerts per observed device-hour, unknowns incorrectly approved/matched, queue drops and compute overhead separately.

Split calibration and evaluation by session/day and physical unit where applicable; adjacent frames from the same capture cannot establish independent generalization. Long-term aging claims need genuinely separated recordings. Synthetic drift is useful for algorithm tests but does not validate months of hardware behavior.

Set operational false-alert and latency targets before tuning, and record dataset size, coverage, uncertainty and failure cases. If available measurements do not distinguish same-model units reliably, ship inventory and change monitoring with **RF identity verification unvalidated** rather than loosening thresholds until they pass.

## 12. Precise scope and limits at completion

The completed module should automatically retain eligible observed identities, expose their digital/RF history, highlight newly discovered and unapproved entities, support explicit enrolment, and flag changes relative to a preserved reference. It should tolerate ordinary validated drift through bounded updates without losing historical evidence.

It will still have these limits:

- FCS verifies frame integrity, not an authentic transmitter, owner or immutable MAC identity. Beacons can claim the same address/SSID.
- BSSID, station-address and RF-cluster counts are not counts of unique physical devices.
- Current band coverage is channels 1–13 in 2.4 GHz and 36/40/44/48/149/153/157/161/165 in 5 GHz. This plan adds no DFS or 6 GHz support and does not make the receiver monitor both bands simultaneously.
- Current payload decoding is 1 Mbps DSSS and legacy 20 MHz OFDM. Advertised modern PHY capabilities do not imply those payload formats are decoded.
- Capture gaps, bursts beyond processing limits, weak signals, unsupported waveforms and non-transmitting devices limit discovery. “Not seen” is not “absent” or “safe”.
- A same-address adversary may be indistinguishable with available evidence. RF anomaly detection is supplementary evidence, not guaranteed authentication.
- Approval is local operator policy; it does not authenticate a device or exempt it from security monitoring.
- LoRa changes, new ML training, radio countermeasures and completion of named drone-link identification are outside this work.

## 13. Implementation starting point after approval

Start with **E0/E1: persisted review state and discovery events in the existing Wi-Fi master repository**, then E2's red new-device workflow. Next add context-aware RF history before enabling any adaptation. This delivers useful monitoring early while giving drift handling the evidence and controls it needs.

Reviewed local references:

- `src/wifi_master.hpp`, `src/wifi_master.cpp`
- `src/wifi_fingerprint.hpp`, `src/wifi_fingerprint.cpp`
- `src/scanner.cpp`, `src/scanner.hpp`, `src/config.hpp`
- `src/wifi_frame.hpp`, `src/wifi_gui.cpp`
- `src/security/wifi_frame_event.hpp`, `src/security/wifi_mac_frame.hpp`
- `src/main.cpp`, `src/gui/app_shell.hpp`
- `tests/test_wifi_identity.cpp`, `tests/test_wifi_master.cpp`
- `docs/WIFI_IDENTITIES.md` (its final DSSS-only scope paragraph is superseded by current OFDM integration)
- `docs/GUI_REVAMP_IMPLEMENTATION.md`, `docs/GUI_REVAMP_INVENTORY.md`
- `docs/WIFI_SECURITY_VALIDATION_2026-10-11.md`
- `gui_plan.md` (earlier design context; current GUI implementation takes precedence)
