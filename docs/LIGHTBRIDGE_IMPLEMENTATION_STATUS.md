# Lightbridge telemetry implementation investigation

5 November 2026. Requested target: passive IQ-to-telemetry decoding in the
existing native DJI worker. Status: not implemented; RF protocol unresolved.

## Verified integration boundary

`tools/drone_native_worker.cpp` currently runs the vendored DJI DroneID OFDM
decoder and a separate O4 candidate detector. Successful DroneID outer CRCs
lead to envelope inspection and telemetry parsing. Neither a product-name map
nor successful DroneID parsing establishes a Lightbridge transport.

The recent application changes already include native packets in the unified
table and Bluetooth Remote ID. These paths should remain independent of any
future Lightbridge decoder. No existing RF decoder was changed in this investigation.

## Sources examined

- https://github.com/proto17/dji_droneid/issues/28
  Direct discussion by decoder developers/users distinguishes narrow controller
  activity from DroneID. The discussion leaves Lightbridge pilot allocation,
  randomization/interleaving, coding, CRC and spatial-stream handling unresolved.
  It does not provide a reproducible Lightbridge decoding implementation. Its
  WiMAX relationship is a hypothesis, not an implementable specification.
- https://pmc.ncbi.nlm.nih.gov/articles/PMC10490811/
  The described implementation covers Wi-Fi identification mechanisms; its
  Lightbridge background discussion supplies no complete Lightbridge PHY decoder.

Search scope was limited public searches and these sources. This does not prove
that no implementation or specification exists elsewhere.

## Available local research inputs

`lightbridge_input_inventory.json` inventories Phantom 4 and Inspire 2 paths from
the existing model inspection CSV and verifies current file existence/size.
Dataset originals were not altered. These are candidate waveform inputs, not
packet-level ground truth and not proof that every sample is Lightbridge.
The local model reader describes DroneDetect V2 as 60 MSps complex64, centered
at 2.4375 GHz; these metadata need confirmation against original dataset
documentation before using physical timing or decoding hypotheses.

## Required implementation evidence

1. Verify raw format, sample rate, frequency coverage and continuity of candidate
   recordings. Establish a reproducible burst/frame view and separate uplink
   from downlink hypotheses.
2. Recover synchronization and pilot mapping, modulation, coding/interleaving,
   scrambling, length/framing and checksum rules. Record evidence for each
   parameter; do not transplant the DroneID ZC/turbo pipeline without support.
3. Recover packet bytes with repeatable independent integrity checks. Identify
   telemetry message types, scaling and validity flags using independent data.
   Aircraft/session labels alone cannot validate GPS, altitude or serial fields.
4. Implement a bounded native module on the shared IQ stream. Reset state on
   gaps/retunes, retain original timestamps and sample spans, and never turn a
   synchronization candidate into decoded telemetry.
5. Replay held-out positive captures plus other RF, corrupted frames and noise;
   then integrate verified packet results into the existing native packet UI.

An SDK/serial DUML parser by itself would not meet the requested USRP IQ-to-
Lightbridge functionality. A synthetic encoder/decoder round trip alone would
not establish compatibility with a real Lightbridge transmitter either.

No training, RF transmission, hardware access or model promotion was performed.
