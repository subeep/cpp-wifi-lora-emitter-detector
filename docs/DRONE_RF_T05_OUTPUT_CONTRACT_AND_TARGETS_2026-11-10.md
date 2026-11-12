# T05 — output contract and operating-target decision sheet

10 November 2026. Part 1 restates the output contract the code will implement. Part 2 is the decision sheet for Sudeep (T31). **No target value in this document is chosen**; the sheet gives definitions, what the evidence so far implies, and what each choice costs. Numbers marked "estimate" have not been measured.

## Part 1 — output contract

Each observation publishes six independent fields. Every field carries its own evidence reference, observation time, availability time, revision number and qualification status.

| Field | Values | Rule |
|---|---|---|
| Observation quality | valid / partial / unusable | Carries observed band, time and coverage. Unusable can never produce a fresh "none detected". |
| Activity | none detected / drone-system activity / uncertain | "None detected" only for a valid, sufficiently covered observation inside the qualified band, time and sensitivity envelope. Controller-only activity is activity, with role reported separately; airborne status stays unknown. |
| Family | accepted / candidate / unresolved | With evidence scope. Useful, but not a compulsory gate for a model claim. |
| Model | accepted / classifier candidates / unresolved | Accepted requires accepted activity and a class that passed independent-unit qualification. |
| Decoded identity | reported identity with message provenance | Kept separate from the RF-inferred model, including when they disagree. |
| Conflict | yes / no | When qualified sources disagree, inferred identity is unresolved and both claims are kept. |

Candidate lists come in three kinds that must not be mixed: catalog-compatible models (reference information, with stated completeness), observation-specific classifier candidates (labelled experimental until calibrated), and a calibrated candidate set (requires measured true-model inclusion on independent units, including unfamiliar models).

Version one scope: one observation per capture and frequency region; no cross-capture tracking.

## Part 2 — decision sheet for T31

### How the evidence so far constrains the choices

| Fact | Source | Consequence |
|---|---|---|
| Verified no-drone recordings: zero | Readiness report, 29 October | No false-event rate can be stated today, for any threshold. |
| X310 link limit: 25 MSps clean, 33.3 MSps overflows | Measured 4 November on the 1 GbE link | One capture sees about 20 MHz. A 40 MHz link is seen in part. Wider needs 10 GbE. |
| Unseen-aircraft accuracy, three models: 68–72% of windows, 78–83% by recording | Validation, 22 October | Accepting every top guess would be wrong roughly one time in four to five. |
| Rejection policy: accepts 31% of known windows at 1.9% unknown false acceptance | Same, diagnostic policy | Identity coverage and wrong-identity rate trade steeply against each other. |
| Pairwise accuracy on unseen aircraft: 0.69–0.87 depending on training units | Same-family diagnostic, 10 November | Results depend on which aircraft were trained; more units per model matter more than architecture. |
| Storage free for recordings: about 1.3 TB | `/media/sudeep/Storage`, 10 November | Bounds total recorded hours (see D1). |

### D1. Maximum false drone events per monitored background hour

- **Definition proposed:** one event is one or more alerts for the same frequency region merged within a fixed interval (the interval is D4). A monitored hour is one wall-clock hour of the scanner running its normal visit schedule, not one hour of IQ.
- **What it costs:** with zero false events observed in T hours, the 95% upper bound on the true rate is about 3 / T per hour.

| Rate you want to be able to claim | Zero-alarm hours needed in the sealed final set |
|---|---:|
| 1 per hour | 3 |
| 0.5 per hour | 6 |
| 0.1 per hour | 30 |
| 1 per day | 72 |

- **Storage:** scan-style recording measured 39 GB per wall-clock hour on 10 November (nine centres, 1 s each, 63 s per cycle, about 6% of segments lost to dropped samples). The 1.3 TB free holds about 33 such hours in total, shared between training, calibration, development and the sealed final set. A claim below roughly 0.3 per hour therefore needs more disk, shorter captures, or a way to count hours without storing all IQ.
- **Decide:** the rate, and whether it applies per band or to the whole schedule.

### D2. Minimum encounter recall, by condition

- **Definition proposed:** an encounter is one continuous period with a drone system active in the scene. It is recalled if at least one activity alert overlaps it.
- **Constraint:** recall can only be measured for conditions you record. The first positive recordings can cover linked ground operation and controller-only at a few distances; flight needs an authorised flight.
- **Decide:** which conditions matter (range, line of sight, busy Wi-Fi, controller-only), and the minimum recall for each.

### D3. Maximum first-alert delay

- **Constraint:** the delay cannot be shorter than the revisit interval of the visit schedule for that band, plus processing. Persistence rules (requiring two detections) add at least one more revisit. The actual revisit interval of the coordinated schedule on the X310 has not been measured for this purpose; the pilot session will measure it.
- **Decide:** the maximum delay, knowing it sets how many bands can share the schedule.

### D4. Identity acceptance: coverage and wrong-identity rate

- **Definitions proposed:** coverage is the fraction of true encounters of a qualified model that receive an accepted model name. Wrong-identity rate is the fraction of accepted names that are wrong, counted per encounter, including encounters with models the system was not trained on.
- **Constraint:** on current evidence these pull hard against each other, and only three models have any independent-unit evidence. A strict wrong-identity limit will leave most encounters as "model unresolved" or as a candidate list.
- **Decide:** the maximum wrong-identity rate, the minimum useful coverage, and whether a candidate list (for example two or three names) is an acceptable output when a single name is not supported.

### D5. Scan bands and revisit schedule

- **Options:** 2.4 GHz and 5.8 GHz only; add 5.1–5.2 GHz; add the 2399.5 MHz DroneID visit (already in the default schedule).
- **Constraint:** every added centre lengthens the revisit interval for all others (D3) and adds storage (D1).
- **Decide:** the bands and centres. The recording recipe uses a nine-centre proposal that you can change before the first session.

### D6. Resource limits

- **Decide:** the largest acceptable loss of Wi-Fi monitoring duty when drone inference is on, and whether a GPU may be assumed at deployment. CPU inference is the plan's first target; the compact model's forward pass was about 2 ms and the residual model's about 56 ms on CPU (22 October, forward pass only).

### D7. Release scope if exact-model evidence stays thin

- **Decide:** whether a first release may ship activity and link family only, with model names disabled, if T32 is not closed. The plan requires your explicit approval for that reduced scope.

### Record of decision

To be filled in under T31: chosen values for D1–D7, date, the evaluation scope they apply to, and the reason. If a target is changed after final results are seen, a new evaluation version and fresh sealed sessions are required.
