# Security validation — 16 October 2026

## Result

The tested detector regressions and input-rejection controls passed. A new receive-only X310 session exposed two operational limits: busy-channel analysis exclusions and bounded evidence retention. No detector thresholds or production code were changed. No RF transmissions or production-store changes were made.

## Fresh hardware session

X310 326CF02, RX2, channel 36 (5180 MHz), 20 MSps, requested gain 20 dB; requested duration 180 seconds. The radio was released after completion.

- 172 captures, 172 sampled seconds, 166.192 analysed seconds; 7.597 seconds reported acquisition dead time.
- 246,492 FCS-valid legacy OFDM frames; zero receiver overflows, failed captures, security queue drops or enrolment queue drops.
- 50 captures (29.1%) reached the software burst cap. Flood, beacon replay and transaction replay each excluded 50 captures. The remaining 122 captures are not equivalent to uninterrupted detector exposure.
- Flood rule: 100 evaluations; baseline learned 30 analysed seconds. Beacon replay: 1,589 evaluations. Transaction replay: zero eligible evaluations.
- Zero incidents observed. This is unlabelled ambient traffic, not a calibrated false-positive rate or evidence that no attack occurred. No live deauthentication/disassociation or eligible authentication/EAPOL exchanges were observed in the decoded frame-type counts.
- Enrolment persistence comparison passed for 31 observed interfaces; no storage errors.

The NIC/receiver kept up in this run. The demonstrated coverage restriction is the 4,000-burst analysis budget, not a measured network throughput failure. Simply removing that budget is not yet justified: CPU, memory and detector input capacities must remain bounded and coverage loss must stay visible.

## Evidence-retention limit

The recorder wrote 173,392,972 cumulative bytes and rotated its bounded current/previous files. At completion, approximately 39.2 MB and 51 capture records remained. Earlier evidence was overwritten by design. The final live snapshot retains session counters, but the retained files cannot reproduce the complete session. This is distinct from acquisition loss or a security queue drop.

A full-session equality claim is therefore deliberately not made for this session. Exact live/offline semantic comparisons passed on all five earlier complete recordings (40,402 total accepted frames), excluding only UI recent-tail storage and recorder byte accounting. Input hashes were unchanged.

## Offline security checks

- All 11 selected CTest suites passed, covering Wi-Fi parser/events/baselines/flood/beacon replay/transaction replay, replay CLI, capture provenance, scanner Wi-Fi integration, and existing LoRa security regressions. LoRa was not exercised on hardware.
- 74 constructed transaction replay cases passed through the production CLI: authentication, association/reassociation and EAPOL history anomalies, normal retries, interruptions, profile/session changes, missing evidence, and capacity controls. These establish tested rule behavior, not cryptographic replay verification.
- The existing saved conducted campaign passed all eight checkpoint audits: baseline, historical beacon replay, per-target distinct/repeated disconnect frames, AP-wide distinct/repeated disconnect frames, retry negative control and final state. This is regression reuse of earlier RF evidence, not a new independent attack trial.
- Parser tests passed 4,480 prefix parses and 20,000 random-buffer invariants. No sanitizer-based memory-safety certification is claimed.
- Additional offline mutations of live frame bytes: 256 one-bit-corrupted frames rejected by FCS; 256 CRC-valid incomplete headers rejected as malformed. All 512 produced zero accepted frames and zero incidents, despite metadata claiming valid FCS. Original evidence remained unchanged.
- Flood and beacon tests passed threaded/direct/serialized equivalence, exclusion handling and negative controls; detailed outputs are retained alongside this report.

## Priorities from these checks

1. Improve busy-channel security coverage with measured, bounded processing changes; retain explicit exclusion telemetry and test against the observed 4,000-burst workload. Do not turn incomplete observations into clean baseline or negative evidence.
2. Add a deliberate validation-recording mode with a configurable storage budget and complete capture-boundary archives/manifest. Keep production disk limits; surface the retained evidence horizon clearly.
3. Collect longer labelled normal-usage sessions, including legitimate reconnects and AP restarts. Current ambient runs cannot establish false alarms per representative deployment hour.
4. Use controlled endpoint/traffic evidence for attack recall and service-impact validation. USRP-only passive reception cannot manufacture labelled attack positives, authenticate transmitters, establish PMF acceptance or validate encrypted payload replay. No such claims were made.

## Artifacts

`live-summary.json`, `ambient-audit.json`, `conducted-regression.json`, `corrupt-frames.json`, `transactions.txt`, `tests.txt`, `parser.txt`, `flood.txt`, `beacon-replay.txt`, and the repeatable audit scripts are in this directory. Raw ambient recordings and per-device stores are retained locally under ignored `live36/`. No source datasets or historical campaign inputs were altered.
