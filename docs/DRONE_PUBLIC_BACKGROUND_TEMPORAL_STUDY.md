# Public background data and temporal model development

23 October 2026 (Asia/Kolkata). Additive offline work only. Acquisition, scanning,
GUI inference, existing model weights and acceptance policy were not changed in
this increment. No hardware was opened. No real-data model training was started.

## Implemented

- `tools.drone_ml.public_background`: verifies the published NIST HDF5 size and
  SHA-256, the metadata CSV SHA-256, and the 180-capture int16 IQ layout. Rejects
  incomplete files, external/soft links and virtual datasets. A `.part` file is
  promoted without overwriting a complete file only after verification. Inventory
  output is separate from originals.
- Capture timestamps, publisher context and conservative collection groups are
  retained. Inventory rows have no verified absence label and are not training
  eligible. Simultaneous indoor/outdoor captures must share a partition; 180
  captures are not 180 independent sessions. The existing live-only reviewed
  presence trainer is unchanged; this inventory cannot be passed off as its
  training corpus.
- `tools.drone_ml.temporal_audit`: streams bounded blocks from explicit CF32,
  signed-int16 IQ or verified NIST HDF5 references. Only train/development
  partitions are allowed; reserved or unspecified partitions fail before IQ is
  opened. Source hashes and unchanged file-stat tuples are checked.
- Computes native-rate 1024-point spectra in approximately 1 ms bins, full-band
  energy and up to five fixed frequency views. These diagnostics retain the
  16 MHz interior of a nominal 20 MSps view. This is not the production STFT or a
  replacement for its trained preprocessing.
- Reports energy islands and context coverage at 8, 16, 32, 64, 128, 256, 512 and
  1000 ms, with relative thresholds of 3, 6 and 10 dB above the recording's 20th
  energy percentile. That percentile is NOT a calibrated noise floor. Islands
  are NOT packets, chirps, drone detections or identity labels. A high-duty-cycle
  signal may have zero relative-energy islands.
- No partial block is invented at the end. The report gives exact analysed
  sample counts and excluded trailing samples. Requested contexts longer than
  the analysed interval have null coverage, rather than fabricated padding.

## First local diagnostic run

Manifest: `data/drone_ml/2026-10-23/temporal_research_01/sources.json`.
Results: `data/drone_ml/2026-10-23/temporal_research_01/report.json`.

Five existing development/training recordings, approximately 3.198 seconds in
total. The two Leuven inputs are previously derived 100 ms prefixes. Tampere and
the user's two local captures contribute approximately their first second. No
reserved final-test files were read. All five source hashes matched and source
file-stat tuples were preserved. The local Mini 2 recordings have unresolved
sample-continuity provenance; timing interpretation remains exploratory.

The table selects the highest mean-power fixed view and the +6 dB relative
threshold. It summarizes energy activity, NOT classifier performance.

| Recording context | Analysed seconds | Relative energy activity | Energy islands | 8 ms contexts containing activity |
|---|---:|---:|---:|---:|
| Leuven Mini 2 video context | 0.09935 | 41.41% | 20 | 100% |
| Leuven Mini 2 RC context | 0.09935 | 46.46% | 20 | 100% |
| Tampere Mavic Pro context | 0.99940 | 0% above this relative threshold | 0 | 0% |
| Local Mini 2 linked context | 0.99975 | 59.69% | 142 | 99.4% |
| Local target-off context, other emitters unknown | 0.99975 | 76.01% | 109 | 99.4% |

Both local contexts reach 100% activity coverage at 16 ms and longer eligible
contexts. This does not prove drone absence/presence in either case. It shows
why longer observation plus a generic activity threshold is insufficient to
separate these two recordings. Overlapping diagnostic windows are correlated.

The Mavic Pro result must not become a negative label: a recording-relative
threshold can miss sustained activity, weak bursts or the relevant frequency
view. The short Leuven prefixes cannot evaluate 128 ms or longer contexts.
This pilot does not select an optimal temporal model or claim reduced false
alarms. A chirp-sensitive/phase-sensitive study requires different features and
finer timing than these energy bins.

## Public data acquisition in progress

