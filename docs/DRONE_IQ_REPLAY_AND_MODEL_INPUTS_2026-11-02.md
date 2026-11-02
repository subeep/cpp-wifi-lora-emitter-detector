# Real-IQ integration replay and model-input preparation — 2026-11-02

## Outcome

Completed offline native-decoder replay of **27 recording/member/band cases covering 22 dataset model-context labels** from DRFF-R2, RFUAV, KU Leuven and Tampere. This is 21 training-context labels plus Mini 3 Pro from unknown calibration, not 22 independently verified aircraft identities. Each representative case uses up to its first one second: **21.6 sample-seconds total**, excluding the five separate preselected packet controls. This is a coverage smoke test, not a population accuracy study.

The strongest finding is a coverage problem before model classification: the current live-prefix path saw no CRC-valid packets in these cases, while extended replay found one; a fixed frequency grid recovered two additional packets among five positive controls. Decoder failure must not gate off the mandatory classifier or become a no-drone label.

The new offline input pipeline produced **198 frequency views / 5,564 valid 8 ms STFT tiles** from 26 training-partition cases. All passed random-weight forward compatibility checks. The hybrid model remains **untrained**, with no accuracy, calibrated probabilities or unknown-rejection performance established. Existing live application functions and deployed checkpoints were not changed by this replay/preparation work.

## What was tested

Selection was the frozen training partition's first lexical representative per model/dataset/band. Broad native replay uses the existing Release worker, repeated requests of at most 160 ms, with continuity within each recording. We separately report its first request (at most 160 ms), approximating the live observer's current prefix selection, and the extended sequence (up to 1 s). This is backend replay; it does not exercise RF acquisition, Qt scheduling, live queues or dropped buffers.

Legacy replay used four dispersed 20 ms windows where its current rate limits allow it: 12 cases at 100 MSps. The 14 Tampere cases at 120/200 MSps were explicitly unsupported, not negative results. The extra Mini 3 Pro case was native-only development replay, stays unknown calibration and was not exported into training inputs. Reserved final-test IQ was not opened; Mavic 3C/S contexts with only unknown-final partitions were excluded.

In the table, CRC counts are outer CRC24-valid packets; the sole broad-replay packet also passed inner CRC16. O4 counts are candidate **windows**, not independent detections or confirmed link/model identities. Zero means this backend recovered no evidence in the tested interval; it does not mean no drone was present.

