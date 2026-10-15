# GUI revamp — field and action ledger

Baseline: working tree recorded in `GUI_REVAMP_BASELINE.json`, including the existing uncommitted add-ons. This ledger accompanies the complete rendering-function review; dynamic JSON/telemetry and array-defined columns are accounted for below. No backend schema was changed.

## Destinations and complete field groups

| Original domain | Current destination | Preservation / interaction |
|---|---|---|
| Device / Connect / recording checkbox | Monitoring setup, also accessible from Settings | Original callback bodies retained; configured recording still applies on reconnect; startup disconnected |
| Band / channel / lock / capture duration / lab / threshold / AGC / gain | Monitoring setup | Explicit settings only; page selection is separate; ranges and setter semantics retained |
| Status / overflow / RX stalled / acquisition step | Persistent shell | Material health warnings always visible; processing lane details under Overview |
| Frequency ranges / emitter columns / source lower bound | Wi-Fi or LoRa RF observations; active map also Overview | All 10 original columns remain; secondary columns in header menu; relative power and provisional grouping labelled |
| Wi-Fi packet 23 original columns | Wi-Fi → Packets | All original columns available; retained search and FCS filter; frozen selected snapshot includes exact available measurements, gate and integrity state |
| Wi-Fi master 13 columns / popup fields | Wi-Fi → Identities & RF clusters | All original columns available; selected inspector preserves vendor provenance, SSID/WPS ages/sources, security/capabilities, counts, timestamps and latest RF values; popup fallback remains |
| LoRa packet 31 columns / outcome/tooltips | LoRa → PHY packets; same renderer in offline replay | All columns retained; frozen hypothesis inspector includes every LoraPacketRow member, uncertainty, limits, gate, payload and candidate detail |
| LoRa master 11 columns | LoRa → Master emitters | Original columns/values retained; source cluster qualification and bounded history corrected |
| Five Security views / frame popup / incident modal | Security; modal fallback retained for old callers | Incidents first; current main uses inline evidence. Full rule counters in diagnostics. Recent selection uses ingest key instead of shifting index |
| Baseline freeze/unfreeze/reset | Security → Baselines | Existing command callback and confirmation preserved verbatim; display navigation never invokes it |
| Save next LoRa / replay panel | Captures; shortcut on LoRa | Same callback/path/input checks and replay worker; no new decoder or disk catalogue |
| Cyclostationary measurement groups | Wi-Fi → Experimental link analysis | Existing draw_shadow_panel retained unchanged; enable remains explicit; measured/unknown/unsupported source scope preserved |
| DJI live fields and telemetry | Drones → DJI live broadcasts | All telemetry rows grouped by their existing group, including unknown groups; full JSON copy/raw display and metadata retained; source/history qualifiers retained |
| Remote ID normalized message keys | Drones → Wi-Fi Remote ID | Every message key remains in complete field table; raw hex/source/full JSON retained; source selection uses backend key and ages remain per message |
| DJI offline inputs and results | Drones → Offline IQ review; shortcut from Captures | Same Addon load callback, actual metadata and bounds; no live-state publishing |

## Explicit action contract

| Action | Existing operation preserved |
|---|---|
| Connect/Reconnect | Stop if started; choose device; reset existing gain/AGC UI defaults; configure existing recording path; start scanner |
| Disconnect | Stop scanner and clear existing scanner_started state |
| Acquisition band / Monitor this band | Clear fixed Wi-Fi channel only when switching bands; call existing set_active_band |
| Wi-Fi channel | Existing set_wifi_fixed_channel, including Sweep → nullopt |
| LoRa lock / frequency | Existing optional set_lora_lock_freq |
| LoRa duration / lab | Existing duration setter/readback and laboratory setter |
| Threshold / AGC / manual gain | Existing setters, ranges and optional-gain semantics |
| Capture save / replay | Existing request_lora_capture_save / existing replay function |
| Security baseline controls | Existing action/key callback and reset dialog; baseline function unchanged |
| Cyclostationary enable | Existing set_wifi_cyclo_enabled |
| DJI enable/disable | Existing start/stop and counter reset behavior |
| Remote ID enable/disable | Existing remote_id.enable behavior |
| Offline DJI Decode interval | Existing Addon.load with path/rate/centre/offset |
| Copy evidence JSON | Existing clipboard-only actions; no file export |
| Navigate / filter / select / inspect / scale / collapse / legacy presentation | UI state only |

## Original literal controls and columns

Each original literal below is retained in its destination above, sometimes with clarified presentation text. Optional table columns are available through right-clicking a header. This is an inventory of original controls, not a claim that every original label must remain identical.

