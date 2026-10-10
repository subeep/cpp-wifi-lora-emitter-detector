# Candidate routing and scoped Wi-Fi protocol evidence

Implemented 8 October; verification closed 9 October 2026. Partial L1/L2/L4 delivery under the [completion plan](WIFI_LINK_IDENTIFICATION_COMPLETION_PLAN.md). ML and Remote ID remain deferred. No radio was opened or reconfigured, and no running monitor was restarted.

## Delivered

The offline replay tool accepts `--prepare-candidates --route-candidates [--candidate-round N]`. It retains the strongest proposal and explores another region by explicit round, subject to existing geometry and budget checks. It reduces decimation when needed to retain the 2,064-sample cyclic-analysis support target. This is sample-count support, not sufficient physical duration or a successful signature. Two-candidate and 64-million FIR-operation limits remain. Defaults are unchanged. Routing remains offline; rounds are stateless and do not establish persistent live coverage across changing captures.

The optional live add-on now reuses existing Wi-Fi decoder results after the existing pipeline. No additional decoding or payload copying occurs. It considers at most 512 descriptors, retains at most two facts per sampled tile and eight per job, and requires the full decoder search window to fit one contiguous sampled tile. Session, capture, band, rate, centre and prefix must match. Invalid/stale metadata is discarded without rejecting the DSP job. Child wire version 10 and all process limits remain unchanged.

Both Wi-Fi GUI panels can show “Wi-Fi observed in this window,” the PHY/rate/channel and supporting sample window. Accepted modes are existing legacy OFDM and long-preamble 1 Mbps DSSS, with existing preamble/header/completeness/frame-CRC checks. The existing OFDM decoder's operating rate range remains 20–64 Msps; higher-rate metadata tests do not qualify decoding at those rates. Absence of associated evidence does not mean absence of Wi-Fi. A CRC-checked frame is protocol evidence, not source authentication, emitter separation, non-drone proof or proprietary drone-link identity.

## Verification

- Targeted address/undefined-behaviour sanitizer tests: 2/2 passed (protocol and waveform checks; leak detection disabled).
- Usual GUI and matching worker rebuilt; default-off build and scanner-lane regression passed. No monitor restart. [Verification record](../data/wifi_drone_analysis/link_routing_protocol_gui_2026-10-08/verification.json).
- Standalone: 18/18 tests passed. Integration: 32/32 tests passed, including scanner regression, panel states, worker lifecycle and new protocol tests.
- Protocol tests exercise three received Wi-Fi fixtures from the existing development session, plus invalid CRC/header/payload, partial-window, band, frequency, session/capture and bounded-metadata cases. These reuse the existing decoder and are not an independent PHY oracle. DSSS association uses constructed metadata; no new received DSSS validation is claimed.
- [Routing benchmark](../data/wifi_drone_analysis/candidate_routing_2026-10-08/summary.json): 90 cases from prior qualification, every default field exact against the pre-change binary, and every source window/stat unchanged. All fields outside candidate preparation/qualification remain exact with routing enabled. Cyclic-supported candidates total 45 before and 64 in round zero; these are repeated development cases, not independent detection trials.
- In the 65,536-sample three-burst control, round zero selects regions overlapping bursts one/two and round one selects one/three. This demonstrates routing, not detection recall. Short bursts can still lack support.
- [GUI preview](../data/wifi_drone_analysis/link_routing_protocol_gui_2026-10-08/panel.png) was generated from actual ImGui draw output and visually checked. Its protocol metadata is a synthetic display fixture.

The benchmark script is `tools/cyclostationary/benchmark_candidate_routing.py`; its report records binary, script and input-case hashes. Software scenarios include 20/56/100/200 Msps. The user's 10 GbE NIC/SFP removes the historical transport ceiling from planning; actual sample metadata still governs analysis, and no hardware throughput claim follows from offline tests.

## Still pending

L1 needs live fair routing, gap-aware revisit history and calibrated usable passband/coverage. L2 needs independently labelled drone-link references and focused signatures with competing-protocol controls. L3 needs an open-set decision contract; L4 needs the versioned future-ML handoff. L5 independent identification accuracy and L6 live performance/endurance remain unqualified. Existing Mini 2 data remains development-only and does not enable a named DJI link claim.

No new connection is needed for the next software work. USRP performance/background qualification and later labelled Mini 2 sessions remain separate validation steps. The add-on stays runtime default-off and does not alter existing packet/security/identity decisions.
