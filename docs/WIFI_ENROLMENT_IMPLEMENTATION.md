# Wi-Fi enrolment implementation

16 October 2026. Implements the software inventory/review workflow from
`../wifi_enrolment_plan.md` in the existing C++/Dear ImGui application.
No live receiver/transmitter was operated for this delivery. Existing unrelated
LoRa changes in the working tree were preserved.

## Delivered behavior

- One persistent master repository across supported Wi-Fi bands. Existing
  identities load as previously observed and unreviewed, never automatically
  approved. New eligible claimed identities display **NEW DEVICE** in red.
- Acknowledge, approve/enrol, disallow, revoke approval and label/note operations.
  Acknowledgement does not approve; approval does not clear digital/RF warnings.
  Commands run on the background consumer and show their saved/failed result.
- Full decoded-frame discovery includes beacons beyond the old 400-row identity
  path and transmitting interfaces from supported non-beacon frames. No extra
  IQ decode or RF extraction is added to security-only frames. Receiver-only
  addresses and invalid/group transmitter addresses do not create entries.
- Digital metadata remains in the existing master files. Name, advertised
  security/PMF/channel/capabilities/WPS changes also have bounded review history;
  omission/incomplete IEs cannot fabricate a downgrade in that history.
- Exact burst provenance associates accepted RF readings and packet rows with
  their claimed transmitter. Anonymous clusters remain separate; their packet
  association becomes available after background processing, not by nearest-time
  guessing. Client records do not acquire a fabricated SSID.
- RF profiles use independent capture summaries, median/MAD and circular angle
  statistics. DSSS image/IQ fields are unavailable, rather than zero-valued
  identity evidence. Rejected RF does not discard valid digital observations.
- Explicit accepted-reference versions remain fixed while recent summaries
  update. Repeated outlying capture measurements latch an **experimental RF
  review** warning. This is not authenticated spoof detection or a probability.
- Master list: band/all-band filters, search, review filters, approval/change
  badges, inline identity/profile/history inspector, explicit controls and
  clipboard JSON evidence. Anonymous clusters have an explicit opt-in filter.
  Overview shows a new-identity counter and Wi-Fi shortcut. Security associations
  identify matching claimed addresses in the published incident tail only.

## Architecture and isolation

`WifiEnrolment` owns `WifiMasterList` and a bounded consumer thread. Scanner sends
immutable decoded-frame/RF observations followed by the completed capture outcome.
The producer does not wait for disk I/O. Existing security and add-on observers
remain independently fed. GUI reads cached snapshots and enqueues commands.
Stop/destruction drains admitted work. Queue loss is visible and means the
inventory is incomplete; it does not change the receiver sample rate.

Only source `live` enters this consumer. Offline data must use isolated stores
and an explicit future import path; replay cannot update live approval silently.
Observation IDs deduplicate within a running consumer's monotonic capture stream.
This is not a cross-restart import/replay deduplication service.

The original per-device NDJSON format remains readable; a typed `seen` record
allows a transmitter with no beacon or accepted RF reading to survive restart.
Per-identity `.enrol.json` checkpoints hold review policy, recent history,
measurement windows and accepted references. First modification of an imported
identity saves its original NDJSON as `.ndjson.pre-enrolment`. Do not delete those
backups when rolling back the GUI.

A store lock makes another upgraded writer read-only. Older executables that do
not implement the lock must not be run against the same writable store. An
existing old process was not stopped or restarted during this work.

Approval/reference checkpoints flush the underlying identity and use a flushed
temporary file, atomic replacement and directory fsync. A failure before replacement
leaves the prior policy active. A failure after replacement is reported as applied
with durability **UNCONFIRMED**, not as a successful durable save. Corrupt approval
checkpoints are preserved and fail closed to review-required; repair is explicit.
Routine data may remain unsaved on failure and the persistent warning must be
resolved. The local filesystem is not a tamper-proof authentication database.

## Bounds and exact limits

| Item | Implemented bound/meaning |
|---|---|
| New master admission | 4,096 records; previously stored records are not silently deleted |
| Receive queue | 64 capture jobs, estimated 32 MiB input budget; up to 8 additional command jobs |
| Per job | At most 4,000 decoded observations and 400 anonymous accepted RF readings |
| Source sessions | 64 independent runs per consumer lifetime; newer sessions replace the run high-water; stale sessions rejected |
| Packet associations | Latest 2,048 exact observation-to-record associations |
| Raw RF history | Existing approximately 1,000 readings, lazy slack to 1,100 |
| Profile contexts | 16 active per identity; oldest contexts retire into a bounded archive of 64 summaries; accepted reference versions remain preserved |
| Profile window | Latest 64 eligible independent captures per context; first eligible reading represents each capture |
| History | Latest 128 events; lifetime event count retained; bounded detail/source text |
| Accepted references | Up to 8 explicit versions, preserved; capacity exhaustion rejects further replacement |
| Reference readiness | At least 8 usable captures spanning 10 seconds in a context; a software readiness gate, not proof of hardware uniqueness |
| Diagnostics | 3 consecutive capture deviations beyond max(feature floor, 6 × 1.4826 × reference MAD) |

