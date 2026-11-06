# DJI Lightbridge telemetry — parallel engineering handoff

**Status as of 6 November 2026.** We have a reproducible waveform anchor in Phantom 4 recordings and a working offline symbol-timing experiment. A verified Lightbridge IQ-to-telemetry decoder remains the goal. No recovered payload has passed an independently established packet checksum. Treat every proposed bit mapping and telemetry field as experimental.

## What the experiments established

| Experiment | Result and interpretation |
|---|---|
| Full-recording search | Read 50 Phantom 4 files (48 GB, about 100 seconds). Replayed 200 selected 20 ms excerpts through the existing native DJI DroneID worker. It returned zero valid DroneID packets in those excerpts. This tests compatibility with that decoder only. |
| Symbol timing | A candidate useful symbol of 91.43 µs and prefix of 11.43 µs gives a 102.86 µs period and 10.9375 kHz carrier spacing. Found 96 candidate trains in 40 Phantom files. Overlapping frequency views can count the same burst more than once. |
| Wideband grid | Reconstructed the original IQ at 44.8 MSps using a 4096-point FFT and 512-sample prefix. Prefix timing transferred across recordings. Integer carrier alignment and a common pilot map remain unresolved. |
| Repeating anchor | Found four distinct consecutive symbols in PHA_1000_01. Froze symbols 0 and 3 for search and checked symbols 1 and 2 at predicted times. All four matched at 23 places in PHA_1000_02 and 23 in PHA_0000_01. One Wi-Fi-labelled Parrot Disco comparison yielded zero matches. This limited comparison cannot establish protocol specificity. |
| Structure around the anchor | Compared 846 adjacent-carrier pairs. Cross-file agreement after the anchor fell from 0.143 at offsets +4…+7 to 0.0465 at +9…+12. Symbol zero is the reference by construction. The four anchor symbols were selected for recurrence. |
| Retiming | On the same 46 anchors, searching ±2048 samples at 44.8 MSps raised median prefix coherence at offsets +9…+12 from 0.474 to 0.985. Positions passing timing controls increased from 81/184 to 149/184. Carrier agreement moved only from 0.0465 to 0.0466. Changing data is plausible. A different transmission, pilot pattern or channel can also explain the result. |

**Keep the two protocols separate.** The existing native worker implements DJI DroneID and O4 candidate analysis. Its known ZC roots, turbo parameters, CRCs and telemetry layouts are hypotheses at most for Lightbridge. An aircraft label identifies recording context. It cannot establish the transmitting device, a burst's protocol, or the meaning of decoded bytes.

## Transfer and reproduce on the second system

1. Copy the working-tree versions of the files below. Several research files and generated results may be absent from a Git checkout. Record the originating commit and SHA-256 hashes of copied scripts, IQ and artifacts. Work in a separate checkout and output directory.
2. Copy these three DroneDetect V2 recordings read-only, retaining directory structure. The first supplied the original template; the latter two contain the 46 comparison anchors.

       WIFI/PHA_ON/PHA_1000_01.dat
       WIFI/PHA_ON/PHA_1000_02.dat
       CLEAN/PHA_ON/PHA_0000_01.dat

   For the existing interference comparison also copy WIFI/DIS_ON/DIS_1000_01.dat. Source format is little-endian interleaved float32 I/Q (complex64), nominal 60 MSps centred at 2.4375 GHz. The dataset author reports 28 MHz capture bandwidth. A separate local model reader declares 34 MHz; use the measured passband for conclusions.
3. Copy tools/lightbridge_*.py, tests/test_lightbridge*.py, docs/LIGHTBRIDGE*.md and the following workspace artifacts. The first three result directories contain the lineage from capture to timing correction.

       data/validation/lightbridge-full-2026-11-06-b/trains.json
       data/validation/lightbridge-wide-grid-v1/sequence_replay.jsonl
       data/validation/lightbridge-wide-grid-v1/sequence_check.json
       data/validation/lightbridge-anchor-context-2026-11-06/result.json
       data/validation/lightbridge-anchor-context-2026-11-06/carrier_evidence.npz
       data/validation/lightbridge-anchor-context-2026-11-06/retiming_controls.json

