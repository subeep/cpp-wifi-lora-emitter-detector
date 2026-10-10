# Named drone-link candidates: first reference-backed profile

Latest dataset evidence (9 October): [read-only Leuven/Tampere replay](WIFI_LEUVEN_TAMPERE_LINK_EVIDENCE.md) found recurring marker candidates in Matrice and Mavic Pro and recovered Mini 2 video candidates with bounded offline filtering. This is development evidence; named-family acceptance remains unvalidated.

9 October 2026. This delivers a named research-candidate path, not completion of validated named drone-link identification. Device-category identification also remains unfinished; it is not assumed as an input truth.

## Implemented

The bounded `named_link_research_catalogue_v1` maps three existing physical chirp measurements to DJI OcuSync-related marker profiles: approximately +135.2, +270.2 and −135.3 kHz/µs with nominal 9/18/9 MHz spans. The source is [Cwalina et al., Sensors 2025, section 2](https://pmc.ncbi.nlm.nih.gov/articles/PMC12349632/). That paper reports roughly 66.67 µs symbols and 20 ms up-chirp recurrence; it does not establish uniqueness against all other transmitters or every OcuSync version. The separate [physical-layer fingerprint study](https://pmc.ncbi.nlm.nih.gov/articles/PMC9460464/) distinguishes Wi-Fi, Lightbridge, OcuSync 2 and Enhanced ShockBurst in its own labelled collection; those labels do not transfer to our recordings.

The matcher reuses existing chirp/coherence/frequency-fit and quality checks. It verifies numeric template parameters, expected 32+32 µs contiguous sample support, slope error, frequency-fit residual, absence of phase unwraps and the available passband checks. File names, device labels and template label strings cannot supply identity. This adds no new IQ search, model or child-wire fields.

Both Wi-Fi GUI panels show a named research candidate only when the marker and observed-quality checks pass without a known passband rejection. Unknown passband remains a blocker. The details show unsupported profiles and missing evidence. A match can never populate the accepted drone-link field. Verified Wi-Fi and waveform ambiguity remain separate; no emitter association between their measurements is inferred.

Offline: `wifi_cyclo_replay analyze ... --assess-evidence --named-link-candidates`. The opt-in record contains catalogue/profile, match status, source support coordinates and blockers. Existing records/decisions remain unchanged. No candidate is an accepted family or an exact model; ML and Remote ID remain deferred.

## Verified

- 20 standalone and 34 integration tests passed; final panel positive-display test passed separately.
- Targeted address/undefined-behaviour sanitizer test passed (leak detection disabled).
- Nine synthetic marker controls cover all three profiles at 20/60/200 Msps. Even perfect synthetic matches remain unvalidated. Tone/noise, wrong measured slope, alias ambiguity, arbitrary labels/templates and escaping sample support are checked. These are software controls, not independent family accuracy.
- [90-case replay comparison](../data/wifi_drone_analysis/named_candidate_comparison_2026-10-09/summary.json): every default and pre-existing field unchanged, sources unchanged. Of 270 profile evaluations, 246 were no-marker-match and 24 unavailable. No positive received-marker identification was established by these sparse development windows; this is neither detection recall nor evidence of drone absence.
- [GUI preview](../data/wifi_drone_analysis/named_candidate_gui_2026-10-09/panel.png) uses synthetic display evidence and actual ImGui output, visually checked. The usual GUI/helper were rebuilt without restarting the running monitor or opening a radio.

## What prevents completion

1. **Signal evidence:** current 64 µs support does not establish a complete symbol, 20 ms recurrence or a protocol-exclusive signature. Generic chirps can match. A continuous 20 ms recurrence search cannot be inferred from separated worker tiles.
2. **Reference truth:** the existing 401-record audit has no sealed independently eligible link-validation set. Model/directory names do not establish which link mode is present in a particular window. The existing Mini 2 session cannot measure generalisation across units/sessions.
3. **Other families:** Lightbridge, other OcuSync versions, Enhanced ShockBurst and additional RC links have no implemented distinguishing signature in this catalogue. They remain unsupported, not negatives.
4. **Acceptance:** independently labelled positives and competing non-drone links, full-search false-positive calibration, receiver/passband qualification and live/endurance measurements remain necessary. No accuracy percentage is claimed.

Next concrete work is full-symbol and continuity-aware recurrence qualification for the selected marker, followed by an independently labelled OcuSync/competing-link evaluation. Later Mini 2/controller captures need actual link/mode, firmware, unit/session, RF settings and matched target-off/background records. No live test is silently substituted for the user's deferred session.
