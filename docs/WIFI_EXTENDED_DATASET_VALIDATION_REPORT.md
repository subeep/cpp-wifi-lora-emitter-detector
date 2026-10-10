# Extended SDA dataset validation

10 October 2026. The user's expanded dataset permission now includes RFUAV, DRFF-R2 and analog FPV data on SDA. Mini 2 live testing remains postponed. This work extends offline ingestion and validation; production DSP thresholds, GUI acceptance, radio settings, scan schedules and the running monitor were not changed. No ML or Remote ID work was performed.

## Read-only source handling

SDA was present but unmounted. It was mounted using `udisksctl` with `ro`, and `findmnt` confirmed a read-only NTFS mount at `/media/sudeep/Storage`. The new importer refuses writable source filesystems and outputs within the source tree. DRFF input files use read-only/no-atime handles; RFUAV uses `unrar p` to stdout, never extraction into the dataset directory. Source code included with datasets was inspected as documentation, not executed.

`prepare_extended_link_references.py` reads an explicit selection of six DRFF files and three RFUAV archive members, retaining only the first 100 ms of each. All nine source file-stat tuples were unchanged at completion; all nine derived IQ SHA-256 values were reverified. Working copies occupy approximately 720 MB in the workspace. No full-file/archive checksum claim is made. RFUAV IQ decompression stops after the requested prefix, so the full IQ member CRC is **not** verified.

DRFF MATLAB v7.3 I/Q vectors are read with an isolated h5py dependency under `/tmp/cyclo-hdf5-reader`. Embedded `Fs` and `CenterFrequence`/`CenterFreq` supply rates and tuning; missing or conflicting frequency metadata is rejected. RFUAV uses the archived XML's sample rate, centre and complex-float declaration. Its author's [pinned raw-data reader](https://github.com/kitoweeknd/RFUAV/blob/df72beaef99928ed79b8f49163a8be569741c417/graphic/RawDataProcessor.py), checked with Firecrawl, pairs alternating float32 values as I and Q. Our little-endian interpretation is recorded explicitly; byte order is not separately declared in the XML. XML scale factors are retained as metadata, not applied or treated as amplitude calibration.

[Source/working-copy manifest](../data/wifi_drone_analysis/extended_references_2026-10-10/manifest.json).

## Labels are audited before use

Two concrete conflicts prevent treating these publisher labels as validation truth:

- DRFF `indoor_environment.mat` contains target `mini5PRO_1` and state `hover`. Its environment filename cannot certify a drone-free negative. This could be stale metadata or a labelling error; the importer does not decide which.
- RFUAV `FRSKY X14.rar` contains XML naming `FLYSKY X14`. Neither name is silently corrected or promoted to a protocol label.

DRFF's outdoor environment file has no target field. Its folder is useful collection context, but independently verified drone absence is still missing. All imported records have `link_family_truth: null`, `negative_ground_truth_eligible: false` and `release_validation_eligible: false`. DRFF records share one conservative unknown-unit/session split group; RFUAV records group by source archive. These groups prevent casual splitting of related derivatives and do not establish independence.

## Replay results

Nine intervals, **0.9 seconds**, **1,566 native windows**. Existing thresholds were frozen. Replay used bounded filter refinement and overlap sufficient for the symbol plus surrounding probes.

| Recording context | Capture centre | Distinct marker candidates | Span plus boundary contrast | Apparent recurrence |
|---|---:|---:|---:|---|
| DRFF Mavic 3 Classic, ascend | 5.745 GHz | 5 up-18-MHz | 5 | Approximately 20 ms |
| RFUAV Avata 2, selected 20 MHz context | 5.765 GHz | 2 down-9-MHz | 1 | Not assigned a 20 ms up-chirp cadence |
| DRFF Mavic 3 Classic / Mini 4 Pro Wi-Fi mixtures | 2.437 GHz | 0 | Not evaluated | Unavailable |
| DRFF Mini 4 Pro, ascend | 5.745 GHz | 0 | Not evaluated | Unavailable |
| RFUAV Mini 4 Pro, selected 20 MHz context | 2.450 GHz | 0 | Not evaluated | Unavailable |
| DRFF indoor / outdoor environment | 2.437 / 5.745 GHz | 0 | Not evaluated | Unavailable |
| RFUAV archive with X14 name conflict | 2.440 GHz | 0 | Not evaluated | Unavailable |

Mavic 3 Classic supplied five matches without needing filter refinement; both raw and refined views support the same five markers. Avata 2's three refined evaluations collapse to two distinct down-chirp markers, only one of which passes the fuller span check. Duplicate views are not counted as independent observations. No-match intervals do not establish absence of drone activity or lack of the waveform in the rest of a file.

This extends received-marker evidence to a new publisher collection at 5.8 GHz and adds received down-chirp examples. It does **not** distinguish OcuSync generations, prove protocol uniqueness, decode a link or identify an aircraft model. All named-family acceptance remains disabled. No accuracy or false-positive rate is estimated from these short, development-exposed intervals.

[Marker replay](../data/wifi_drone_analysis/extended_marker_replay_2026-10-10/summary.json), [symbol diagnostics](../data/wifi_drone_analysis/extended_symbol_validation_2026-10-10/summary.json), [source and result verification](../data/wifi_drone_analysis/extended_symbol_validation_2026-10-10/verification.json).

## Other data and remaining work

Analog FPV archive listings and sweep metadata were inspected, but its IQ was not imported. The metadata describes separate frequency-tuned captures, with gaps between blocks; concatenating them would fabricate continuity. Its format and block boundaries need a separate importer. The much larger archives were not unpacked wholesale.

Completed software milestones:

1. Bounded, read-only DRFF and RFUAV adapters with explicit source provenance.
2. Label-conflict checks, unverified-negative handling and conservative source grouping.
3. Fixed-policy replay and full-span evaluation on nine new real recording contexts.
4. All **24 standalone tests passed**, including new checks for metadata conflicts, I/Q layout and bounds, malformed XML, short/oversized/timed-out subprocess reads and source/output isolation. Syntax and whitespace checks passed.

Next work is direct prefix/symbol-structure verification using these new examples alongside Leuven/Tampere, then controlled distortion/mixture tests and a properly labelled competing-signal corpus. Independent held sessions/units, trustworthy mode-level truth, receiver qualification and full-search false-positive calibration remain required before accepting a named link. Live Mini 2 sessions and USRP live integration remain deferred.
