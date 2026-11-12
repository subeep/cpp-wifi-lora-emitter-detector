# Handoff: drone tracks — history, position and alerts

Date: 2026-11-12. For the second agent. Nothing here is to be committed.

## Goal

Today the Drones view shows what is on the air right now. When a row expires, everything known
about that drone is gone from the screen. This task turns current rows into **tracks**: one
record per drone for the whole run, with when it was seen, what identified it, where it said it
was, and an alert when a new one appears. It is the first operator-facing feature built on the
detection chain.

## Step 0: fixes from the review (do these first, they are small)

1. `remote_id_lifetime`: 10 s makes a Remote ID row flicker in coordinated mode, where a Wi-Fi
   channel is revisited about every 26 s. Use the link-row lifetime (45 s) and say why.
2. Remote ID to DroneID serial match: also match when the Remote ID serial ends with the
   DroneID serial and the extra leading part is a 4-character manufacturer code plus one
   length character (ANSI/CTA-2063-A form). Keep exact match as well. Mark the rule
   "unverified on a live aircraft" in the comment. Tests for both forms and for a near miss.
3. Classifier hint: the 0.50 rule for non-top candidates can never apply; replace it with a
   named minimum score for the top candidate (do not tune it on the locked DRFF-R2 test
   units). Map `DJI_Avata_2` to "Avata 2" and `DJI_Mavic_3_Pro` to "Mavic 3 Pro" (both are in
   the model table now).

## Step 1: track store

New header `src/drones/drone_tracks.hpp`, plain state, no radio, no threads of its own.

- Input: the map returned by `detections()` on every refresh, plus the time.
- A track is keyed by the strongest identity available: DroneID serial, else Remote ID id,
  else the link row id. When a link-only track later gets a serial (its row id changes from
  `band:...` to `serial:...` because the link was folded into a named row), the two must become
  one track, not two: carry the link track's first-seen time into the serial track.
- Per track keep: first seen, last seen, total time present, number of separate appearances,
  bands and link frequency ranges seen, timing group, model name and how it was obtained
  (DroneID, Remote ID, classifier hint only), serial (self-reported), Remote ID id, whether the
  identity was ever encrypted, conflict ever raised, last known position (step 2).
- A track is "present" while its row exists and "gone" afterwards; a gone track that comes
  back within a named gap (choose it, state why) is the same appearance.
- Bounded: at most a named number of tracks in memory (oldest gone tracks dropped first).

## Step 2: position

DroneID version 2/3 records and Remote ID Location messages carry positions.

- The decoder worker will emit these extra fields per packet (the first agent adds them to
  `tools/drone_native_worker.cpp`; do not edit that file): `reported_drone_lat`,
  `reported_drone_lon` (already there), `reported_height_m`, `reported_altitude_m`,
  `reported_pilot_lat`, `reported_pilot_lon`, `reported_home_lat`, `reported_home_lon`,
  `reported_speed_m_s`. A field is absent when the record does not carry it or it is zero/not
  fixed.
- Carry them through `evidence_packet.hpp` into `Packet` and into the track as "last reported
  aircraft position", "last reported pilot position", "home point", each with its time.
- Everything is **self-reported and unauthenticated**: say so wherever it is shown or logged.
  Reject values outside valid latitude/longitude ranges.
- Show distance and bearing from the receiver only if a receiver position is configured;
  otherwise show coordinates only. No map in this task.

## Step 3: alerts

- A new track raises one alert: "New drone: <identity>, <band>, <evidence>". A track that
  returns after being gone raises "Drone returned". A link-only track raises a lower-severity
  alert than a named one. No alert for classifier hints alone.
- Alerts go to the app's existing alert/incident surface if one fits (look at how Wi-Fi
  security incidents are shown) and to the log (step 4). No sound, no network, no pop-up
  outside the app.
- A flickering row must not produce repeated alerts: alerts come from tracks, not rows.

## Step 4: persistence and view

- Append track events to `data/drone_tracks.jsonl` (schema `rfmon.drone_track.v1`): opened,
  updated (identity or position changed, rate-limited), closed. Same size bound and rotation
  as the detection log. The existing detection log stays as it is.
- Drones view: a "Drone history" table under "Current drone detections": identity, model,
  first seen, last seen, time present, appearances, bands, evidence, last reported position,
  state (present/gone). Newest first, bounded rows.
- `tools/coordinated_live_check.cpp`: print the tracks in its JSON so live runs can be
  checked (tell the user which fields were added).

## Limits

- Receive-only. No count of aircraft is claimed: a track is "one identity or one link seen",
  and one aircraft can be two tracks (for example a link row and a Remote ID row).
- No UI redesign (that lives in `~/Documents/upgraded_ui`), no map, no network access.
- Not yours: `tools/drone_native_worker.cpp`, `src/drones/detector.*`, `src/drones/addon.hpp`,
  `src/drones/droneid_followup*.hpp`, `src/scanner.*`, `src/drones/symbol_timing*`,
  `tools/drone_ml/`, the `model_links` table. A replay tool is being added under
  `tools/drone_session_replay.cpp` by the first agent; it will call `detections()` and, when
  it exists, the track store, so keep the track store free of GUI types.

## Done when

- Tests (new `tests/test_drone_tracks.cpp`) cover: a row becomes a track; a link track that
  gains a serial stays one track with the earlier first-seen time; a row that disappears and
  returns inside the gap is one appearance, outside it two; alerts fire once per new track and
  once per return, and not on row flicker; positions outside range are dropped; the track cap
  holds; the log rotates.
- Step 0 tests pass; all existing tests pass; `rf_monitor_gui` and `coordinated_live_check`
  build.
- A short note to the user lists what is shown, what is logged, and what could not be
  verified without a live drone.

## Added after this brief was written: the model verdict

`src/drones/model_verdict.hpp` (first agent's file) gives one answer per row to "which model":
`verdict(row)` returns a tier (Confirmed, Likely, Family, Reported, Unknown), the answer text,
its basis and an optional detail line. It is already shown in the "Model" column of the
table and written to the detection log as `verdict`. Use it for a track's model and for alert
text instead of composing your own wording, and record the best tier a track ever reached.