| Recording context | Dataset | MSps | Seconds | First-prefix CRC / O4 windows | Extended CRC / O4 windows | Legacy |
|---|---|---:|---:|---:|---:|---|
| DJI Air 2 | DRFF-R2 | 100 | 1 | 0 / 0 | 0 / 0 | 0 packets |
| DJI Air 2S | DRFF-R2 | 100 | 1 | 0 / 0 | 0 / 0 | 0 packets |
| DJI Avata 2 | RFUAV | 100 | 1 | 0 / 0 | 0 / 9 | 0 packets |
| DJI FPV | RFUAV | 100 | 1 | 0 / 0 | 0 / 0 | 0 packets |
| DJI Inspire 2 | KULeuven_DroneRF | 100 | 0.2 | 0 / 0 | 0 / 0 | 0 packets |
| DJI Inspire 2 | Tampere_DroneRF | 120 | 1 | 0 / 0 | 0 / 0 | Rate unsupported |
| DJI Inspire 2 | Tampere_DroneRF | 200 | 0.5 | 0 / 0 | 0 / 0 | Rate unsupported |
| DJI Matrice 100 | Tampere_DroneRF | 120 | 1 | 0 / 0 | 0 / 0 | Rate unsupported |
| DJI Matrice 210 | Tampere_DroneRF | 120 | 1 | 0 / 0 | 0 / 0 | Rate unsupported |
| DJI Matrice 210 | Tampere_DroneRF | 200 | 0.5 | 0 / 0 | 0 / 0 | Rate unsupported |
| DJI Matrice 300 | KULeuven_DroneRF | 100 | 0.1 | 0 / 0 | 0 / 0 | 0 packets |
| DJI Mavic 3 | DRFF-R2 | 100 | 1 | 0 / 0 | 0 / 0 | 0 packets |
| DJI Mavic 3 Pro | RFUAV | 100 | 1 | 0 / 0 | 0 / 0 | 0 packets |
| DJI Mavic Mini | Tampere_DroneRF | 120 | 1 | 0 / 0 | 0 / 0 | Rate unsupported |
| DJI Mavic Pro | Tampere_DroneRF | 120 | 1 | 0 / 0 | 0 / 0 | Rate unsupported |
| DJI Mini 2 | KULeuven_DroneRF | 100 | 0.2 | 0 / 0 | 0 / 0 | 0 packets |
| DJI Mini 4 Pro | DRFF-R2 | 100 | 1 | 0 / 6 | 0 / 58 | 0 packets |
| DJI Mini 5 Pro | DRFF-R2 | 100 | 1 | 0 / 0 | 1 / 0 | 0 packets |
| DJI Phantom 4 | Tampere_DroneRF | 120 | 1 | 0 / 0 | 0 / 0 | Rate unsupported |
| DJI Phantom 4 Pro Plus | Tampere_DroneRF | 120 | 1 | 0 / 0 | 0 / 0 | Rate unsupported |
| DJI Phantom 4 Pro Plus | Tampere_DroneRF | 200 | 0.5 | 0 / 0 | 0 / 0 | Rate unsupported |
| Parrot Disco | Tampere_DroneRF | 120 | 1 | 0 / 0 | 0 / 0 | Rate unsupported |
| Parrot Mambo | Tampere_DroneRF | 120 | 1 | 0 / 0 | 0 / 0 | Rate unsupported |
| SJRC F11 Pro | KULeuven_DroneRF | 100 | 0.1 | 0 / 0 | 0 / 0 | 0 packets |
| Yuneec Typhoon H | Tampere_DroneRF | 120 | 1 | 0 / 0 | 0 / 0 | Rate unsupported |
| Yuneec Typhoon H | Tampere_DroneRF | 200 | 0.5 | 0 / 0 | 0 / 0 | Rate unsupported |
| DJI Mini 3 Pro | DRFF-R2; unknown calibration | 100 | 1 | 0 / 0 | 0 / 0 | Not run |

Across the first 26 cases, extended replay examined 1,276 O4 windows. Only Avata 2 (9 candidate windows) and Mini 4 Pro (58) produced O4 candidates. Mini 4 Pro had six within the first prefix; Avata 2's candidates appeared later. Mini 5 Pro yielded one CRC24/CRC16-valid INFP packet during extended replay, without supported telemetry parsing or exact-model identification. Mini 3 Pro's additional 62 windows yielded no candidate. No native errors were reported. The 12 eligible legacy cases yielded no confirmed broadcasts or reported errors.

Mini 2 specifically: its tested Leuven recording was only 200 ms, with no recovered native or legacy broadcast. This does **not** test or repair Mini 2 ML classification, and cannot establish whether a decodable broadcast was present in that interval.

## Packet positive controls and frequency coverage

Five existing clips had already been selected because they previously decoded. Their parent recordings were verified as training-partition sources. These controls test regression/transport/channel-selection behavior, not unbiased recall. They are additional clips; they do not add new model-context labels to the 22 above.

| Recording context | Original view: CRC packets | Frequency grid: CRC packets | Envelope | Telemetry parsed |
|---|---:|---:|---|---|
| DJI Mavic 3 | 0 | 1 | infp | No |
| DJI Air 2S | 1 | 1 | v2 | Yes |
| DJI Mini 5 Pro | 1 | 1 | infp | No |
| DJI Mavic 3 Pro | 1 | 1 | v3 | No |
| DJI Mavic Pro | 0 | 1 | v2 | Yes |

All five grid recoveries passed outer CRC24 and inner CRC16. The two v2 results parsed telemetry but did not produce an exact model name. INFP/v3 results remained unsupported telemetry envelopes. A successful checksum is packet integrity evidence, not cryptographic identity authentication. No claim of universal O1/O2/O3/O3+/O4+ control or video decoding follows from these results.

