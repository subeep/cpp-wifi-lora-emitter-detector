# Upgraded plan — task list and progress

Tracks implementation of [upgradedplan.md](upgradedplan.md) (unified Drones page and coordinated three-band detection). Branch: `coordinated-mode`. Started 2 November 2026.

Scope of this round: **P1–P5**, i.e. up to "coordinated mode is added and working correctly". P6–P9 (continuous model inputs, training, prospective tests, promotion) are not started.

## Status at a glance

| Milestone | State | Evidence |
|---|---|---|
| P1 — Schedule/evidence contracts | Done | `src/coordinated_schedule.hpp`; `test_coordinated_schedule` |
| P2 — Single-page presentation | **Done** (incl. relationships, inspector, evidence export) | `src/coordinated_panel.*`, `src/drone_evidence.hpp`, Drones page |
| P3 — Offline schedule simulator | Done (fake-radio test of the real scan loop) | `test_scanner_coordinated` |
| P4 — Shared observation distribution | Done for existing modules | Visit lanes in `Scanner::run_coordinated` |
| P5 — Coordinated receiver mode | Done; verified live on the X310 | `BAND_COORDINATED`, `coordinated_live_check` |
| P6–P9 | Not started | — |

## Task list

### Baseline
- [x] Branch `coordinated-mode` from `main` (`fecc71a`).
- [x] Baseline build and full `ctest`: **71/71 passed** before any change.
- [x] X310 reachable: `addr=192.168.10.2`, serial 326CF02, HG image, **1 GbE** link, so 2.4/5 GHz visits run at 20 MSps.

### P1 — Schedule and observation contracts
- [x] `coord::Schedule` / `Slot` / `Visit` with explicit band budget, dwell, pre-roll, settle (`src/coordinated_schedule.hpp`).
- [x] Default profile from the plan: sub-GHz 2 × 512 ms, 2.4 GHz 4 × 512 ms, 5 GHz 6 × 512 ms; pre-roll 8 ms; settle 100 ms.
- [x] 2.4 GHz list = DroneID 2399.5 MHz **drone-only** visit + Wi-Fi ch 1–13; 5 GHz = existing 9 non-DFS channels.
- [x] Validation (rates, multiples of 500 kHz for sub-GHz, dwell 50 ms–10 s, 512 MiB per-visit bound, band/kind consistency).
- [x] Estimates: cycle = Σ(dwell + pre-roll + settle) = **7.44 s** for the default profile (6.144 s valid); average revisit per centre = N/n × cycle (sub-GHz 3.7 s, 2.4 GHz 26.0 s, 5 GHz 11.2 s). Inference time is never a cycle term.
- [x] Rotating cursors; configuration hash (FNV-1a) published with every run.
- [x] Coverage-aware freshness: receiving now / recent, frequency not observed since / not reacquired / insufficient coverage / expired — keyed on whether a later visit actually covered the entry's frequency.

### P4 — Shared observation distribution
- [x] One capture per visit, routed by visit kind:
  - sub-GHz → energy map + LoRa decode on every LoRa channel inside the usable passband, **derived from the same 5 MSps capture** (`src/channelize.hpp`, 2-stage Blackman FIR, delay-compensated so sample provenance scales exactly);
  - Wi-Fi channel → existing Wi-Fi PHY / security / registry path + drone & ML observers;
  - drone-only → drone & ML observers only (no Wi-Fi rows mislabelled as ch 1).
- [x] Separate bounded lanes (1 running + 1 queued each). A busy lane **drops and counts** the visit; acquisition never waits (`SerialLane::try_push`).
- [x] LoRa decode split out of `run_lora_listen_step` (`decode_lora_iq`) — the band-mode path is unchanged.
- [x] Native DJI/O4 backend: in coordinated mode the whole visit is offered as consecutive ≤160 ms messages with exact timestamps; any message the backend drops becomes a discontinuity (no bridged gaps). Band modes keep the single 160 ms prefix.
- [ ] Not done: shared immutable buffer referenced by several lanes (each visit currently has exactly one lane, so no copy is needed yet); cyclostationary shadow on drone-only visits.

### P5 — Coordinated receiver mode
- [x] New explicit mode `BAND_COORDINATED`; existing modes untouched and still selectable.
- [x] Loop: plan cycle → per visit tune (always retune) → settle → capture dwell + pre-roll → hand to lane → next.
- [x] Conflicts refused, not overridden: fixed Wi-Fi channel or LoRa lock ⇒ no acquisition + visible reason + clear buttons.
- [x] Schedule edits apply at the next cycle boundary; cursors reset; new hash.
- [x] Coverage ledger: per slot visits / valid / analysed / lane drops / valid time / analysed time / measured full sweep; overflows; empty captures; last 160 visits.
- [x] Same RX-stall reconnect as band modes.