4. Use Python with NumPy and SciPy. Run modules from the repository root. Source paths in the artifacts point to /home/sudeep/Pictures/DroneDetect_V2. Either provide that path on the second machine or rewrite the exact path prefix in **working copies** of the JSON/JSONL files. Preserve filenames, sample offsets, ordering and the original artifacts. Sample offsets in sequence_check.json count complex samples at the **original 60 MSps**. Retiming shifts count samples at **44.8 MSps**.
5. Reproduce the latest result before extending it:

       python -m tools.lightbridge_retime data/validation/lightbridge-anchor-context-2026-11-06 retiming_replica.json

   Confirm 23 anchors from each comparison file, the same 846 carrier pairs and the rounded retiming results above. The script also checks fixed-grid agreement against the stored baseline. No live USRP connection or model training is required.

## Parallel assignment for the other coding agent

**Own the offline modulation and bit-mapping investigation.** The companion workstream can own burst localization, additional captures and pilot/frame-boundary recovery. Consume the frozen anchors and timing results through files. Keep experiments under a distinct module/output name so both systems can exchange results without merging live-worker changes.

1. **Build a symbol manifest.** For each anchor and relative symbol retain the recording hash, source offset, actual FFT start, frequency grid, signed timing shift, prefix coherence, competing-control score and energy support. Reuse the resampling and frequency-offset convention in tools/lightbridge_retime.py. Begin with the broad-search timing gate used in the report: prefix coherence above 0.7 and an advantage above 0.25 over the competing control. Split runs at abrupt timing or occupied-band changes. Predeclare and record any new cutoffs.
2. **Test modulation on contiguous runs.** Use the repeated symbols to estimate channel/phase only where justified. Compare differential BPSK, QPSK and higher-order alternatives with residual error and constellation distributions. Include a wrong-timing, shifted-carrier or shuffled-symbol control. Report how many symbols and bins survive each gate. A four-cluster plot alone cannot validate QPSK bits.
3. **Freeze and transfer one hypothesis.** Develop on PHA_1000_02, freeze the chosen transformations and bit ordering, then replay unchanged on PHA_0000_01. Both files have already been inspected for waveform development. Describe this as file-to-file replication; reserve fresh recordings for final qualification. Log rotations, conjugation, carrier ordering, excluded bins and every framing or coding hypothesis tried.
4. **Require packet evidence.** Save candidate bits and erasure/soft scores with their source coordinates. Test repeatability, plausible lengths and integrity checks with negative controls. A CRC or layout selected by searching the same bytes needs independent confirmation on other frames. Verify field meanings separately with synchronized controller telemetry or known test values before reporting GPS, serial, altitude or identity.
5. **Return a reproducible bundle.** Include a runnable command, script versions, manifest, candidate symbol/bit artifacts, metrics split by recording, rejected hypotheses and a short decision report. If channel alignment prevents a defensible modulation decision, return the specific problematic runs and frequency ambiguities so the companion workstream can resolve them.

**Integration gate.** Keep the experimental branch offline until it recovers repeatable packet bytes with independent integrity evidence. A subsequent native worker change should consume the existing shared IQ stream, preserve original sample/time provenance, reset on discontinuities, and expose waveform candidates separately from validated packets and parsed telemetry.

## Source trail

- [Full scan](LIGHTBRIDGE_FULL_SCAN_2026-11-06.md)
- [Symbol trains](LIGHTBRIDGE_SYMBOL_TRAINS_2026-11-06.md)
- [Wideband grid](LIGHTBRIDGE_WIDEBAND_GRID_2026-11-06.md)
- [Repeated sequence](LIGHTBRIDGE_REPEATED_SEQUENCE_2026-11-06.md)
- [Anchor context](LIGHTBRIDGE_ANCHOR_CONTEXT_2026-11-06.md)
- [Retiming experiment](LIGHTBRIDGE_RETIMING_2026-11-06.md)
