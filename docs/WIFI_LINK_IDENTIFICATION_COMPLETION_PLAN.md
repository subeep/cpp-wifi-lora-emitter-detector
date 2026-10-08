# Wi-Fi-band link identification: bounded completion plan

Date: 8 October 2026. Status: proposed implementation and acceptance plan; documentation only.

## Outcome and stopping point

Complete the current DSP segment by delivering a versioned, explainable link-identification module in the existing Wi-Fi GUI. It must recognise a declared supported subset of observed links, retain useful waveform information for other signals, and abstain when evidence is insufficient. Exact drone-model identification belongs to the later ML stage.

This plan narrows the next delivery from indefinite waveform expansion to a small validated link catalogue. It does not claim perfect identification of every transmission in a band. It preserves the broader roadmap and its release gates; completing this segment does not complete the original multi-family M4/M8 breadth targets.

The minimum functional release scope is:

1. Ordinary Wi-Fi protocol evidence for supported captured formats, using correctly associated existing decoder facts where available and qualified DSP evidence otherwise. Generic OFDM alone never earns a Wi-Fi protocol label.
2. At least one independently validated drone-link family/mode, chosen by reference quality and measurable distinguishability. The user's Mini 2 is the preferred collection opportunity, not proof that its exact protocol generation can be distinguished. A recording labelled Mini 2 supplies device context; it does not by itself establish a unique link signature.
3. Unknown, ambiguous, mixed-signal, partial-coverage and insufficient-evidence outcomes. Generic RC/video compatibility remains distinct from drone attribution.
4. Evidence, support scope, coverage and freshness in both Wi-Fi panels, plus a stable handoff record for future ML.

A second drone/RC/video family is optional for this segment and must not delay the first validated release. If the first family cannot be separated from its confusables, the result remains a waveform candidate and the validation milestone remains open. Do not manufacture a family name or rename a device-associated pattern to satisfy the release count.

## Current baseline

Delivered experimental measurements include ordinary/conjugate cyclic features, local cyclic-background checks, general CP timing, fixed Barker-11 compatibility, bounded timing-grid refinement, burst-local analysis, frequency/envelope morphology and linear-sweep checks. Offline candidate filtering exists; live candidate filtering remains unqualified. The optional worker and GUI exist. Named link/drone acceptance and cross-capture activity history remain unfinished.

The latest software full-budget jobs took about 229–328 ms. The existing live design admits at most four independent 65,536-sample tiles, 262,144 samples total, and one bounded burst analysis per tile. The child has a 128 MiB address-space cap, a two-second request deadline and bounded transport/queue limits. These are software facts, not live receiver acceptance.

Preserve current source/runtime default-off behaviour, radio ownership, scan schedule, acquisition settings, packet/security/identity semantics and existing stores. All additional DSP uses private copies in the isolated worker. A documentation plan does not authorise a transmitter test or a monitor restart.

## Decision contract

Return separate fields rather than a single forced label:

| Axis | Examples / required meaning |
|---|---|
| Observation | Signal observed; no supported signal evidence; observation unavailable |
| Waveform | Supported OFDM structure, code-compatible spreading, FSK-like, chirp-like, unresolved |
| Link | Validated catalogue entry, experimental candidate, ambiguous, unknown |
| Drone relevance | Unknown; compatible with drone use; supported drone-link association |
| Quality and coverage | Valid usable observation, partial bandwidth, short duration, overlap, clipping, gaps, budget omissions |
| Time | Observation timestamp, result age, current/historical/stale state |

“No supported signal evidence” is not proof of absence. Correlation scores are measurements, not posterior probabilities. Protocol recognition cannot establish aircraft/controller role or physical flight state without separate validation. Unknown waveform or link does not mean non-drone.

## Milestones

