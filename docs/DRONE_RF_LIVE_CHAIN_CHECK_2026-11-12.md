# Live check of the drone chain on the X310

12 November 2026. Receive-only. No drone was available, so this checks behaviour without a drone and the follow-up visits' mechanics; it cannot show a named drone row. Nothing is committed.

## Method

`build/coordinated_live_check` drives the production scanner in coordinated mode with a temporary data root (`data/wifi_master` untouched). The tool now attaches the same chain as the GUI: link detector, DroneID decoder, follow-up queue, evidence store, merged rows and detection log. Two flags were added: `--no-followup`, and `--simulate-link MHZ`, which feeds a made-up link sighting to the follow-up queue only (never to the rows) from 15 s to 75 s.

X310 on 1 GbE, 20 MSps, default schedule. Results: `data/drone_ml/2026-11-12/live_check_01/`.

## Results

| Run | Length | Wi-Fi-band captures timed | Captures with a link group | Rows shown | Follow-up visits | Mean cycle |
|---|---|---|---|---|---|---|
| 1. Plain | 150 s | 186 | 0 | none | 0 | 7.8 s |
| 2. Simulated link at 2437 MHz | 130 s | 127 | 7 (`32.8kHz`) | "Link only (32.8kHz)" | 24 (20 valid) | 10.0 s while active |
| 3. Plain, repeat | 90 s | 109 | 0 | none | 0 | 7.7 s |

1. **No drone, no rows (runs 1 and 3).** 295 captures, no link group, no DroneID packet, the view state stays "No drone detected", and the log file is not even created. Scan timing is unchanged: 7.7 s per cycle against 7.44 s configured, no lane drops, no link-detector drops, 115 ms worst processing time per capture.
2. **Follow-up visits work on the radio (run 2).** The request opened, the scanner made 24 extra visits rotating through 2399.5 to 2459.5 MHz, the decoder took them ahead of ordinary captures (136 priority chunks, 100 ordinary chunks displaced), and the cycle grew from 7.7 s to about 10.0 s, a 23% share against the 25% limit. Ordinary visits kept their order and counts.
3. **Problem: 1 s captures are less reliable on this link.** In run 2, 4 of 24 follow-up visits were not valid, there were 4 overflows and 8 empty captures (runs 1 and 3: one overflow, none empty), and UHD printed dropped-packet marks. The 1 GbE link at 20 MSps is near its limit for a continuous 1 s capture.
4. **Unexplained: a `32.8kHz` link group appeared in run 2 only.** Seven captures between 2401 and 2419 MHz were flagged (four at 2401, two at 2413.5, one at 2418.5), starting with the very first capture of the run, before any follow-up visit, so the follow-up visits did not cause it. Up to 75% of a capture's blocks were flagged. It was absent five minutes earlier and one minute later. Either something nearby transmits intermittently with that symbol timing, or this is a false link from a non-drone signal that the recorded no-drone data (0 of 263) did not contain. Not resolved: no IQ was kept.
5. **The decoder covers under half the air.** It decoded 41% of the samples offered in each run and refused the rest (queue full). This is the known speed limit, and it is why follow-up captures need their priority.
6. **Row behaviour in run 2.** The link row appeared once, stayed while sightings continued, and was withdrawn when a later capture of the same centre showed no link, as designed. The log wrote 4 lines in total.

## Not covered

A real drone: link group on a live drone, DroneID decode during a follow-up visit, the request closing on a packet, and a named row. The desktop GUI itself was not driven; the rows come from the same code run inside the check tool.

## Later the same day: faster decoder, first named drone live

Correction to item 4 above: the 32.8 kHz sighting was the user's Phantom 4 (a true detection).

The decoder worker now decodes the windows of each message on up to 12 threads (same output as
before on 16 offline inputs; `data/drone_ml/2026-11-12/decoder_speed_01/`). A 120 s live check
with it (`data/drone_ml/2026-11-12/live_check_02/`):

| | Earlier run (150 s) | Faster decoder (120 s) |
|---|---|---|
| Offered samples decoded | 40% | 98% |
| Chunks dropped because the queue was full | 465 | 0 |
| Capture-to-result delay, last message | 4.2 s | 0.16 s |
| Overflows / empty captures | 1 / 0 | 0 / 0 |

A drone was on the air during this run and the whole chain worked on it for the first time:
15 kHz link seen at 2401 MHz, follow-up visits opened, DroneID decoded on the third visit
18 s after the first sighting, row named "Mini 2" (product code 63, version 2 record, serial
self-reported). Packets were decoded in both bands (2.4 GHz and 5776.5 MHz). 11 follow-up
visits, all valid.

Seen and not fixed here:
- The named row lost and regained its group four times and link-only rows came and went in the
  other band, as the link moved between 2.4 and 5.8 GHz. The named row expired after 60 s
  without a new packet while the link was still there, and the drone went back to "Link only".
- Mini 2 is listed as "expected 15 kHz" in the model table; this run agrees but is one
  observation of one aircraft, so the table was not changed.
