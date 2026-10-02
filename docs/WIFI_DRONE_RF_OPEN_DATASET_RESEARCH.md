# Public RF datasets for the Wi-Fi drone-analysis add-on

Research date: 2 October 2026  
Method: Firecrawl searches and live reads of publisher pages, repository documentation and file-list APIs; read-only inspection of the user's local DroneDetect files.  
Scope: planning and data assessment only. No application code, classifier, converter or transmitter was implemented or run. No new bulk dataset downloads were started.

## 1. Recommended starting corpus

Use **the existing local DroneDetect_V2 first**, add the **Tampere Zenodo complex-IQ recordings** for another receiver and 5 GHz examples, and selectively add **RFUAV raw archives** for more recent drones and diverse controllers. Include ordinary Wi-Fi, Bluetooth and Zigbee recordings as negative controls. DroneDetect's interference subsets still contain drone signals; they are not drone-free negative examples.

These sources can bring forward offline experiments without waiting for new drone purchases or field collection. They do not establish that every proposed link family is recognizable, identify all physical units, or satisfy the plan's independent-unit and deployment gates. Public device labels must not be silently translated into OcuSync, ELRS or another protocol/mode label without supporting acquisition metadata.

## 2. Your local DroneDetect inventory

Verified location: [DroneDetect_V2](/home/sudeep/Pictures/DroneDetect_V2).

| Item | Read-only inventory result |
|---|---|
| Files | 390 `.dat` recordings; 374,203,574,280 bytes, approximately 374.2 GB / 348.5 GiB |
| Conditions | `CLEAN`: 95; `BLUE`: 95; `WIFI`: 100; `BOTH`: 100 |
| Model codes | `AIR`, `DIS`, `INS`, `MIN`, `MP1`, `MP2`, `PHA` |
| State codes | `ON`, `HO`, `FY`; publisher labels mean switched on, hovering and flying |
| Missing populated folders | `BLUE/PHA_FY` and `CLEAN/PHA_FY` exist but contain no `.dat` files |
| Other available combinations | Five recordings per populated condition/model/state folder; no `DIS_HO` folder was present |
| Local metadata | No loader, README, licence file or sidecar metadata found inside this directory |
| Sample checks | Read only 32 KiB from one `AIR_FY` file in each condition. Little-endian float32 values were finite and consistent with the published interleaved-float description. This is a format sanity check, not full-file integrity or signal validation. |