| Milestone | Deliverable | Exit requirement | Connection / labelled drone needed? |
|---|---|---|---|
| L0 — Scope and reference contract | Supported-profile matrix, first-family shortlist, input manifest and sealed evaluation split | Exact labels, required evidence, negatives, operating envelope and evaluation rules frozen | No for audit; missing references identified |
| L1 — Observation and candidate qualification | Bounded candidate preparation, quality/coverage record and selection report | Correct coordinates; documented selection misses; useful weak/unknown path; measured cost | Offline first; USRP needed for receiver qualification |
| L2 — Reference-driven signatures | Frozen deterministic family checks and ordinary-traffic alternatives | Distinguishing evidence on development sources; adversarial/impairment checks; no held-test tuning | Labelled recordings required; live drone not needed if references suffice |
| L3 — Link decision and bounded history | Open-set catalogue decision, evidence fusion and conservative re-observation | Unknown/conflict tests pass; no unsupported source association; full-search errors measured | Replay first |
| L4 — GUI and future-ML handoff | Link summary, reason/evidence details, freshness/coverage and versioned records | Both bands display correct states; stable schema; no forced ML gate | No hardware required for UI/schema checks |
| L5 — Independent identification validation | Sealed per-family and unknown-class report | Accuracy/coverage targets below met for at least one drone-link family/mode | Independent labelled drone references needed |
| L6 — Live performance and segment closure | USRP off/on/off report, endurance report, capability ledger and release note | Isolation/performance gates pass; L0–L5 complete; limitations and unsupported scope explicit | USRP required; representative live background and later drone sessions |

L0 starts first. Reference acquisition, negative-corpus work and receiver qualification begin early rather than waiting for L4. L4 can use simulated results while L2/L3 mature. L5 evaluates the frozen L1–L3 pipeline; any tuning after exposure requires a new held evaluation set. L6 uses the same frozen pipeline and repeats affected gates after changes.

### L0 — Freeze a small, testable scope

- Audit the local DroneDetect files, existing Mini 2 recording and other already available sources. Resolve format, true sample rate, usable bandwidth, discontinuities, capture settings, device/mode labels, unit identity and session independence. Preserve unknown metadata explicitly.
- Split by original recording, session, device and receiver where available before window extraction. Keep all crops, resampled copies, noise variants and replay derivatives of a source together. Development-exposed recordings cannot become an independent sealed test later.
- Select the first family by available mode-level truth and separability, not popularity. Prefer the Mini 2 collection route if adequate evidence is obtainable. Keep its previously requested live test deferred until the user is ready.
- Include ordinary Wi-Fi access points/clients/cameras, non-drone RC/video where relevant, other drone families, noise, tones, receiver artifacts and overlapping signals. Interference-labelled drone recordings are not drone-free negatives.
- Freeze a supported receiver/profile envelope: actual tune windows, usable passband, sample rates, minimum contiguous duration, quality/SNR conditions and live selection budget. Nominal band membership does not establish visibility.
- Record a per-entry evidence requirement and a negative/unknown roster. This prevents later thresholds or labels from being chosen around favourable examples.

Output: reference manifest, profile matrix and evaluation specification. No new DSP branch is required solely to complete this audit.

### L1 — Qualify the observations entering identification

- Evaluate native versus privately downmixed/filtered/decimated candidates using independent coordinate/filter checks. Preserve sample provenance, filter transients and continuity; never stitch separate bursts or gaps for coherent analysis.
- Route only a bounded number of candidates. Retain background/context exploration so weak or unfamiliar signals are not excluded simply because current cyclic features fail.
- Measure selection misses using exhaustive offline reference coverage versus the unchanged live budget. Report the effect of the strongest-burst policy and any proposed fairness change.
- Qualify receiver artifacts, passband edges and clipping where calibration permits. Mark unknown analog coverage or scaling as unknown. Account for overlaps without assuming one spectral region equals one transmitter.
- Add expensive paths only after demonstrating useful coverage within the combined budget. The two-second watchdog is a fault limit, not the desired result-update interval.

Output: qualified observation record and resource/coverage comparison. Receiver measurements can begin with background RF; a drone is unnecessary for that part.

### L2 — Build only signatures needed by the first catalogue

For each entry, specify expected and competing evidence: symbol/CP timing, chip/code structure, carrier/rate consistency, occupied bandwidth, burst/preamble structure and any protocol-validated facts. Use combinations supported by references rather than a single correlation maximum.

