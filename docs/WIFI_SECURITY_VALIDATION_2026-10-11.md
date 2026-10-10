# Wi-Fi attack detection validation — 11 October 2026

Attack detection is the active priority. Named drone-link validation is next, with ML, Remote ID and Mini 2 live testing still deferred.

## Completed offline validation

The current production replay executable was checked against all seven phase checkpoints and the final snapshot of the 2 October conducted HackRF/X310 campaign. All eight matched the saved semantic state exactly (excluding UI recent-tail retention and recorder byte accounting). The baseline remained incident-free; historical beacon replay and all four disconnect-flood paths advanced their expected incidents. The retry control neither added nor renewed an incident. The final state retained 1,114 accepted frames and three coalesced incidents.

`tools/audit_wifi_security_campaign.py` provides the repeatable, radio-free audit. It checks expected path advancement, negative controls, capture health and input immutability, and records input/executable SHA-256 hashes. It rejects an output path within the campaign and refuses to overwrite a previous report. Inputs are read-only; original recordings were unchanged.

The first audit-run implementation incorrectly expected the beacon rule to populate a timeline like the flood rule. Its report is retained as `campaign-audit.json`. Beacon observations store their path on the incident itself. The corrected audit checks that current path and increasing observation count; all phases pass in `campaign-audit-verified.json`. No detector rule was modified to obtain this result.

Added 31 constructed event-stream cases through the production CLI with default thresholds:

- 24 reconnect/retry controls: single-capture disconnect bursts, sparse reconnect-like traffic and retry-only trains, each with one/four targets at four window phases. All remain incident-free, with every supplied frame accepted and no flood capture exclusions.
- Seven beacon cases: normal sequence wrap, repeats without a newer beacon, restart into a new TSF epoch, equal old TSF with different sequence, historical replay, Retry-marked historical replay, and a restart reproducing identical old state.
- Historical replay and an indistinguishable identical-state restart retain **suspected** alerts with the restart explanation. A Retry bit does not hide a genuine historical A–B–A pattern.

These cases model selected observations. They do not establish all legitimate reconnect storms are distinguishable from malicious floods. Sustained high disconnect rates may still correctly generate a suspected flood observation regardless of intent.

All eight affected suites passed: parser, events, baseline, flood, beacon replay, capture provenance, scanner Wi-Fi lane and replay CLI. The CLI checks also retain positive repeated/AP-wide paths and event re-export equality. No detector thresholds or production behavior were changed.

## Benign hardware recording regression

Replayed the prior 36-capture receive-only recording: 2,550 FCS-valid frames, 35.983372 analysed seconds, zero incidents, one burst-limited capture excluded. All semantic fields except flood diagnostics matched the saved snapshot. The snapshot used flood policy v1 (17 evaluations); today's rolling-window v2 performs 33 evaluations with the same exclusion. This difference is recorded, not omitted as an exact match.

This short, previously inspected sample is development evidence. Zero alerts here is not an independently measured false-alert rate. No sensitivity, specificity, false-alarm rate or RF-start-to-alert latency is claimed.

Artifacts: `data/security_validation/2026-10-11/campaign-audit-verified.json` and `benign-regression.json`. The source campaign and receive-only recording remain in their original directories. No hardware was opened, transmitter operated, firmware flashed, monitor restarted or source recording modified.

## Next acceptance gates

1. Gather longer benign fixed-channel sessions across normal usage, reconnects, AP restarts and configuration changes; retain independent AP/client logs and divide calibration from untouched evaluation sessions.
2. Measure false alerts per usable observed hour, incident onset latency and rule recall separately from decoder/receiver yield. Preserve coverage exclusions and avoid counting multiple views of one incident as independent trials.
3. Extend controlled positive reception checks to DSSS, additional OFDM rates/channels and receiver conditions. The existing conducted campaign covers legacy OFDM 6 Mbps on channel 11 only.
4. Use endpoint logs to assess actual service impact and PMF behavior. Passive headers alone cannot establish authenticated attacker identity or client disruption.

The next useful connection is the USRP for a coordinated receive-only baseline collection. No transmission is needed for that stage. Authentication/association flooding, encrypted-data replay, RF jamming and LoRa attack rules remain separate implementation work.