### P2 — Single Drones page
- [x] Start / Stop coordinated detection (Stop restores the previous mode; starts/stops unified drone service only if it started it).
- [x] Schedule strip with the current slot highlighted, current centre/rate/usable span/time left, measured vs configured cycle.
- [x] Per-band coverage table; overflow/empty-capture warnings.
- [x] Schedule editor with live cycle/sweep preview and validation.
- [x] One combined entry table: DJI native, DJI fallback, Wi-Fi Remote ID, model (diagnostic only) — evidence levels kept separate, freshness per entry.
- [x] Module health; explicit limits (sub-GHz has no ML; LoRa visit truncation; classifier running ≠ qualified).
- [x] Mode also selectable in Settings; overview shows all three band maps in coordinated mode.
- [x] **Provisional relationships** between rows (`evidence::associate`): only (a) the same reported identifier across detectors — labelled as unauthenticated claims, not one aircraft — or (b) evidence from the same capture — labelled as the same samples, not independent confirmation. Frequency, product name, power or timing alone never relate rows. Rows are never merged.
- [x] **Conflicts** kept visible on both rows (e.g. same reported serial, different reported model).
- [x] Table marks `~` (related) and `!` (conflict); rows are selectable with stable keys.
- [x] **Inspector**: identity/evidence, source interval (exact capture # → visit, centre, validity, analysed), evidence device time, relationships, conflicts, coverage timeline for the entry's frequency (newest first, with the evidence's own visit marked), decoded/diagnostic fields, raw detector output. A selection that disappears is reported, not silently replaced.
- [x] **Evidence export** (asynchronous, never on the GUI thread; never overwrites): `data/drone_evidence/<UTC>-<source>/evidence.json` (schema `rfmon-drone-evidence` v1: entry, fields, raw output, relations, conflicts, covering visits, schedule hash) plus `iq.cf32_le` ±50 ms around the evidence when the capture is still retained, with its device-time anchor; metadata only otherwise, with the reason.
- [x] **Opt-in IQ retention** (`Scanner::set_iq_retention`): after the Wi-Fi lane finishes with a Wi-Fi/drone capture, its IQ is moved (not copied) into a bounded ring with a full copy of its capture record. Off by default; ~83 MB per retained 20 MSps visit.
- [x] Every coordinated visit now records its `capture_seq`, linking DJI (`capture N` in the source), Remote ID (frame provenance) and model results (`Result::capture`) to the exact visit.

### Tests
- [x] `test_coordinated_schedule` (32 checks) — schedule, cursors, hash, validation, freshness, channelizer gain/alias/alignment, **real SX1262 packet decodes after 5 MSps embedding and derivation**.
- [x] `test_scanner_coordinated` (29 checks) — production `Scanner::run()` with a fake radio: order with cursors, request parameters, per-capture markers prove no cross-band delivery, drone-only routing, LoRa on derived 865.985 MHz, conflict refusal and resume, lane drops with max acquisition gap ≈ 0, overflow accounting, schedule change, clean exit to band mode.
- [x] `test_coordinated_panel` — production Drones section rendered offscreen against a running coordinated `Scanner`; combined-entry evidence levels and freshness.
- [x] `test_coordinated_panel` extended (36 checks): relationships only from explicit evidence, conflicts on both rows, unique keys, capture parsing, export span/anchor/byte-exact IQ/no-overwrite/metadata-only, inspector rendering on a real row, opt-in retention default off.
- [x] `test_scanner_coordinated` extended: retention off by default, newest-N kept, retained IQ is the visit's own samples with an intact record, each maps to its visit, disabling releases it.
- [x] Full suite after changes: 74/74.
- [x] `tools/coordinated_live_check.cpp` (`build/coordinated_live_check`) — receive-only live check on the real receiver with the production drone add-on and ML worker, temporary data root, JSON summary; `--mode` for band-mode A/B.
- [x] Live X310 runs — see "Verification".

### Next (not started)
- [ ] Live packet evidence in coordinated mode: ESP32 Remote ID rig on a 2.4 GHz channel; a LoRa transmitter on an IN865 channel; a DJI aircraft if available.
- [ ] Decide the sub-GHz dwell (512 ms cuts slow SF11–12 LoRa packets) and whether 2.4 GHz needs more visits per cycle.
- [ ] P6: continuous same-view model inputs (the ML worker still samples sparse 8 ms intervals per visit).

## Verification

### Automated tests
| Run | Result |
|---|---|
| Baseline, before changes | 71/71 passed |
| Final, after all changes (full suite) | **74/74 passed** (71 existing + 3 new) |

`test_coordinated_panel` renders the production Drones section offscreen against a coordinated `Scanner` on a fake radio (Start/Stop, strip, coverage, entries, limits, conflict control) and checks `build_entries` keeps evidence levels separate with correct per-entry freshness.

### Live X310, receive-only — run 1 (coordinated, 120 s, drone add-on + ML worker)
Tool: `build/coordinated_live_check --seconds 120 --ml` (temporary data root; project stores untouched).

| Measure | Result |
|---|---|
| Cycles | 15; measured 7.71–7.73 s vs 7.44 s configured (≈ 23 ms host overhead per visit) |
| Visit order violations | 0 |
| Visits sub-GHz / 2.4 / 5 GHz | 30 / 60 / 90 — all valid, all analysed, **0 lane drops** |
| Overflows / empty captures | 0 / 0 |
| Observer calls / record mismatches | 150 / **0** (60 × 2.4 GHz incl. DroneID visits, 90 × 5 GHz, none sub-GHz) |
| FCS-valid Wi-Fi frames | 2.4 GHz 1,345; 5 GHz 21,801 |
| Wi-Fi security captures | 145 (= Wi-Fi channel visits; drone-only visits excluded) |
| ML worker | 450 processed, 0 dropped, 0 stale, 0 invalid |
| Native DJI/O4 | 600 messages; 468 M samples analysed, 1.09 G dropped at its 1-deep queue (backend slower than real time, as known); 147 discontinuities; 1,462 O4 windows; 0 candidates; 0 packets; 0 errors |
| LoRa | 120 derived-channel capture records (4 per sub-GHz visit); 0 packets on air |
| Peak memory | 582 MB |

Finding fixed after run 1: the "full sweep" figure. With 14 centres at 4 per cycle the cursor wraps every 3 or 4 cycles, so both the configured value (worst case 4 cycles) and the measured wrap-to-wrap value were misleading. Both now report the **average revisit per centre** (N/n × cycle; measured from the first wrap).

### Live X310 — run 2 (coordinated, final code) and A/B against band modes
Same tool and conditions, back to back, 120 s each (sub-GHz band mode 60 s). Different runs see different ambient traffic, so counts are indicative.

| Measure | Wi-Fi both bands (band mode) | Coordinated (run 2) |
|---|---|---|
| Wi-Fi time captured | 107 × 1 s | 145 × 0.52 s (≈ 75 s) + 32 sub-GHz visits |
| FCS-valid frames 2.4 / 5 GHz | 1,923 / 20,016 | 876 / 24,274 |
| Energy groups 2.4 / 5 GHz | 25 / 13 | 28 / 16 |
| Observer calls (drones/ML) | 112 | 151 |
| ML worker processed / dropped / stale | 93 / 77 / 0 | 444 / 2 / 0 |
| Native DJI/O4 samples analysed | 166 M | 480 M |
| Overflows / mismatches | 0 / 0 | 0 / 0 |
| Peak memory | 1,126 MB | 521 MB |

Coordinated run 2 detail: 15 cycles at 7.70–7.74 s; 0 lane drops; measured average revisit per centre sub-GHz 3.86 s (configured 3.72), 2.4 GHz 27.0 s (26.0), 5 GHz 11.6 s (11.2) — the difference is the ~4% host overhead per cycle.

Sub-GHz band mode (60 s): 4 LoRa listen records (one channel per ~10.5 s cycle), 0 energy groups, 0 packets. Coordinated mode: 4 channels per visit (122 records in 120 s), also 0 groups/packets — no LoRa transmitter was active. LoRa decoding on derived channels is therefore proven by the fixture tests, not yet by a live packet.

What this shows: coordinated mode runs all three bands from one radio with zero overflows, zero dropped analysis and no cross-band data; Wi-Fi decoding and the security path keep working; drones/ML receive more usable visits than in band mode. 2.4 GHz gets less dwell than the Wi-Fi-only mode (by design of the default profile).

### Live X310 — run 3 (P2 evidence path, coordinated, 60 s, ML worker, `--retain 4 --export-dir`)
| Measure | Result |
|---|---|
| Cycles / overflows / empty / mismatches / lane drops | 7 / 0 / 0 / 0 / 0 |
| ML processed / dropped / stale | 213 / 1 / 0 |
| Retained captures at end | 4 (bounded as configured) |
| Live evidence export | latest model result → capture #72 (2.4 GHz ch 1, valid, analysed) retained; `iq.cf32_le` 1,160,000 finite samples = 50 ms before the model interval to the capture end (the interval was near the end), manifest consistent with the capture (10.4 M samples at 20 MSps, device time valid, no overflows) |
| Peak memory | 852 MB (includes 4 retained captures) |

### Not verified
- The GUI section on the real desktop with the real receiver (no desktop automation available here); it is verified offscreen with the production panel code and live through the same `Scanner` API. Open **Drones → Start coordinated detection** to see it. The Export button itself was not clicked in a test; the export function it calls is tested offline and live.
- A live DJI DroneID packet, a live Remote ID broadcast, or a live LoRa packet in coordinated mode — none were on air (ESP32 Remote ID rig not connected).

## Known limits (unchanged by this work)

- One receiver, time-shared: a transmission entirely between visits is missed; no schedule guarantees detection.
- 1 GbE X310: 20 MSps per visit, so 2.4 GHz needs several visits per sweep.
- Sub-GHz has no drone model; 512 ms sub-GHz visits can cut slow LoRa (SF11–12) packets.
- No presence model is qualified: the Drones page shows no "no drone" or confirmed-drone decision.