Priorities:

1. Strengthen timing/carrier/channel robustness for the first family's actual waveform and its closest confusables.
2. Add protocol/preamble/header confirmation when feasible and relevant. Existing decoder evidence must be tied to the same capture/time/frequency; an unrelated nearby Wi-Fi packet cannot label a candidate.
3. Add a small PN/Gold/Barker or other code bank only if references show the bank addresses a real gap. CCK, FSK, chirp or other branches follow the same evidence-driven rule.
4. Test wrong codes, near timings, ordinary traffic, short observations, low signal quality, partial passband, frequency/sample-clock error, echoes and mixtures.
5. Freeze all discovery-selected parameters before within-capture held checks. Separately reserve independent devices/sessions for L5; disjoint windows of one recording are not independent deployment validation.

Do not expand all waveform groups to finish this segment. Keep unneeded branches experimental and document their limitations.

Output: versioned signature specifications, implementation plan and development report, including failed hypotheses and indistinguishable families. An unresolvable pair must stay ambiguous or use a genuinely defensible broader family label.

### L3 — Make conservative link decisions and re-observations

- Apply input eligibility, then waveform evidence, then applicable catalogue checks. Evaluate alternatives and conflicts before accepting a named link.
- Preserve unknown candidates. Do not require a strong DSP drone verdict before the future model can inspect a signal.
- Calibrate the maximum over the whole searched pipeline, including candidate choice, code/timing/carrier searches and history. Several features from the same IQ are correlated evidence and cannot be multiplied as independent probabilities.
- Add small bounded histories for qualified re-observations. Associate only compatible frequency/time/quality/signature observations and keep ambiguous associations separate.
- Distinguish observed frequency changes from validated hopping. Never infer a complete hop sequence, transmitter continuity or departure across scan gaps.
- Keep full multi-emitter tracking, physical role inference and flight-state inference outside this closure scope.

Output: deterministic, versioned decision policy with supported, candidate, ambiguous, unknown and unavailable states; bounded histories and replay tests.

### L4 — Show the result and prepare the ML boundary

Both Wi-Fi panels should show link label/status, waveform, observed frequency/bandwidth, observation age, supported profile and the strongest supporting/competing reasons. Detail views retain raw measurements, incomplete coverage, skipped work and worker health. Unsupported results remain visible without appearing as validated labels.

Define a handoff record containing analysis/schema/catalogue versions, quality and missing-feature masks, waveform/link candidates, timestamps, receiver settings, source coordinates and reproducible IQ references or bounded optional snippets. Do not silently enable continuous recording. Audit and low-rate unknown/exploration samples should remain available by policy so later ML does not inherit only this stage's confident positives.

Output: tested GUI states and a stable export/replay contract. No ML training, model inference or Remote ID dependency is included.

### L5 — Pass independent identification tests

Use the original roadmap's targets as proposed acceptance criteria, frozen at L0:

| Measurement | Proposed acceptance / required report |
|---|---|
| Accepted named-link precision | At least 99% point estimate and at least 95% lower confidence bound per advertised family/profile; define the confidence method in advance |
| Conditional drone-link detection recall | At least 95% within the declared supported operating envelope, with uncertainty; count eligible observations rejected by the quality policy as misses rather than quietly removing them |
| Observation and encounter coverage | Report unconditional encounter recall under the actual sweep separately from conditional recall; include skipped IQ/jobs and blind time |
| Unknown rejection | Test whole unseen families and confusable ordinary devices; report their incorrect known-label rate by group, not only pooled accuracy |
| Drone false alerts | Retain the candidate operational target of at most one false drone assessment per eight monitored hours, with uncertainty and predefined episode grouping |
| Acceptance coverage | Report accepted, unknown, ambiguous and unavailable fractions; high precision achieved by accepting very little must be obvious |

Report known-link errors separately from drone-attribution errors. Plain Wi-Fi or generic RC/video recognition cannot count as successful drone attribution. Event/session dependence must be reflected in confidence intervals; adjacent windows do not constitute independent encounters.