| Source | Baseline line | Kind | Original label |
|---|---|---|---|
| `src/main.cpp` | 134 | TableSetupColumn | Freq (MHz) |
| `src/main.cpp` | 137 | TableSetupColumn | Ch |
| `src/main.cpp` | 138 | TableSetupColumn | BW (kHz) |
| `src/main.cpp` | 139 | TableSetupColumn | Protocol guess |
| `src/main.cpp` | 140 | TableSetupColumn | Sources |
| `src/main.cpp` | 141 | TableSetupColumn | Power (dB, rel.) |
| `src/main.cpp` | 142 | TableSetupColumn | Cycles seen |
| `src/main.cpp` | 143 | TableSetupColumn | Age (s) |
| `src/main.cpp` | 144 | TableSetupColumn | Last seen (s ago) |
| `src/main.cpp` | 145 | TableSetupColumn | Band |
| `src/main.cpp` | 234 | TableSetupColumn | Device ID |
| `src/main.cpp` | 235 | TableSetupColumn | First seen |
| `src/main.cpp` | 236 | TableSetupColumn | Last seen |
| `src/main.cpp` | 237 | TableSetupColumn | Readings |
| `src/main.cpp` | 238 | TableSetupColumn | Freq (MHz) |
| `src/main.cpp` | 239 | TableSetupColumn | SF |
| `src/main.cpp` | 240 | TableSetupColumn | BW (kHz) |
| `src/main.cpp` | 241 | TableSetupColumn | IRR (dB) |
| `src/main.cpp` | 242 | TableSetupColumn | IQ eps |
| `src/main.cpp` | 243 | TableSetupColumn | IQ phi (deg) |
| `src/main.cpp` | 244 | TableSetupColumn | DC (dBc) |
| `src/main.cpp` | 420 | Button | Disconnect |
| `src/main.cpp` | 430 | Checkbox | Record Wi-Fi security events |
| `src/main.cpp` | 479 | BeginTabItem | Monitoring |
| `src/main.cpp` | 532 | Checkbox | Lock to frequency |
| `src/main.cpp` | 539 | InputFloat | MHz##lora_lock |
| `src/main.cpp` | 550 | InputFloat | LoRa capture seconds (1-30) |
| `src/main.cpp` | 558 | Checkbox | Legacy laboratory codecs (nonstandard) |
| `src/main.cpp` | 568 | CollapsingHeader | Save a LoRa IQ capture |
| `src/main.cpp` | 570 | InputText | Save under |
| `src/main.cpp` | 572 | Button | Save next completed capture |
| `src/main.cpp` | 585 | SliderFloat | Threshold (dB above noise floor) |
| `src/main.cpp` | 591 | Checkbox | AGC |
| `src/main.cpp` | 598 | Checkbox | AGC (not supported on this radio) |
| `src/main.cpp` | 604 | SliderFloat | Gain (dB) |
| `src/main.cpp` | 665 | CollapsingHeader | Detected Wi-Fi packets |
| `src/main.cpp` | 690 | CollapsingHeader | Drone / link analysis (experimental cyclostationary DSP) |
| `src/main.cpp` | 694 | Checkbox | Enable optional DSP analysis |
| `src/main.cpp` | 702 | CollapsingHeader | Wi-Fi Identities & RF Clusters (persistent) |
| `src/main.cpp` | 714 | CollapsingHeader | LoRa PHY packets (Sub-GHz IN865 channels) |
| `src/main.cpp` | 724 | CollapsingHeader | LoRa Master Emitters (persistent across restarts) |
| `src/main.cpp` | 737 | BeginTabItem | Drones |
| `src/wifi_gui.cpp` | 20 | TableSetupColumn | Time |
| `src/wifi_gui.cpp` | 21 | TableSetupColumn | Freq (MHz) |
| `src/wifi_gui.cpp` | 22 | TableSetupColumn | Ch |
| `src/wifi_gui.cpp` | 23 | TableSetupColumn | Identity key |
| `src/wifi_gui.cpp` | 24 | TableSetupColumn | Network / SSID |
| `src/wifi_gui.cpp` | 25 | TableSetupColumn | AP ch |
| `src/wifi_gui.cpp` | 26 | TableSetupColumn | Advertised security |
| `src/wifi_gui.cpp` | 27 | TableSetupColumn | Decode status |
| `src/wifi_gui.cpp` | 31 | TableSetupColumn | Frame type |
| `src/wifi_gui.cpp` | 32 | TableSetupColumn | OFDM Mbps |
| `src/wifi_gui.cpp` | 33 | TableSetupColumn | PSDU bytes |
| `src/wifi_gui.cpp` | 34 | TableSetupColumn | Modulation |
| `src/wifi_gui.cpp` | 35 | TableSetupColumn | Power (dB) |
| `src/wifi_gui.cpp` | 36 | TableSetupColumn | BW (MHz) |
| `src/wifi_gui.cpp` | 37 | TableSetupColumn | Duration (us) |
| `src/wifi_gui.cpp` | 38 | TableSetupColumn | Confidence |
| `src/wifi_gui.cpp` | 45 | TableSetupColumn | CFO (ppm) |
| `src/wifi_gui.cpp` | 46 | TableSetupColumn | IRR (dB) |
| `src/wifi_gui.cpp` | 47 | TableSetupColumn | IQ eps |
| `src/wifi_gui.cpp` | 48 | TableSetupColumn | IQ phi (deg) |
| `src/wifi_gui.cpp` | 49 | TableSetupColumn | DC (dBc) |
| `src/wifi_gui.cpp` | 50 | TableSetupColumn | EVM (%) |
| `src/wifi_gui.cpp` | 51 | TableSetupColumn | Sync corr |
| `src/wifi_gui.cpp` | 248 | Button | Close |
| `src/lora_gui.cpp` | 10 | CollapsingHeader | LoRa security monitoring |
| `src/lora_gui.cpp` | 63 | TableSetupColumn | Time |
| `src/lora_gui.cpp` | 64 | TableSetupColumn | Decode outcome |
| `src/lora_gui.cpp` | 65 | TableSetupColumn | Decoder |
| `src/lora_gui.cpp` | 66 | TableSetupColumn | Header |
| `src/lora_gui.cpp` | 67 | TableSetupColumn | Freq (MHz) |
| `src/lora_gui.cpp` | 68 | TableSetupColumn | SF |
| `src/lora_gui.cpp` | 69 | TableSetupColumn | BW guess (kHz) |
| `src/lora_gui.cpp` | 70 | TableSetupColumn | CR |
| `src/lora_gui.cpp` | 71 | TableSetupColumn | LDRO |
| `src/lora_gui.cpp` | 72 | TableSetupColumn | Sync observed |
| `src/lora_gui.cpp` | 73 | TableSetupColumn | Header len |
| `src/lora_gui.cpp` | 74 | TableSetupColumn | Payload CRC |
| `src/lora_gui.cpp` | 75 | TableSetupColumn | CFO (bins) |
| `src/lora_gui.cpp` | 80 | TableSetupColumn | SNR (dB) |
| `src/lora_gui.cpp` | 85 | TableSetupColumn | EVM (%) |
| `src/lora_gui.cpp` | 86 | TableSetupColumn | Sync corr |
| `src/lora_gui.cpp` | 87 | TableSetupColumn | CFO (ppm) |
| `src/lora_gui.cpp` | 88 | TableSetupColumn | IRR (dB) |
| `src/lora_gui.cpp` | 89 | TableSetupColumn | IQ eps |
| `src/lora_gui.cpp` | 90 | TableSetupColumn | IQ phi (deg) |
| `src/lora_gui.cpp` | 91 | TableSetupColumn | DC (dBc) |
| `src/lora_gui.cpp` | 92 | TableSetupColumn | DC ang (deg) |
| `src/lora_gui.cpp` | 93 | TableSetupColumn | Payload |
| `src/lora_gui.cpp` | 94 | TableSetupColumn | IQ polarity |
| `src/lora_gui.cpp` | 95 | TableSetupColumn | Preamble peak |
| `src/lora_gui.cpp` | 96 | TableSetupColumn | SFD peak |
| `src/lora_gui.cpp` | 97 | TableSetupColumn | CFO estimate (Hz) |
| `src/lora_gui.cpp` | 98 | TableSetupColumn | Drift (Hz/symbol) |
| `src/lora_gui.cpp` | 99 | TableSetupColumn | Capture offset (s) |
| `src/lora_gui.cpp` | 100 | TableSetupColumn | FEC mismatches |
| `src/lora_gui.cpp` | 101 | TableSetupColumn | LoRaWAN structure |
| `src/lora_gui.cpp` | 228 | CollapsingHeader | Offline LoRa capture replay |
| `src/lora_gui.cpp` | 239 | InputText | Capture directory |
| `src/lora_gui.cpp` | 241 | Checkbox | Legacy laboratory codecs (nonstandard)##replay |
| `src/lora_gui.cpp` | 246 | Button | Replay capture |
| `src/security_gui.cpp` | 140 | TableSetupColumn | Frame type |
| `src/security_gui.cpp` | 141 | TableSetupColumn | Frames |
| `src/security_gui.cpp` | 142 | TableSetupColumn | Share |
| `src/security_gui.cpp` | 235 | Checkbox | Management frames only |
| `src/security_gui.cpp` | 237 | Checkbox | Hide control frames (ACK/RTS/CTS/Block Ack) |
| `src/security_gui.cpp` | 299 | Button | Close |
| `src/security_gui.cpp` | 311 | Button | Unfreeze learning |
| `src/security_gui.cpp` | 312 | Button | Freeze learning |
| `src/security_gui.cpp` | 316 | Button | Reset all baselines... |
| `src/security_gui.cpp` | 327 | Button | Reset |
| `src/security_gui.cpp` | 329 | Button | Cancel |
| `src/security_gui.cpp` | 478 | CollapsingHeader | Benign alternatives |
| `src/security_gui.cpp` | 480 | CollapsingHeader | Rate timeline |
| `src/security_gui.cpp` | 494 | CollapsingHeader | First and latest evidence |
| `src/security_gui.cpp` | 508 | Button | Close |
| `src/security_gui.cpp` | 641 | BeginTabItem | Coverage |
| `src/security_gui.cpp` | 645 | BeginTabItem | Frame types |
| `src/security_gui.cpp` | 646 | BeginTabItem | Recent frames |
| `src/security_gui.cpp` | 647 | BeginTabItem | Baselines |
| `src/security_gui.cpp` | 648 | BeginTabItem | Incidents |
| `src/drones/panel.cpp` | 24 | TableSetupColumn | Reported serial |
| `src/drones/panel.cpp` | 25 | TableSetupColumn | Reported model |
| `src/drones/panel.cpp` | 26 | TableSetupColumn | RX centre (MHz) |
| `src/drones/panel.cpp` | 27 | TableSetupColumn | Packets |
| `src/drones/panel.cpp` | 28 | TableSetupColumn | Freshness |
| `src/drones/panel.cpp` | 54 | Button | Copy complete decoded packet JSON |
| `src/drones/panel.cpp` | 56 | TableSetupColumn | Group |
| `src/drones/panel.cpp` | 57 | TableSetupColumn | Field |
| `src/drones/panel.cpp` | 58 | TableSetupColumn | Value |
| `src/drones/panel.cpp` | 59 | TableSetupColumn | Validity / interpretation |
| `src/drones/panel.cpp` | 69 | CollapsingHeader | Raw decoded packet JSON |
| `src/drones/panel.cpp` | 98 | Button | Copy Remote ID evidence JSON |
| `src/drones/panel.cpp` | 110 | TableSetupColumn | Field |
| `src/drones/panel.cpp` | 110 | TableSetupColumn | Reported value |
| `src/drones/panel.cpp` | 126 | BeginTabItem | DJI live broadcasts |
| `src/drones/panel.cpp` | 127 | Button | Disable DJI add-on |
| `src/drones/panel.cpp` | 128 | Button | Enable DJI add-on |
| `src/drones/panel.cpp` | 136 | BeginTabItem | Wi-Fi Remote ID |
| `src/drones/panel.cpp` | 138 | Checkbox | Enable Wi-Fi OpenDroneID |
| `src/drones/panel.cpp` | 147 | BeginTabItem | Offline IQ review |
| `src/drones/panel.cpp` | 150 | InputText | ##IQ path |
| `src/drones/panel.cpp` | 151 | InputDouble | Sample rate (Hz) |
| `src/drones/panel.cpp` | 152 | InputDouble | Centre frequency (Hz) |
| `src/drones/panel.cpp` | 153 | InputDouble | Offset (seconds) |
| `src/drones/panel.cpp` | 154 | Button | Decode interval |

