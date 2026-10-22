# Project validation and improvements — 16 October 2026

## Scope

Reviewed the application workspaces, production scanner lifecycle, receive tools, persistent stores, experimental DSP panel and available test targets. Exercised connected-X310 receive paths and all 65 registered software tests. This is subsystem validation, not a claim that every function, UI interaction or RF condition has exhaustive coverage. No RF transmission was performed. No datasets were modified.

## Completed changes

- Reset the production **Wi-Fi** master list. All 1,245 existing files were moved to `data/master_backups/wifi_master-20261016-102623`; their SHA-256 hashes were verified against a manifest. The active store contains zero device records. The separate LoRa master and security history were preserved. Tests used isolated stores and did not repopulate the cleared list. A decoded address remains a claimed interface, not proof of a unique physical device; resetting the inventory does not solve that inherent distinction.
- Replaced the default DSP text dump with a spectrum plot, sampled-window map, coverage/quality/worker summaries, and three readable pattern summaries. Detailed numerical evidence remains expandable. Frequency labels adapt to column width. The panel continues to label drone identity as unknown and named matches as unvalidated research candidates. The preview image is synthetic visual QA, not a live measurement.
- Fixed stale active-band status during locked LoRa listening. The first live sequence caught `wifi_5g` still reported while LoRa captures were being collected; the corrected sequence passed.
- Fixed capture gain provenance: fixed-gain settings are read back from UHD after configuration. Gain metadata now reflects hardware rounding/clamping rather than merely the request. AGC remains represented as unknown/dynamic; old files cannot be retroactively calibrated.
- Registered 15 existing DSP/decoder/registry/master-list/bench-generator tests that previously were built but omitted from CTest. The full suite now has 65 tests.
- Added an isolated receive-only lifecycle mode to `band_smoke_test --isolated NEW_DIRECTORY`. Corrected the README's obsolete statement that the application performs no protocol decoding.

## Live hardware evidence

X310 serial 326CF02, UBX-160, RX2. The final lifecycle sequence passed all five phases:

| Phase | Observation |
|---|---|
| 2.4 GHz, channel 1, DSP enabled | Valid Wi-Fi traffic; 12 DSP results at the checkpoint; correct band/epoch |
| 5 GHz, channel 36, gain change | Valid Wi-Fi traffic; DSP total increased to 27; correct band/epoch |
| Locked LoRa, 865 MHz | Correct active-band status, four captures by checkpoint, stale Wi-Fi DSP result cleared |
| Return to 2.4 GHz, channel 6; DSP disabled | Reception resumed; DSP results cleared |
| Stop/start scanner on channel 6 | Reconnected and resumed reception without receiver error |

Final sequence: 19,038 accepted Wi-Fi frames across 64 captures, no security queue drops, no enrolment drops/storage errors. DSP checkpoints reported no worker failures, timeouts, protocol errors or busy drops. LoRa finished with five captures/10 usable seconds and no input loss or exclusions; no LoRa packets were observed.

A separate LoRa capture-save check produced four device-timed captures/8 usable seconds, no exclusions or queue loss, and a saved IQ capture that replayed successfully through the production offline decoder. Absence of packets does not validate positive LoRa decoding; archived hardware fixtures cover tested positive cases offline.

Earlier same-day reports separately cover all 22 supported Wi-Fi channels, enrolment policy/persistence and security adversarial controls. The busy-channel burst-cap and recording-retention limits in `../security_validation_2026-10-16/REPORT.md` remain; this change does not remove them.

## Feature validation matrix

| Workspace / subsystem | Tested | Remaining evidence needed |
|---|---|---|
| Overview, navigation, settings | GUI startup/render smoke test; shell regression; live band/gain lifecycle | Exhaustive manual UI interactions and alternate displays |
| RF detection / frequency views | Live Wi-Fi observations; DSP/registry synthetic tests | Ground-truth recall for weak/overlapping/intermittent emitters |
| Wi-Fi packets | Live DSSS/OFDM in prior campaign; live both bands; decoder/PHY/stress tests | Other unsupported PHYs do not become supported through these tests |
| Wi-Fi master/enrolment | Reset verified; isolated rediscovery; previous policy/reference/reload checks; master/identity tests | Long-term drift, authenticated identity, legitimate multi-BSSID/randomized addresses |
| Experimental DSP | Live worker on both bands, invalidation and disable; math/process/structure/reference tests; visual panel review | Independently validated named links; calibrated thresholds and representative negatives |
| Security | Earlier live and controlled-recording regressions; complete security suite | Labelled attacks/service impact, longer benign evaluation; busy-channel coverage improvements |
| LoRa | Live acquisition/save/replay; PHY, archived receiver and grouping tests | Live known transmitter with known payload/configuration; MIC/authenticated identity unavailable |
| Captures / offline review | Live LoRa save/replay; capture-provenance, security replay and cyclostationary CLI tests | Full-session archival on very busy channels; arbitrary corrupt/unsupported files beyond tested cases |
| DJI / Remote ID add-ons | Existing decoder/add-on/Remote ID software suites | No live labelled drone or Remote ID broadcaster tested; no new named-family validation claimed |
| Bench / waveform generators | Existing radio-free loopback test now included in CTest | Conducted/shielded TX/RX setup and explicit RF test parameters; no live bench transmission |
| Hardware lifecycle | X310 connect, retune, rate transition, gain change, stop/start | B210 unavailable; physical unplug/replug, network faults, other daughterboards and throughput limits not tested |

## Verification and artifacts

- Full build completed; **65/65 tests passed**, 92.33 seconds.
- After the LoRa status correction, scanner Wi-Fi, scanner LoRa and DSP panel tests passed again (3/3).
- Final panel render test passed; screenshot inspected. Full GUI startup with eight rendered frames exited successfully without connecting its receiver.
- `git diff --check` passed.
- Original failing lifecycle report is retained under ignored `system-live/`; corrected evidence is under ignored `system-live-fixed/`. `system-summary.json` contains aggregate results. Raw device identifiers/captures remain local and ignored.
- `master-reset.json`, build/test logs, `panel-test.txt`, and `dsp-overview.png` accompany this report. No commits or deployments were performed.

The most useful additional connections for remaining positive RF checks are a known LoRa transmitter and, later, the Mini 2/controller. No transmitter is needed for further passive stability runs. Labelled device/endpoint logs are needed to distinguish observed traffic from verified identity or actual attack impact.

Fractional gain validation: requested 20.3 dB; UHD configured readback recorded as [20.0] dB across 8 captures. This verifies gain-setting provenance, not absolute RF calibration.
