# Connection and EAPOL replay-like anomalies

Implemented 11 October 2026 as an additive passive security rule. This is experimental observation software, not authenticated replay detection or evidence of client disruption. Existing disconnect-flood and historical-beacon policies are unchanged.

## Supported observations

- Open-system authentication requests and successful-status responses (algorithm 0, transactions 1/2).
- Association and reassociation request/response exchanges with readable bodies and successful response status.
- Supported plaintext RSN EAPOL-Key exchanges: descriptor type 2, descriptor versions 1–3, consistent 16-byte-MIC layout, pairwise M1/M2/M3/M4 flag patterns and correct AP/client direction. MIC bytes are not verified. WPA descriptor 254, group-key handshakes, ambiguous layouts, SAE/FT/shared-key authentication, protected bodies, aggregation and fragmented frames are outside this first rule.

Management detection requires a byte-identical historical request (FCS omitted, Retry masked) to return after a newer request of the same kind has an observed response. Both the original and newer exchanges must have responses, and the repeated request must have Retry clear and be at least 0.5 seconds after the original. Ordinary repetitions without a newer answered exchange do not trigger. Sequence progression is tracked modulo 4096; an unexplained backward/new sequence resets history rather than proving replay.

EAPOL comparison uses exact EAPOL bytes, so a new MAC sequence number does not hide repeated handshake content. AP requests and station responses are paired by inferred stage and replay counter. M3 requires a preceding answered M1 in retained history and matching ANonce. A historical request can trigger only after a newer, higher-counter request of the same or later stage has an observed response, with the same ANonce context and Retry clear. A new ANonce, reconnect, observed disconnect, missing continuity or expiry breaks comparison. An ordinary same-counter M3/M4 retry is not a positive.

Neither rule validates the sender, response authenticity, key installation or attack success. Delayed retransmissions, reordering, missed session changes and firmware state reuse remain explanations. Alerts have low qualitative confidence and the `suspected` tier. These thresholds are development policy, not calibrated probabilities. Excluding Retry-marked historical requests is a deliberate conservative coverage limit in this new rule; the separate historical-beacon rule retains its existing behavior.

## Integration and safeguards

The production scanner already supplies FCS-checked supported-PHY MPDUs. The new state rule consumes that existing stream; it does not alter decoding, reception, scanning or RF transmission. Live and offline consumers share the same implementation. Existing incident persistence handles these incidents; transaction comparison history is intentionally not restored across process restarts.

The entire capture is validated before transaction state or incidents are updated. It requires device timestamps agreeing with sample anchors, matching receiver/session metadata, in-range nonempty sample spans, ordered events, exact submitted counts and healthy capture coverage. Loss, malformed/rejected input, overflows, caps, unsupported relevant transaction input, clock regressions and capacity overruns prevent comparison. Unsupported relevant input conservatively excludes the whole capture for this rule; it is not evidence of quiet. This can reduce availability in mixed traffic and is visible in diagnostics. Existing flood/beacon recovery semantics remain separate.

Default limits: 32 pending captures, 256 relevant frames per capture, eight receiver profiles, 128 link/category contexts per profile, eight exchanges per context, 2,048 bytes per input frame, 30-second retention, one-second maximum inter-capture gap. Memory/history evictions and unsupported inputs are counted. Longer recordings do not imply unbounded comparison history.

Snapshot JSON adds `transactions`. The shared Wi-Fi security panel shows experimental rule status, evaluated/excluded counts, unsupported input and history-eviction diagnostics. Incident details show exact counter strings, age thresholds, original/newer requests and responses, and the repeated request (five evidence references). Stored packet excerpts remain bounded by the existing incident store.

## Validation and delivery

`tests/test_wifi_security_transactions.py` exercises the production CLI using independently constructed packet bytes, CRCs and explicitly synthetic timestamps. Coverage includes historical authentication/association/reassociation, benign retries/reconnects, missing responses, session/profile changes, loss, timestamp disagreement, overflow, fragments/FCS, capacity overflow, RSN handshake ordering and nonce changes, unsupported/truncated formats, large 64-bit counters and duplicate ingestion.

All 42 integration test suites passed after the final build, including the 74 transaction cases. All eight conducted-campaign checkpoints reproduced legacy semantic fields exactly; additive transaction diagnostics are retained separately in the audit. The prior benign recording still accepted 2,550 frames with zero incidents and no changes to legacy fields. It contained zero eligible new-rule transactions, so it does not measure the new rules' false-alarm behavior.

The actual GUI rendered a synthetic EAPOL incident and opened its evidence dialog through an automated interaction. The screenshot was visually inspected. Artifacts are under `data/security_validation/2026-10-11/transactions/`, including input/snapshot fixtures, hardware and benign regression reports, and GUI images. The fixture is explicitly labelled synthetic.

These are event-level software tests; no positive RF qualification for the new rules is claimed. The earlier conducted hardware corpus contains beacon/disconnect cases and cannot validate EAPOL/authentication attack recall. It is used to ensure existing incidents and baseline behavior remain unchanged. The audit tool explicitly retains the new diagnostics while comparing every legacy semantic field against the older snapshots.

Build output is staged in `build/cyclostationary-integration-check/`. The running monitor and primary binaries were not replaced. Activate a matched app/helper build during the next normal restart; this workspace also contains the earlier cyclostationary wire-v11 change.

## Remaining scope

Encrypted-data PN replay, group-key EAPOL replay, protected-management IPN replay, application/drone-command replay, authenticated MIC validation and LoRaWAN replay remain unimplemented. Wider authentication methods and real-world false-alarm/recall calibration require separate evidence and testing. The next hardware task is receive-only observation of owned AP/client connections with independent endpoint logs before any controlled positive transmission campaign.

Protocol field reference: [hostap/wpa_supplicant EAPOL-Key definitions](https://w1.fi/wpa_supplicant/devel/wpa__common_8h_source.html). Flag-based message names are observations, not proof of negotiated security context; the existing parser and new rule explicitly restrict ambiguous cases.
