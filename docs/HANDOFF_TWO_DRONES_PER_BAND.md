# Handoff: keep two drones in one band as two rows

Date: 2026-11-12. For the second agent. Nothing here is to be committed.

## Problem

`rfmon::drone_claims::detections()` in `src/drones/evidence_claims.hpp` builds the rows of the
"Current drone detections" table. Two aircraft in the same band can end up as one row:

1. **Two link-only drones with the same timing group.** Link rows are keyed
   `band:<band>/<group>`, so two 15 kHz links at 2412 MHz and 2462 MHz are one row.
2. **One drone sends DroneID, another does not.** The fold at the end of `detections()` gives a
   link-only row to *every* DroneID row in the band with an empty or compatible group and then
   drops it. A second, silent drone disappears into the first one's row.
3. **Two DroneID drones, one link sighting.** Both serial rows take the same link's group.

## What to build

Use the centre frequency of the link sightings, which the store already has
(`Region::key.center`, `symbol_timing::Result::center`).

1. **Link clusters.** Within a band and group, join current link sightings whose centres are
   close into one cluster and keep clusters that are far apart as separate rows. Captures are
   20 MHz wide at Wi-Fi centres 5 MHz apart, so one link is seen at several neighbouring
   centres; the live Phantom 4 sighting on 2026-11-12 covered 2401–2419 MHz. Choose the joining
   distance from that and state it as a named constant with the reason.
2. **Row id.** Link-only row ids must stay stable while the link stays put and must not
   depend on which capture saw it last. Example: `band:2.4 GHz/15kHz@2410` with the cluster's
   lowest centre rounded to 5 MHz. The detection log compares ids, so an unstable id produces
   appeared/expired noise.
3. **Folding.** A DroneID packet's channel says nothing about which video link sent it, so
   fold only when it is unambiguous: exactly one compatible link cluster and exactly one
   DroneID row without a link in that band. Otherwise keep the link rows separate and leave the
   DroneID rows without a group.
4. **Display.** Show the cluster's frequency range in the table (new column or in the existing
   frequency text) so the user can tell two link rows apart.

## Limits to respect

- Do not claim a count of aircraft. Two clusters are "two separate links", which may still be
  one aircraft that changed channel between sweeps (a centre is revisited about every 26 s).
  Say so in the code comment.
- Withdrawal must still work per centre: a later capture of a centre without a link removes
  that centre from its cluster, and the row goes when the cluster is empty.
- The conflict flag stays as it is: same capture only, measured groups only.

## Files

- Yours: `src/drones/evidence_claims.hpp` (`detections()`, `Detection`),
  `src/drones/evidence_log.hpp` if the logged fields change, the table in `src/main.cpp`,
  `tests/test_drone_evidence_claims.cpp`.
- Read but do not change: the `model_links` table and `candidates_for()` at the top of
  `evidence_claims.hpp` (updated today), `tools/coordinated_live_check.cpp` (it prints the rows;
  tell the user if a field it reads is renamed).
- Not yours in this task: `tools/drone_native_worker.cpp`, `src/drones/detector.*`,
  `src/drones/addon.hpp`, `src/drones/droneid_followup*.hpp`, `src/scanner.*`.

## Done when

- New tests in `tests/test_drone_evidence_claims.cpp` cover: two same-group links far apart
  give two rows; one link seen at three neighbouring centres gives one row with a stable id
  across a new capture; a DroneID row plus two link clusters is not folded; one DroneID row
  plus one cluster is folded as today; withdrawal of one centre keeps the rest of the cluster.
- The existing tests in that file still pass (adjust ids where the key format changed).
- `ctest -R 'drone_evidence|droneid|scanner_coordinated'` passes and `rf_monitor_gui` and
  `coordinated_live_check` build.
- A short note of what changed and what is still ambiguous is given to the user.

## Changed after this brief was written (2026-11-12, afternoon)

A live run named a Mini 2 and showed the named row losing its name and group while the drone
was still there. `evidence_claims.hpp` was changed for that; build on it, do not undo it:

- `Packet::current_until` replaces the fixed 60 s packet age. `Store::expire()` extends it
  while a current link "supports" the packet, up to `packet_hold` (300 s) after the packet.
- `supports(packet, packet_5g, link_group, link_5g, row_group)` is the single rule for
  "this link can be the aircraft that sent this packet": same band unless the model's table
  group says otherwise; other band only when the model table gives the model that group.
- The fold in `detections()` uses `supports()` and now also folds a link in the other band,
  but only into a row that has no link in its own band.

Your clusters should plug into the same rule: a packet is held and folded by a link cluster
only when the choice is unambiguous. With two supporting clusters, neither holds the name.
Tests for the hold are at "A named drone keeps its name" in
`tests/test_drone_evidence_claims.cpp`.