First source: [NIST Wi-Fi and Bluetooth I/Q recordings](https://doi.org/10.18434/mds2-2731),
version 1.3.2. Isolated download directory:
`data/drone_ml/public_sources/nist_mds2_2731_v1_3_2/`.

The first file, `2_4ghz_indoor.h5`, is 21,600,072,448 bytes. It contains 180
one-second captures at 30 MSps centered on 2437 MHz. Published SHA-256:
`73948e960d0c222c685875cbbeb1556dce1ab04dd500b3cc00dbfb46253b14f3`.
README and acquisition CSV have downloaded; the CSV hash matches the publisher.
The large IQ file is still a `.part` until verification succeeds. Only this first
21.6 GB file was requested, leaving disk headroom for derived data.

The [NIST collection report](https://doi.org/10.6028/NIST.TN.2237) describes ordinary
Wi-Fi traffic in an uncontrolled university environment. These are strong
background candidates; unknown nearby emitters and anomalous intervals still
need review. Dataset provenance is useful evidence, not a reason to assign
unconditional absence labels to every crop.

A one-shot dependent process in
`data/drone_ml/2026-10-23/nist_background_01/after_download.py` waits for the exact
existing curl PID/start-time identity. It records progress every 15 seconds.
After transfer exit it verifies and inventories the source, then audits the
first, middle and last capture as development diagnostics. Partial transfer,
checksum/layout failure or timeout produces a failed status. It never launches
another download, trains a model or enables acceptance. It is a running local
job, not a recurring notification automation; host shutdown can interrupt it.

Live progress:

```bash
watch -n 5 cat /home/sudeep/Documents/newrocktest-cpp/data/drone_ml/2026-10-23/nist_background_01/progress.json
```

Progress JSON is not proof of current liveness after an interruption. The process
record and `/proc` start identity, or the execution sessions, must also be checked.
Do not start another transfer solely because a progress file is old.

## Next milestones and decision gates

1. **Implemented:** isolated public-source verification, provenance inventory,
   bounded temporal diagnostics and protected-partition checks.
2. **Running:** first NIST file download, dependent hash/layout verification and
   three-capture diagnostics. Completion is represented by `phase=complete`;
   that means data diagnostics complete, not model qualification.
3. **Pending:** inspect representative spectrograms/quality and source metadata,
   classify label confidence, and expand backgrounds across independent sources
   and sessions. Keep uncertain intervals excluded. Revisit DRFF environment
   label conflicts; do not override them with filename assumptions.
4. **Pending:** add a public-data corpus adapter with explicit label review and
   cross-source/collection leakage guards. Prevent receiver/dataset identity
   from becoming a shortcut for background versus drone. NIST alone cannot
   supply independent train/calibration/development sessions by splitting its
   timestamps randomly.
5. **Pending:** compare local 8/16/32 ms crops and temporal 64/128/256/512 ms
   contexts on fixed group splits. Start the main temporal candidate at 128 ms,
   preserving local detail and a roughly one-second evidence history. Fit
   thresholds on separate groups; report missed encounters, false acceptance,
   known rejection, latency, compute and coverage. Do not call window-level
   activity coverage or short-clip errors false alerts/hour.
6. **Pending:** evaluate the separate presence baseline versus a compact shared
   CNN + temporal convolution with presence/identity/rejection outputs. Retain
   unknown RF/insufficient evidence states. Withhold entire unknown drone models
   from all training heads. Freeze the pipeline before final tests and later
   local USRP/Mini 2 qualification.

Follow-on implementation: the untrained shared CNN/causal-TCN candidate, fixed-view
128 ms preview preparation and initial CPU timing are now available. See
[temporal candidate](DRONE_TEMPORAL_MODEL_CANDIDATE.md). This advances architecture
preparation, not dataset review, real-data training or model qualification.

Useful later candidates from the research pass:

- [DroneRFb-DIR](https://doi.org/10.57760/sciencedb.20454): six types, eighteen
  physical units and urban background context; file access, labels, independence
  and non-commercial/share-alike terms require admission review.
- [Noisy Drone RF v2](https://www.kaggle.com/datasets/sgluege/noisy-drone-rf-signal-classification-v2):
  74.9 ms examples at 14 MSps, augmented noise class; independent parent-recording
  splits and a compatible bandwidth profile are required. Upsampling cannot
  manufacture the missing bandwidth.
- [Annotated drone/Wi-Fi/Bluetooth images](https://doi.org/10.17632/y4w96kzsb5.1):
  localization benchmark, not a replacement for numeric IQ/STFT source data.

## Verification

Seven targeted tests cover energy timing on known synthetic bursts, sustained
traffic, contexts longer than observations, reserved-file exclusion, NIST int16
conversion and bounds, external-link rejection, incomplete download exclusion,
unverified label preservation, source immutability and hash tampering.
Run `python -m unittest tests.test_drone_ml_public_temporal` in the existing ML
runtime. These are software checks, not RF classification accuracy measurements.

Final checks: all 17 public-temporal/presence/audit tests and all 3 existing live
worker tests passed (20 total). The live worker tests initially hit sandbox IPC
permission errors; the same three tests passed when run outside that restriction
with synthetic data and no hardware. Python compilation and whitespace checks
also passed. The presence smoke test trains one temporary synthetic epoch only;
it is not the planned real-data training run and is not promoted.
