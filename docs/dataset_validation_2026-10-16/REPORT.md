# Dataset research and offline validation — 16 October 2026

## Changes and measured results

Added JSON output to the existing offline `lora_replay` tool and two repeatable validation scripts under `tools/validation`. No model was trained, no radio was opened, and production device inventories were not populated. Original IQ files were read only and their hashes checked before and after replay.

* **LoRa received fixtures: 3/3 passed** exact payload, valid header, CRC, spreading factor and bandwidth checks. These are three packets from one earlier SX1262 capture session at SF7/BW125 kHz, not three independent device/session trials. Other metadata fields such as coding rate and sync word are not asserted by this gate.
* **Derived LoRa stress cases: 8/9 passed.** All six added-noise cases passed; two of three +1 kHz frequency-offset cases passed. The offset variant of `capture-1790061257005581306-0` failed recovery. No CRC-valid wrong payload was returned. Noise power is defined relative to the entire recording's mean power, not measured packet SNR. These synthetic variants are diagnostics, not independent field observations. The baseline gate passes while retaining stress misses explicitly.
* **Cached drone excerpts: 23/23 processed, 92 windows.** The current production DSP worker processed hash-verified excerpts from Leuven, Tampere, DRFF-R2 and RFUAV references. Every identity remained unknown and named-family acceptance stayed disabled. This verifies processing and conservative identity handling, not positive drone-detection recall or named-link accuracy. Selected intervals are supplied as continuous based on the reference manifests; continuity was not independently audited against original receiver logs.
* **Targeted CTest checks: 5/5 passed**, including the new `lora_payload_replay` gate, LoRa receiver, observation and security tests, and the cyclostationary reference corpus. Deliberately changing an expected payload in a temporary fixture copy correctly failed validation. Unknown and duplicate replay options correctly returned errors. `git diff --check` passed.

The SDA source volume was not mounted during this work. Existing project-local derived excerpts were used; source dataset directories were not modified. These excerpts remain development data and cannot become an independent held-out test set by renaming them.

Evidence: [LoRa results](lora-payloads.json), [drone replay summary](drone-replay-verified/summary.json), [targeted tests](tests.txt). The initial `drone-replay.txt` records an unsuccessful invocation with a wrong executable path; `drone-replay-verified` contains the completed run.

## Suitable datasets found with Firecrawl

| Dataset | Useful evidence | Access and limitations |
| --- | --- | --- |
| [LoRaIQ, EPFL](https://zenodo.org/records/20341802) | Raw SigMF IQ and CSV containing transmitted payloads, PHY settings, receiver indices and sample locations. Best next source for checking real decoder recovery against known payloads. | Public download; approximately 71 GB IQ archive plus 28.3 MB CSV. Covers SF7/BW125 kHz pedestrian transmissions and SF10/BW250 kHz UAV transmissions at 862.5 MHz, CR4/5. Does not cover all LoRa modes or native DJI links. Not downloaded. |
| [AWID3](https://icsdweb.aegean.gr/awid/awid3) | Packet captures and extracted features for deauthentication, disassociation, reassociation, rogue AP, KRACK, Kr00k and other attacks in an enterprise Wi-Fi testbed. Useful for packet-level attack-rule validation and benign controls. | [Registration/access request](https://icsdweb.aegean.gr/awid/download) required. PCAP does not provide raw IQ or USRP continuity evidence. Published attack-frame ranges need careful label handling. No importer implemented or access request submitted. |
| [CageDroneRF / U-RAPTOR](https://github.com/DroneGoHome/U-RAPTOR-PUB) | [Paper](https://arxiv.org/html/2601.03302v1) describes raw drone RF captures in cage and campus settings, with contemporary drone models. Potentially useful for broader real waveform coverage. | Full data uses a [request portal](https://aerodefense.tech/u-raptor-data-request). Public code does not mean unrestricted direct dataset download. Per-record link mode, independence and raw-versus-augmented labels must be inspected before accepting named-link truth. Dataset terms include noncommercial/share-alike conditions. No request submitted or code/model imported. |

AirID uses USRP transmitters carried by drones; it is not evidence for the drones' native proprietary control links. Synthetic corpora can stress algorithms but do not independently validate named drone-link identity.

## Next validation work

1. **LoRaIQ adapter:** start with metadata and a bounded selection of original captures once downloaded. Verify sample representation, rate, offsets, payload and PHY labels. Group all receiver views by transmission ID; split by session/device where available. Measure exact recovery and false CRC-valid payloads across conditions, not just detections. Keep reference files immutable.
2. **Frequency-offset robustness:** reproduce the recorded +1 kHz failure, inspect acquisition and demodulation evidence, then test any fix on additional offsets and independent captures. Do not tune only to these three fixtures.
3. **Wi-Fi attack validation:** add an offline packet adapter for AWID3 after access is available. Preserve capture timestamps and label uncertainty. Test supported rules against attack and benign periods; report unsupported attack classes explicitly. Packet evidence must not be marked as RF-validated or given invented continuity/FCS provenance.
4. **Named drone links:** audit new data for actual protocol/link-mode ground truth, controller direction, sessions, units and negative examples. Keep augmented derivatives with their parent capture. Use an independent test split before enabling any named-family acceptance. Model-name filenames alone are insufficient. Mini 2 live testing remains postponed.

These changes improve repeatability and expose a real decoder robustness gap. They do not establish complete link identification or a new field-accuracy percentage. No bulk datasets were downloaded.
