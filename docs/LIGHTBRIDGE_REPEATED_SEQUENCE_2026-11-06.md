# Repeating sequence in Phantom 4 recordings

A four-symbol waveform found in PHA_1000_01.dat recurs at predictable relative sample offsets in two other Phantom recordings. It provides a reproducible timing anchor for aligning bursts. The RF transport, position of the packet boundary and telemetry fields remain unresolved.

## Discovery and replay

The same-band probe compared 10 pairs of separated prefix trains across eight source files. Each pair uses the same original 20 ms excerpt and frequency region. Clipped first trains were excluded. Normalized complex matched filtering searched nearby offsets and the rest of the second train. Templates at offsets −3 through +3 symbols were explored. One pair stood out with four broadband symbols scoring 0.63–0.67 and independently peaking at the same +14 sample offset at 11.2 MSps. The first train starts at symbol 14 and the second at symbol 166 in region 2, parent start sample 50,772,032. Other pairs had much weaker peaks at predicted offsets.

The full-capture search froze the first and fourth useful-symbol templates, separated by 3,456 samples at 11.2 MSps. It used normalized correlation magnitude, with a 0.35 threshold on both at the same timing hypothesis. The search read each two-second file in overlapping blocks and assigned each peak to its source block. Filter startup and end effects limit coverage at file endpoints. Peaks within 1,024 analysis samples are suppressed locally. Counts refer to candidate sequence anchors rather than unique packets. The middle two symbols were checked within three analysis samples of their predicted positions. They were seen during discovery but did not participate in the full-capture search. The maximum normalized circular correlation between any two different reference symbols was 0.183.

| Recording and dataset condition | Two-anchor hits | Both middle symbols also above 0.35 |
|---|---:|---:|
| PHA_1000_01, Wi-Fi, source | 48 | 30 |
| PHA_1000_02, Wi-Fi | 35 | 23 |
| PHA_0000_01, clean | 39 | 23 |
| DIS_1000_01, Wi-Fi, Parrot Disco | 0 | 0 |

Of the source recording's 48 hits, 46 lie outside the discovery excerpt. The two additional Phantom files yielded 46 four-symbol matches combined. The comparison recording's best joint score was 0.128. Thresholds and the frequency channel are exploratory. Filenames can share hardware and sessions. One comparison cannot measure a field false-alarm rate or uniquely attribute the waveform to Lightbridge. The observations support prediction of a symbol-level timing anchor. Locating the header and recovering independent checksums are separate steps.

## Reproduction and evidence

Use the repository Python environment. Run these modules in order, passing new output filenames.

    python -m tools.lightbridge_same_band TRAINS_JSON PAIRS_JSON
    python -m tools.lightbridge_sequence_replay TRAINS_JSON REPLAY_JSONL OTHER_IQ_FILES...
    python -m tools.lightbridge_sequence_check TRAINS_JSON REPLAY_JSONL CHECK_JSON

The source selection is frozen to the discovery recording and interval. Results are under data/validation/lightbridge-wide-grid-v1 in same_band.json, sequence_replay.jsonl and sequence_check.json. The latter two files retain exact capture paths and hit sample offsets. A synthetic test recovered a phase-rotated template pair at the injected offset and rejected deliberately incorrect spacing.

The next decoding experiment can use the confirmed anchors to align original wideband IQ and compare carrier values before and after the sequence across repetitions. Fixed carriers or header positions should reproduce in separate files before testing payload mappings.