## Array-defined and dynamic information

- Wi-Fi identities: all 13 original array-defined columns and complete BeaconInfo/WPS/latest-reading inspector fields remain in the same rendering function.
- Security: original coverage (17), frame types (3), recent frames (13), baseline columns and incidents (12) remain; the original evidence body, exact counter strings, context, timeline and raw excerpts are reused. Rule summaries/configuration tooltips are retained in the diagnostics drawer.
- DJI: every original Snapshot counter displayed by the previous renderer remains; all TelemetryField group/name/value/status rows are generated dynamically, and raw unknown payload keys remain in packet JSON. No normalized telemetry field was removed.
- Remote ID: message iteration retains every key except source/raw_hex in the primary field table; those keys have their dedicated provenance/raw view. Full source JSON remains copyable. No message-field whitelist was substituted.
- Source-only fields not previously published by a GUI contract are not invented. Full dataset/history or exact normal-packet ingest lookup is unavailable.

## Verification evidence

Production shell mouse navigation/setup opening, all display scales, layout switching and explicit-command count are tested in `test_gui_shell`. Existing offscreen Wi-Fi tests retain their prior assertions and now exercise production shell/inline identity, packet and incident inspectors with zero passive baseline commands. Existing LoRa and DJI tests exercise their actual production renderers; DJI GUI errors now fail the test. Full regression/build results and limitations are recorded in `GUI_REVAMP_IMPLEMENTATION.md`.