The diagnostic uses deterministic overlapping 20 MSps views rather than relying only on the strongest 10 MHz channel. It recovered Mavic 3 and Mavic Pro clips missed by the original wideband input. This frequency grid is **offline only**; it has not been silently enabled in the live GUI. It also excludes source edges and cannot promise all simultaneous emitters or wideband waveforms are preserved. Wi-Fi/OpenDroneID real-IQ qualification was not part of this replay.

## Model work completed

Implemented `tools/drone_ml/hybrid_inputs.py` and `hybrid_feature_policy.py` alongside the replay harness. Input preparation now:

1. Verifies replay-derived IQ hashes and preserves source/group provenance.
2. Creates seven overlapping frequency views for each 100/120 MSps source and eleven for each 200 MSps source, at 20 MSps per view. Frequency views are correlated derivatives, not new independent recordings.
3. Uses causal FIR downconversion/decimation, 8 ms of real pre-roll and retained filter delay; it does not pad future samples or concatenate unrelated recordings. Supported source rates are integer multiples of 20 MSps from 20 to 200 MSps.
4. Produces numerical STFT tiles of shape `1 × 256 × 625`. Available context is 80–256 ms in this sample, not a promise that 256 ms captures every broadcast. Short files remain short.
5. Recomputes five correlation/prominence features from exactly the same frequency view. A completed 16 ms DSP window is attached only to its ending 8 ms tile; earlier tiles retain missing masks.
6. Keeps invalid/constant tiles masked rather than treating zeros as verified background. All generated views have unset presence/identity labels and `training_eligible=false` pending label-scope review.
7. Uses a baseline feature policy that excludes source-dependent ADC clipping availability and unimplemented Zadoff–Chu/bandwidth/duty features from model inputs. ADC availability could otherwise reveal the dataset encoding instead of the emitter. ADC quality remains audit metadata.

The experimental architecture remains a shared STFT CNN (channels 16/32/64/96, 64-dimensional output), a masked DSP branch (18→32→32), fusion (96→64), and six causal temporal convolution blocks with dilations 1/2/4/8/16/32. It has separate presence and identity heads plus an embedding for later rejection experiments. The 21-class instantiation has 261,975 parameters. Its 127-tile theoretical receptive field is 1.016 s; this prepared sample supplies only 80–256 ms. Model support for longer context is not evidence that any particular duration is sufficient.

The mandatory classifier should consume valid scheduled views regardless of decoder success. Presence, conditional identity and unknown rejection need separate calibration. “Unknown drone” requires credible drone-presence evidence with rejected known identity; inconclusive RF must remain inconclusive. Recording labels must not be copied to every frequency slice or quiet tile. Even a positive recording does not guarantee that a selected 256 ms bag or subband contains that drone.

These tensors use a new preprocessing contract and are explicitly incompatible with the old live checkpoint. The all-view forward checks used random weights and saved no classification predictions. They establish finite values, shape compatibility, masks and provenance only. No optimizer, training job, checkpoint export or model promotion was started.

## Performance and verification limits

The first 26 native replays consumed **388.23 s of reported worker processing for 20.6 s of sampled RF** (about 18.85× the RF duration). The Mini 3 Pro addition took 15.55 s for 1 s of RF. The binary was already Release/-O3 with FFTW. These are observed offline worker timings under concurrent work, not isolated end-to-end benchmarks. They provide no real-time guarantee and exclude some I/O/coordination costs.

Random-weight CPU forward checks used two Torch threads and one prepared view at a time: 43.64–223.75 ms per view, median 200.72 ms across variable-length contexts. They exclude channelization, STFT generation, DSP extraction, acquisition and UI queues. No GPU throughput, batched scheduling or full application latency qualification was performed. Processing seven or eleven views sequentially adds substantial cost; this diagnostic is not a deployable real-time schedule.

Twenty tests passed covering causal filtering, future-sample isolation, feature alignment, missingness policy, reserved-partition rejection, invalid signal handling, temporal masking and model losses. `git diff --check` passed. Existing C++ functionality was not modified in this pass, so this is not a new claim of a full GUI/hardware regression run.

