# Explicit link decisions and unknown preservation

9 October 2026. Partial L3/L4 delivery under the [completion plan](WIFI_LINK_IDENTIFICATION_COMPLETION_PLAN.md).

## Behaviour

`scoped_link_decision_v1` is a bounded, deterministic single-window policy shared by the GUI and offline replay. It keeps protocol observations, waveform state and drone-link identity separate. Its catalogue explicitly contains no independently accepted drone families; generic CP/Barker, cyclic or morphology matches cannot name a link. It neither calculates a probability nor accumulates repeated observations into confidence.

The decision states are unavailable, unknown, ambiguous-waveform/link-unknown, and protocol-observed/drone-link-unknown. Waveform detail retains unavailable, quality-rejected, coverage-rejected, competing-pattern, patterns-only and no-supported-pattern states. Concurrent blockers remain in a bounded reason list even when a display state takes precedence. Unknown receiver passband and unqualified receiver profile remain explicit.

A contained, same-capture CRC-checked Wi-Fi frame can establish a Wi-Fi protocol observation while whole-window DSP remains ambiguous or quality-rejected. This does not label other emitters in the window or establish drone relevance. Two verified Wi-Fi PHY modes are compatible protocol observations, not conflicting drone identities. The shared association helper rejects mismatched captures and unselected windows; facts from another tile are not inherited.

Both Wi-Fi panels show the link decision, with a “Link decision reasons” section listing blockers and the next evidence needed. This is guidance only; it does not retune the radio, schedule another capture, persist history or change existing security/identity decisions.

## Offline record

Use `wifi_cyclo_replay analyze ... --review-waveforms --link-decision`. The flag requires waveform review and adds a separate `rfmon.link_decision.v1` record. Existing output fields remain unchanged. It records policy/catalogue version, state/reasons, source sample coordinates and step, rate, capture centre, continuity status and explicit unknown drone/link fields. Offline analyze does not run the Wi-Fi decoder, so its protocol field remains unknown even for recordings containing Wi-Fi.

The record explicitly states that future ML eligibility is not gated by this decision. It does not implement recording, model inference or the complete future-ML handoff. Source provenance remains in the enclosing replay record; no new timestamps are inferred. Live history/export and a complete handoff contract are still pending.

## Verification and limits

- 19 standalone and 33 integration tests passed, including CLI additive-output equality, invalid-option rejection, explicit unknown/conflict/quality handling, protocol association and GUI states.
- Targeted address/undefined-behaviour sanitizer decision test passed; leak detection disabled.
- [90-case replay comparison](../data/wifi_drone_analysis/link_decision_comparison_2026-10-09/summary.json) checks default equality against the pre-routing binary, additive equality with the decision enabled, source preservation and conservative unknown identity across prior synthetic/received development cases. These are not independent detection trials.
- [Actual ImGui preview](../data/wifi_drone_analysis/link_decision_2026-10-09/panel.png) visually checked. It uses synthetic display evidence, not a live detection.
- Usual GUI and matching helper rebuilt. No radio access, transmission, receiver reconfiguration or running-monitor restart.

This is decision scaffolding, not a completed open-set classifier or validated drone-link release. Supported named-family states cannot be enabled until an independent catalogue exists. Live candidate routing, compatible re-observation history, full-search false-positive calibration, independent link validation and USRP performance/endurance remain pending. ML and Remote ID remain deferred. Software planning continues to use actual capture rate metadata rather than the historical 1 GbE ceiling.