Follow the broader pilot requirement of at least three physical units and three sessions per family as a collection starting point, not sufficient statistical evidence by itself. If only one Mini 2 is available, unit generalisation remains unverified. Plan 48–96 representative non-drone background hours across two environments as in the existing roadmap, and increase exposure if confidence or clustering requires it. Neither synthetic controls nor a short perfect demonstration waives these requirements.

Output: sealed report, confusion/unknown tables, failure examples, coverage accounting and the supported family/profile ledger. Insufficient references leave this milestone pending; they do not prevent completing software preparation.

### L6 — Demonstrate live operation and close the segment

- Run matched USRP baseline/off/on/off trials for every advertised receiver profile, measuring overflows/gaps, scan cycle, GUI latency, CPU/RSS, producer-copy time, analysis latency, queue drops and observation-to-result age.
- Retain the existing proposed limits: no attributable new receiver overflows or security losses, at most 2% median/p95 scan-cycle increase and at most 5% p95 GUI-frame increase. Freeze an end-to-end result-age/update target per profile at L0 using the actual scan cadence; a worker-only timing target is insufficient.
- Exercise disable/enable, band changes, reconnects, worker crash/hang/malformed replies and overload. Overload must remain bounded and disclosed; optional analysis cannot backpressure the receiver.
- Complete the existing 72-hour endurance gate for the advertised profile, including freshness and bounded-memory checks.
- Publish reproducible versions, supported scope, unsupported families/modes, resource measurements, validation gaps and rollback/disable instructions.

Output: live acceptance report and segment release note. No receiver retuning/scheduling changes or resource-cap increases are assumed by this plan.

## Work packages and resource needs

Recommended next implementation package: L0 manifest/profile contract plus L1 candidate/quality qualification, with the first family's reference adequacy assessed immediately. This prevents another sequence of broad DSP additions without progress toward identity.

Next: L2 focused signatures and L3 open-set decision. Then L4 GUI/handoff integration, followed by L5/L6 validation using frozen versions. Begin USRP background and performance collection as soon as a suitable receiver window is available; it can overlap offline development. A new spreading-code bank is not the automatic next task.

Connections needed:

- None for the immediate manifest, offline replay, decision-contract and GUI planning work.
- USRP for real receiver/background qualification and live performance, coordinated around existing monitor use.
- Later labelled Mini 2/controller sessions and independent units or trustworthy equivalent recordings for drone-link validation. Document actual mode, firmware, unit/session and controller/aircraft state; collect target-off and ordinary-traffic controls under matched conditions.
- Additional link equipment only when the selected catalogue entry lacks references. Do not collect every listed drone family to finish the minimum segment.

L0 should produce the effort estimate after the reference audit. Calendar completion is data-dependent; do not promise a validated family release while independent reference availability is unresolved.

## Definition of done

**Software-ready checkpoint:** L0–L4 delivered with replay/resource tests and explicit experimental labels. Useful and reviewable, but not completion of validated drone-link identification.

**Segment complete:** L0–L6 delivered; ordinary-traffic alternatives work; at least one drone-link family/mode passes independent identification gates; unknown/conflict/coverage handling works; both GUI panels expose the evidence; live receiver performance and endurance pass; the future ML handoff is versioned. Any unresolved gate is listed as pending rather than silently waived.

After this stopping point, pause general waveform expansion. Start exact-model ML as a separate authorised project using the preserved references and split policy. Broader families, extra bands, full tracking, role/flight-state inference and Remote ID remain separate extensions.

## Related records

- [Broader integration roadmap](WIFI_CYCLOSTATIONARY_DRONE_ADDON_PLAN.md)
- [Current coverage and qualification evidence](WIFI_CYCLOSTATIONARY_WAVEFORM_COVERAGE.md)
- [Implementation record](WIFI_CYCLOSTATIONARY_IMPLEMENTATION.md)
- [Reference validation record](WIFI_CYCLOSTATIONARY_REFERENCE_VALIDATION.md)
- [General DSP next steps](WIFI_CYCLOSTATIONARY_NEXT_STEPS.md)
