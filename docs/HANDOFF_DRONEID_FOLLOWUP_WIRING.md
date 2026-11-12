# Handoff: connect the DroneID follow-up visits in `src/main.cpp`

12 November 2026. For the agent that owns `src/main.cpp` and `src/coordinated_panel.cpp`. Nothing here is committed.

## What exists

When the symbol-timing detector reports a drone link in a band, the coordinated scanner can now make extra, longer visits to that band's DroneID channels until a DroneID packet is decoded or the request times out. The code is in place and tested, but it is **not connected in the app**: without the lines below the scanner behaves exactly as before.

- Queue and rules: `src/drones/droneid_followup.hpp` (`rfmon::droneid_followup::Followup`).
- Wiring helper: `src/drones/droneid_followup_wiring.hpp` (`connect()`).
- Scanner: `Scanner::set_droneid_followup()`; counters in `CoordinatedStatus` (`followup_visits`, `followup_valid`, `followup_analysed`, `followup_acquired_s`).
- Decoder: follow-up captures are decoded whole and ahead of waiting ordinary captures (`Detector::submit(..., priority)`; `Snapshot::priority_chunks`, `displaced_chunks`).

## Lines to add in `src/main.cpp`

1. Include:
   ```cpp
   #include "drones/droneid_followup_wiring.hpp"
   ```
2. After `symbol_timing` and `drones` exist, and **before** the `Scanner` is constructed (so the queue is destroyed after the scanner):
   ```cpp
   rfmon::droneid_followup::Followup droneid_followup;
   rfmon::droneid_followup::connect(droneid_followup, symbol_timing, drones);
   ```
3. After each `Scanner scanner(...)` construction:
   ```cpp
   scanner.set_droneid_followup(&droneid_followup);
   ```

`connect()` reads link sightings from `symbol_timing.snapshot()` and decoded packets from `drones.native.snapshot()` once per scanner cycle, on the scanner thread, and sets `drones.native_priority`.

## Defaults (`Config`)

| Setting | Default | Meaning |
|---|---|---|
| `dwell_s` | 1.0 s | valid time per follow-up visit |
| `max_share` | 25% | most scanner time follow-up visits may take |
| `hold_s` | 30 s | request ends this long after the last link sighting |
| `max_request_s` | 120 s | request gives up after this long |
| `cooldown_s` | 60 s | after a packet or a give-up, sightings do not reopen the request |

A request closes on any DroneID packet with an identity, readable or encrypted. `set_enabled(false)` turns the feature off.

## Worth showing in the Drones view

`droneid_followup.status()` gives, per band: whether a request is open, the link group that opened it, the last outcome ("packet decoded", "link no longer seen", "timed out"), and counts of requests, visits and packets. One line per band is enough, for example "2.4 GHz: looking for DroneID (30kHz link), 6 visits".

## Not verified

Not run in the app or against the X310. Tested with the fake radio (`scanner_coordinated`), the queue's unit test (`droneid_followup`) and the decoder transport test (`unified_drone_addon`). ThreadSanitizer was not run.