Source file stats were checked before/after and the dataset mount was read-only. Both earlier model checkpoint hashes remained unchanged. Derived IQ/tensor hashes and source/member provenance are recorded. Full original-source SHA-256 was not computed for every large archive: do not confuse a derived-prefix digest or preserved file stats with complete source-content hashing. Physical sampling-clock continuity in all publisher sources is still not independently established.

Four 200 MSps cases initially failed only at derived-prefix export after native/legacy processing had already finished: the requested prefix exceeded the reader's 40-million-sample limit. The harness was corrected to cap exports, those four prefixes were regenerated, and all input preparation completed. Original results/errors remain preserved alongside `prefix_repairs.json` and `resolved_results.json`; they are not four decoder failures.

## Before a trustworthy training run

1. **Review label scope:** adjudicate usable positive regions/bags without assigning identity to all subbands. Keep undecoded intervals unlabelled. The current 198 views are a pipeline check, not a training-ready corpus.
2. **Establish negative evidence:** the reviewed corpus still lacks a verified non-drone evaluation set. Controlled no-drone recorder sessions need scene, interval, band, actual sample rate and gain metadata. Router/phone and mixed-emitter cases matter. Target-off, synthetic silence and decoder failure are insufficient absence ground truth. This is the main unresolved prerequisite for measuring the false-alarm problem.
3. **Freeze independent groups:** only Air 2, Air 2S and Mini 4 Pro have declared separate known-calibration groups in the inherited split, whose independence still requires review. The other 18 training labels lack separate known-class calibration groups. Keep physical-aircraft/session/environment groups intact; overlapping views never cross partitions. Mini 3 Pro stays unknown calibration. Final-test partitions stay reserved.
4. **Compare baselines fairly:** shared STFT temporal presence/identity model versus the same model with the five common DSP features; evaluate receiver/source-held-out behavior and missing-feature/dropout ablations. O4-profile features must demonstrate value on non-O4 and non-DJI cases instead of becoming an O4-only presence gate. Do not pick the larger model merely because it has more inputs.
5. **Qualify decisions:** measure false alarms per monitored time, known/unknown recall, known rejection and unknown false acceptance, per-class/group-balanced results and time to detection. Choose thresholds only on development data, then freeze the policy before final evaluation. Maximum softmax is not a calibrated identity probability.
6. **Address deployment coverage and load:** replay-driven frequency coverage, longer contiguous context and bounded asynchronous scheduling need a measured compute budget and explicit lost-coverage counters. Keep the classifier running for unresolved decoder cases. Test C++/Python numerical parity before integrating the new checkpoint. Live Mini 2/USRP qualification remains pending.

No detection method can guarantee every drone under arbitrary band coverage, range, interference, collisions, silence or receiver overload. The objective is measured recall and false alarms within a stated operating envelope, with unknown/inconclusive states preserved.

## Reproducible evidence

- [Machine-readable summary](../data/drone_ml/2026-11-02/verification_summary.json)
- [Broad replay manifest](../data/drone_ml/2026-11-02/integration_replay_01/manifest.json)
- [Resolved backend results](../data/drone_ml/2026-11-02/integration_replay_01/resolved_results.json)
- [Prefix repair audit](../data/drone_ml/2026-11-02/integration_replay_01/prefix_repairs.json)
- [Mini 3 Pro development replay](../data/drone_ml/2026-11-02/unknown_development_replay_01/result.json)
- [Original positive controls](../data/drone_ml/2026-11-02/packet_controls_01/results.json)
- [Frequency-grid positive controls](../data/drone_ml/2026-11-02/packet_grid_controls_01/results.json)
- [Prepared input index](../data/drone_ml/2026-11-02/hybrid_inputs_01/index.json)
- [All-view compatibility and preservation checks](../data/drone_ml/2026-11-02/hybrid_inputs_01/model_input_validation.json)
- [Pretraining caveats and readiness](DRONE_ML_PRETRAINING_READINESS_2026-10-29.md)

The replay and extractor source snapshots are retained with their runs. Current research-model description metadata predates the offline input join: its statement that same-interval extraction is unimplemented is superseded by the offline artifacts above; streaming deployment remains unimplemented.
