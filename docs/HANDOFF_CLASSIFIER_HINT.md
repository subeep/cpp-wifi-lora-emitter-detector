# Handoff: classifier as a hint inside a link row

Date: 2026-11-12. For the second agent. Nothing here is to be committed.

## Why

The agreed order of evidence is: (1) link detector and timing group, (2) model name from decoded
DroneID, (3) the RF classifier only as a ranking of candidates. Parts 1 and 2 work live. Part 3
is disconnected: `src/coordinated_panel.cpp` passes an empty `ml::Snapshot{}`.

The classifier matters for rows that will never get a name from DroneID: aircraft whose
DroneID is encrypted (Mavic 3, Mini 4 Pro, Mini 5 Pro in DRFF-R2) and "Link only" rows.

On its own the classifier is not trustworthy: it gave a drone name to 48% of no-drone
intervals (`docs/DRONE_RF_TIMING_GATE_DIAGNOSTIC_2026-11-10.md`). So it must never create a
row, change a row's identity, or raise the conflict flag.

## What to build

1. **Join classifier output to link rows.** For a current link row (link-only, or DroneID
   present but encrypted/unreadable), take classifier results from the same captures that carry
   the link sighting (same run, session, capture number). Results from captures without a link
   sighting are ignored.
2. **Restrict to the row's candidates.** Keep only classes that the model table
   (`model_links` / `candidates_for()` in `src/drones/evidence_claims.hpp`) lists for the
   row's timing group. Map classifier class names to table names in one explicit table; a
   class with no mapping is dropped and counted.
3. **Show it as a hint.** One extra field on the row, for example
   "Classifier hint: Mini 4 Pro (7 of 9 link captures)". Wording must say it is a hint. Rows
   named by DroneID show no hint. No hint when fewer than a stated minimum of link captures
   voted (choose it, name the constant, say why).
4. **Log it** in the detection log as its own field, not part of `identity_display`, and keep
   it out of the comparison that decides whether a row "updated" unless the top hint changes.

## Limits

- No new model training, no threshold tuning on the locked DRFF-R2 test units (Air 2 units
  5–8, Air 2S 5–7, Mini 4 Pro 5, mavic3C, mavic3S): those files must not be opened.
- The classifier process and its inputs stay as they are; this task only consumes its output
  (`src/ml_inference.cpp` already parses `top_candidates`).
- If the classifier is off or not ready, rows look exactly as today.

## Files

- Yours: `src/drones/evidence_claims.hpp` (row fields), `src/drones/evidence_log.hpp`,
  `src/coordinated_panel.cpp`, the Drones table in `src/main.cpp`,
  `tests/test_drone_evidence_claims.cpp`, a new small header for the join if useful.
- Not yours: `tools/drone_native_worker.cpp`, `src/drones/detector.*`, `src/drones/addon.hpp`,
  `src/drones/droneid_followup*.hpp`, `src/scanner.*`, the `model_links` table itself.

## Done when

- Tests cover: hint appears only on link rows; classes outside the row's group are dropped;
  a classifier result from a capture without a link sighting is ignored; too few votes gives
  no hint; a DroneID-named row never shows a hint; classifier off changes nothing.
- Existing drone tests pass, `rf_monitor_gui` and `coordinated_live_check` build.
- A short note to the user states which classifier classes could not be mapped to the model
  table.