Diagnostic floors are 0.5 ppm CFO, 2 dB image/signal, 0.005 IQ amplitude error,
1 degree IQ phase, 3 dB DC and 10 degrees DC phase. These are explicitly
**uncalibrated diagnostic defaults**, not validated identity thresholds. Related
IQ fields are not counted as independent proof. All trusted automatic adaptation
is disabled. Accepting a reference is an explicit local policy decision, not
receiver calibration or authentication. The command accepts the qualifying
contexts available when the worker executes it and records their source IDs.

Receiver configuration, run/session, channel, sample rate, gain/AGC, antenna,
actual tune values, PHY and estimator version separate contexts. CaptureRecord
currently lacks a stable receiver serial/calibration ID. Therefore profiles from
different sessions are deliberately not treated as interchangeable. Historical
references survive restart, but a new session collects a separate context.

This version updates bounded rolling summaries; it archives retired context summaries, but does not implement indefinite
hourly/daily archives, cross-session receiver correction or an automatic trusted
profile-learning algorithm. Reference history is retained on disk; the GUI shows
the active reference per context and the version count. Full historical-version
comparison, reference export/import and capacity management are follow-ups.

## Validation and remaining gates

Validation uses temporary stores, constructed FCS-valid frames, the existing
recorded-fixture scanner test and offscreen EGL/Mesa GUI fixtures. Dataset and
production master files were not used as test output directories.

Software acceptance covers:

- discovery, acknowledgement versus approval, restart and label persistence;
- same-address repeats, duplicate bursts/captures, digital changes and hidden SSIDs;
- rejected/nonfinite RF, capture health, context separation, fixed references and
  persistent divergence warnings;
- client-only records, transmitter/BSSID mismatch and receiver-address exclusion;
- store locking, failed replacement, malformed policy and imported-record backup;
- bounded history/raw compaction and per-capture influence under packet floods;
- preserved scanner/security/cyclostationary outputs in the existing lane test;
- native master inspector rendering and explicit acknowledgement control.

Final validation: **50/50 CTests passed** in 22.28 seconds. The enrolment suite
passed **70 checks**, and the offscreen GUI passed rendering and
explicit mouse acknowledgement checks. The final source hashes, test output and
synthetic screenshots are in `wifi_enrolment_validation/`. Passing these checks
is not live RF or long-term drift qualification.

Remaining hardware-dependent gate: labelled multi-session/day receive-only
recordings, same-model units, receiver changes and measured false alerts. No drone
is required. Automatic adaptation stays disabled until those measurements support
an explicit policy. Cross-session comparison requires reliable receiver context
or a reviewed calibration mechanism.

Other planned enhancements not claimed as delivered: coverage-aware returned-device
notification episodes, persistent user grouping of physical assets, multi-record
batch approvals, long-term summary archives, precise incident evidence joins beyond
claimed-address associations, and automatic incident-conditioned learning. There
is no active mitigation, unknown-device authentication or all-device detection
claim. LoRa implementation and detector policy were not changed by this module.

## Build and use

Use an isolated build; it does not overwrite the previous GUI executable:

```sh
cmake -S . -B build-wifi-enrolment -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DRFMON_ENABLE_DJI_DRONEID=ON -DRFMON_ENABLE_WIFI_CYCLO_SHADOW=ON
cmake --build build-wifi-enrolment -j 3
ctest --test-dir build-wifi-enrolment --output-on-failure -j 1
```

After stopping any older monitor that owns the receiver/store, launch
`build-wifi-enrolment/rf_monitor_gui` in the desktop session. Mesa software
rendering remains available through `LIBGL_ALWAYS_SOFTWARE=1` and
`__GLX_VENDOR_LIBRARY_NAME=mesa` if the desktop GLX driver requires it.
The application starts disconnected. Use either Wi-Fi page's **Master list &
enrolment** tab; approval actions are independent of which page is receiving.

The new sidecars must be retained for policy/history recovery. Legacy binaries
will not show enrolment state and may not understand client-only `seen` records;
returning to one is not a transparent enrolment rollback. Preserve the store and
use a copy of the pre-upgrade files for a deliberate older-version rollback.