The [publisher's DroneDetect record](https://ieee-dataport.org/open-access/dronedetect-dataset-radio-frequency-dataset-unmanned-aerial-system-uas-signals-machine) documents seven drone models, a bladeRF receiver, 2.4375 GHz center frequency, 28 MHz bandwidth and two-second interleaved complex-float recordings. Its “60 Mbits/s” sample-rate wording conflicts with the stated 120 million complex samples in two seconds; **60 million complex samples/s** is the consistent interpretation, to be checked against the original loader/acquisition settings before feature extraction. A 960,000,000-byte float32 IQ file then corresponds to exactly two seconds. IEEE offers the dataset through a free account; your local copy removes that download dependency. The exact reuse terms were not verified from the public record.

Two local recordings are shorter than the nominal length:

| File relative to dataset root | Bytes | Duration assuming headerless float32 IQ at 60 Msps |
|---|---:|---:|
| `CLEAN/INS_FY/INS_0010_00.dat` | 842,727,408 | 1.755682 s |
| `BOTH/INS_FY/INS_1110_00.dat` | 873,922,520 | 1.820672 s |

Many other files are slightly longer than 960 MB. Import must record actual lengths, inspect format/alignment and distinguish extra samples from metadata or corruption; do not crop or pad silently. Shorter files are not automatically corrupt, but must be excluded from tests requiring a complete nominal interval until reviewed. None were changed.

Folder flight-state labels can support offline evaluation; they do not justify a live claim of hovering or flying from RF alone. Capture timing between separate files and physical-unit independence remain unknown. Never concatenate files into an invented continuous activity timeline.

## 3. Public sources ranked by practical value

### A. Tampere drone control and video IQ — first external addition

[Radio-Frequency Control and Video Signal Recordings of Drones](https://zenodo.org/records/4264467), DOI `10.5281/zenodo.4264467`.

- Open direct downloads; Zenodo metadata explicitly lists **CC BY 4.0**.
- Nineteen IQ `.bin` files, approximately 8.56 GB in total, plus loaders/examples.
- Explicit interleaved signed 16-bit little-endian I/Q. The 2.4 GHz recordings use 120 Msps for one second; 5 GHz recordings use 200 Msps for half a second.
- Ten drone models spanning DJI, Parrot and Yuneec. Some models have both bands; Parrot Mambo control and video have separate recordings.
- Anechoic-chamber acquisitions are useful clean references and cross-dataset checks. Short bench captures and older models limit field and current-link conclusions. Paired `1of2` / `2of2` files are nonconsecutive and must stay separate.

Recommended small selection before taking the whole corpus:

| File | Size | Purpose |
|---|---:|---|
| `DJI_mavic_pro_2G.bin` | 480 MB | DJI 2.4 GHz reference |
| `Parrot_mambo_control_2G.bin` | 480 MB | Separately labelled control reference |
| `Parrot_mambo_video_2G.bin` | 480 MB | Separately labelled video reference |
| `DJI_phantom_4_pro_plus_5G_1of2.bin` | 400 MB | Initial 5 GHz reference |

This selection is 1.84 GB. File downloads and published checksums are available in the [Zenodo record API](https://zenodo.org/api/records/4264467). The observed local `Pictures/zenodo datasets` folder currently contained only `.crdownload` files; incomplete browser downloads must not enter the corpus manifest. Check for completed existing copies before downloading duplicates.

### B. RFUAV — best verified breadth candidate

[RFUAV dataset](https://huggingface.co/datasets/kitofrank/RFUAV), [author repository](https://github.com/kitoweeknd/RFUAV).

The dataset card declares **Apache 2.0** and describes a 37-class, approximately 1.3 TB research corpus. The author repository says the public release is a subset. The live [Hugging Face file list](https://huggingface.co/api/datasets/kitofrank/RFUAV/tree/main?recursive=false) contains 37 model/controller `.rar` archives alongside images and weights; this confirms downloadable archive entries, not that their contents have been locally checked. The repository's [raw-data processor](https://github.com/kitoweeknd/RFUAV/blob/main/graphic/RawDataProcessor.py) reads interleaved float32 I/Q. Per-pack XML supplies sample rate, frequency and data type; its example 100 Msps is not a universal setting.

Useful archive entries include DJI Mini 4 Pro (4.95 GB), Avata 2 (11.41 GB), Mavic 3 Pro (5.16 GB), Autel Evo Nano (3.37 GB), RadioMaster Boxer (2.71 GB), FlySky FS I6X (1.57 GB), FrSky and Futaba. Select a few packs and retain their XML and any archive-specific terms. Controller model names alone do not establish protocol version or drone use. Spectrograms and pretrained image models cannot replace raw IQ for our cyclic estimator.

### C. NIST Wi-Fi / Bluetooth IQ — essential false-positive controls

[NIST public dataset](https://www.nist.gov/data-publications/wi-fi-and-bluetooth-iq-recordings-24-ghz-and-5-ghz-bands-low-cost-software-defined), DOI `10.18434/mds2-2731`.

Public baseband IQ: 900 one-second recordings at 30 Msps, centers 2.437 GHz and 5.825 GHz. Five HDF5 data files cover 2.4 GHz Bluetooth, indoor/outdoor 2.4 GHz and indoor/outdoor 5 GHz; metadata/calibration CSVs accompany them. Each currently listed HDF5 download is about 21.6 GB. Prioritize one 2.4 GHz ordinary-traffic file and one 5 GHz file. Inspect HDF5 layout and repository reuse statements before import; they were not fully verified here. These controls help measure whether ordinary radios receive false drone labels; local cameras and RC cars/boats are still needed.

### D. SDR4IoT BLE / Zigbee — compact supplemental controls

[SDR4IoT BLE & Zigbee RF dataset](https://zenodo.org/records/4639390), DOI `10.5281/zenodo.4639390`.

Public Zenodo metadata specifies **CC BY 4.0**, raw IQ and demodulated BLE/Zigbee traffic; `dataset.zip` is only 78.66 MB. The preview includes `.sigmf`, `.csv` and `.cap` entries. Check the archive carefully: metadata and packet exports are not themselves IQ, and the presence of a `.sigmf` file does not prove that its referenced binary is bundled. Confirm actual sample data, rate and packing before admitting it as a cyclic-analysis fixture. Useful for testing GFSK/OQPSK confusion and generic burst classification; it contains no verified drone-link labels.

### E. DroneRFa — later field-diversity candidate

[DroneRFa on ScienceDB](https://www.scidb.cn/en/detail?dataSetId=34f0a91e8a544904998b8fdc44477380), DOI `10.57760/sciencedb.18475`.

The record describes nine outdoor flying-drone types, fifteen indoor drone types and one background type across three ISM bands, with distance/frequency labels; it lists 573.65 GB. Licence: **CC BY-NC-SA 4.0**, so it is not an unrestricted product corpus. The scraped page did not expose a usable file list, and its description contains conflicting associated-paper references. Actual sample packing, download access and provenance need verification. Keep it as a conditional research candidate rather than a dependency for the first milestone.

### F. Remote-controller RF dataset — useful but a different sample domain

[AERPAW / NC State description](https://aerpaw.org/dataset/drone-remote-controller-rf-signal-dataset/), [IEEE dataset](https://ieee-dataport.org/open-access/drone-remote-controller-rf-signal-dataset), DOI `10.21227/ss99-8d56`.

Seventeen physical controllers from eight manufacturers, with approximately 1,000 records per controller. Oscilloscope recordings are five million samples in 0.25 ms, in MATLAB format; this implies 20 GSps real RF sampling rather than ready-to-use complex baseband IQ. IEEE lists a 124.13 GB archive behind free account access. Reuse terms remain unverified. Requires validated frequency metadata and conversion into analytic/baseband samples; short segments cannot establish long-term activity or hop sequences. It can supplement controller waveform work, but should not delay the complex-IQ starting corpus.

### G. DroneRF 2019 and DroneRF2025 — supplementary, not the first dependency

The [DroneRF 2019 author site](https://al-sad.github.io/DroneRF/) and [data paper](https://pmc.ncbi.nlm.nih.gov/articles/PMC6727013/) describe three drone types and background captures. The distributed CSV samples are amplitude records, split into lower/upper receiver parts; do not assume a phase-preserving complex-IQ representation. Use for legacy amplitude/spectral comparisons unless acquisition metadata supports a valid alternate cyclic formulation. The existing `Pictures/qatardronerf/DroneRF` folder contains corresponding CSV filenames, but was not fully audited here.

[DroneRF2025](https://ieee-dataport.org/documents/dronerf2025) lists six drone models and raw IQ/image data, but states that only part has been uploaded. It is listed under standard IEEE documents rather than the verified open-access route. Complete access, format and reuse terms were not verified. Keep it optional.

## 4. What remains uncovered

The targeted search did **not verify a dedicated open raw-IQ corpus with explicit mode labels** for ELRS 2.4 GHz, HDZero, Walksnail or analog FPV. This is a search result, not a claim that none exists. RadioMaster/Jumper archive names do not by themselves prove ELRS capture. Public protocol source code is useful documentation, but is not measured waveform ground truth.

We still need references covering actual mode/firmware/rate variants, non-drone use of RC/video equipment, multiple physical units, local receiver characteristics, source-separated controller/aircraft signals and time-labelled RF changes. Long activity sequences, simultaneous emitters and unchanged scanner coverage are underrepresented in these short recordings. A dataset's flight labels cannot substitute for field validation or authorize live flight-state claims. Remote ID remains deferred.

## 5. Updated milestone path

| Planning increment | Deliverable when implementation begins | Effect on existing milestones |
|---|---|---|
| **M1a — first 2–3 working days of corpus work** | Read-only local manifest; original-loader/packing check; variable-length and missing-folder ledger; source/rate/frequency/licence provenance | Begin M1 using local DroneDetect immediately; do not re-download it |
| **M1b — following 3–5 working days, dependent on downloads** | Small balanced DroneDetect working subset, four-file Zenodo selection, one or two RFUAV packs, verified ordinary-traffic negatives; sealed source-level split | Complete the initial M1 corpus foundation without waiting for all catalogue hardware |
| **M2 — offline waveform measurements** | Same cyclic estimator tested on source-native data and passband/tile selections matching live captures; per-source/condition comparisons | Public IQ accelerates initial waveform and attribution experiments; missing W3/W7 references remain explicit collection tasks |
| **M3 / M4 — qualified activity and families** | Provisional matches measured across independent datasets, followed by actual unit/session/mode validation | Public sets help select promising families; they do not waive G2/G2A or satisfy independence merely by having many windows |
| **M5–M7 — integration and HIL** | Conducted replay of suitable derived recordings plus legacy regression/resource/soak checks | Reference waveforms reduce signal-generation work; unchanged non-interference gates still apply |
| **M8 / M9 — breadth and field release** | Missing-family collection, confusable hardware tests, independent local drone systems and representative sites | Actual hardware/field access remains necessary for release claims |

The added day ranges are provisional work estimates, not measured completion times. Keep the original **14–22 week release estimate** until importer checks and the first cross-dataset benchmark establish which families are separable. Public data should reduce early collection delay; it cannot defensibly justify a shortened field-release schedule yet.

## 6. Data handling and HackRF implications

1. Preserve original files. Future manifests record source DOI/version, checksum, model and verified mode, receiver, sample representation/rate/center, bandwidth, duration, gaps, labels, unit/session confidence and applicable terms. Retain unknown metadata as unknown.
2. Split by source recording, physical unit/session when known, and collection source **before** extracting windows. Overlapping crops, channelized versions, added-noise versions and HackRF replays of one recording share its split. Add entire held-out model families for unknown rejection; report unknown physical-unit independence explicitly.
3. Retain source-native IQ for analysis. For live-budget evaluation, select the actual observable passband, resample with anti-alias filtering where needed, apply bounded tile selection and model scan gaps. Whole-band offline results cannot be claimed for the unchanged 20 Msps live profile.
4. Use raw IQ rather than exported pictures, magnitudes or demodulated symbols for the planned complex SCF. Alternative real-signal cyclic estimators require their own documented formulation and validation.
5. HackRF replay needs suitable complex-baseband portions converted to supported rates and signed 8-bit IQ, with headroom. DroneDetect at 60 Msps, Zenodo at 120/200 Msps and typical RFUAV wideband packs cannot be transmitted intact through a 20 Msps HackRF. Changing a sample-rate label or playing samples slower changes physical timing and cyclic frequencies. Band-limited conversion may discard hops and waveform bandwidth, which must be recorded.
6. Use the existing conducted, attenuated setup for future authorized replay. No transmission was performed here. Preserve replay provenance, ignore loop-boundary artifacts and measure receiver/TX distortion. Replay verifies pipeline behavior; it creates no new independent drone unit and does not prove that a real aircraft is present.

Implementation decisions stay subject to the additive architecture and gates in [the integration plan](/home/sudeep/Documents/newrocktest-cpp/docs/WIFI_CYCLOSTATIONARY_DRONE_ADDON_PLAN.md).
