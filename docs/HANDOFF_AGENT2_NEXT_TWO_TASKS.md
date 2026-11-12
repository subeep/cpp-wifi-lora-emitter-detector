# Handoff: next two tasks for the second agent

Date: 2026-11-12. Do them in this order. Nothing here is to be committed.

## Task 1: make the classifier hint mean something (short)

File: `src/drones/classifier_hint.hpp` and its tests.

1. **Use the scores.** Today the first of the classifier's top three candidates that fits the
   row's timing group votes, whatever its score. A candidate votes only when it is the
   classifier's top candidate for that interval, or its score is at least a named minimum
   (choose it, state why; do not tune it on the locked DRFF-R2 test units).
2. **Groups with one eligible class.** In the 30 kHz group only Mini 4 Pro is a classifier
   class, so the hint can say nothing else. When only one mapped class is eligible for the
   row's group, show no hint (or show "only class available: X", clearly worded) and log which.
3. **Map `DJI_Phantom_4_Pro_Plus` to "Phantom 4 Pro"**: the model table's 32.8 kHz row for it
   was measured on that same recording.
4. **Unmapped count.** Report distinct unmapped class labels seen since start, not a number
   recounted on every refresh.

Done when tests cover: a low-score third candidate does not vote; a one-class group gives no
plain hint; Phantom 4 Pro Plus maps; the unmapped figure does not grow on a repeated refresh.

## Task 2: Remote ID drones in the same table

Why: drones with a Wi-Fi video link (Mavic Mini, Mini SE, Spark, most non-DJI) are invisible to
the link detector and send no DJI DroneID. The only identity they give is broadcast Remote ID,
which `drone::RemoteIdMonitor` (`src/drones/remote_id.hpp/.cpp`) already decodes into tracks.
Those tracks are shown apart from "Current drone detections". The user wants one table.

Build:

1. **Remote ID rows.** Each current Remote ID track becomes a row in `detections()` output,
   id `rid:<id from the track>`, with its own evidence type. Display the self-reported ID and
   UA type as "self-reported", the same wording rule as DroneID serials.
2. **Merging with existing rows.** Merge a Remote ID row into a DroneID row only on an exact
   serial match. Never merge on band, time or timing group alone. A Remote ID row and a link
   row for what may be the same aircraft stay two rows.
3. **Lifetime.** Reuse the row lifetime rules (current while packets keep arriving, then
   expire); pick the Remote ID lifetime from its broadcast interval and name the constant.
4. **Log.** `rfmon.drone_detection.v1` gains a `remote_id` evidence block; existing fields keep
   their meaning.
5. **Table.** One table in the Drones view with an "Evidence" column saying which of link,
   DroneID, Remote ID, classifier hint support the row.

Limits: receive-only; no change to how Remote ID frames are captured or parsed; no aircraft
count claims; the UI redesign lives in `~/Documents/upgraded_ui` and is not part of this.

Files: `src/drones/evidence_claims.hpp`, `src/drones/evidence_log.hpp`, a small adapter header
from `RemoteIdMonitor` tracks to rows, the Drones table in `src/main.cpp`,
`tools/coordinated_live_check.cpp` only to print the new rows, tests in
`tests/test_drone_evidence_claims.cpp`. Not yours: decoder worker, detector, follow-up queue,
scanner, `src/drones/symbol_timing*`, `tools/drone_ml/`.

Done when tests cover: a track gives a row; it expires; exact-serial merge with a DroneID row;
no merge with a link row; the log block; and the existing drone tests pass with the GUI and
`coordinated_live_check` building.
