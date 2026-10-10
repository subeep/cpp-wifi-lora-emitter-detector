# Link identification: first L0/L1 implementation

8 October 2026. Scope: reference audit, metadata-driven software profiles and offline candidate qualification. This is the first implementation package of the [completion plan](WIFI_LINK_IDENTIFICATION_COMPLETION_PLAN.md). L0 and L1 remain partial; named-link acceptance remains unavailable.

## What is delivered

- `tools/cyclostationary/audit_link_references.py`: reproducible read-only reference audit, bounded head/middle/tail probes, source-group and file-alias checks, and explicit validation blockers. Uses the current conservative corpus importer, including its correction that decoded Wi-Fi fixtures are waveform confusables rather than certified drone-free environments.
- `tools/cyclostationary/link_identification_scope.json`: versioned development scope, reviewed source hashes, current tune windows, actual-capture-rate policy, candidate family collection targets and proposed release gates. Source-profile drift requires review. Historical configured rates do not limit future software support.
- `wifi_cyclo_replay analyze --prepare-candidates --qualify-candidates`: separate `candidate_qualification` JSON with proposal accounting, preparation/support status, time-extent unions, original sample coordinates, cascading-filter dependency bounds and explicit unresolved conditions.
- `tools/cyclostationary/benchmark_candidate_qualification.py`: exact comparison against the pre-change executable on synthetic and representative received inputs, with source-window hashes and unchanged-file checks.

Candidate ordering, candidate count, filter output, existing measurements and existing JSON fields are preserved. The added accounting visits only the bounded remaining proposal metadata. It does not execute more FIRs or waveform searches. Qualification is an explicit offline option requiring candidate preparation; it is not a live worker or GUI capability change. Wire version remains 10.

## Reference audit results

Final [audit report](../data/wifi_drone_analysis/link_reference_audit_10gbe_2026-10-08/audit.json):

| Source | Recordings | What these currently support |
|---|---:|---|
| Local DroneDetect | 390 | Development waveform examples; 60 Msps remains a provisional interpretation of conflicting publisher units |
| Local Zenodo references | 6 | Publisher-declared rate/format and model context; exact link-mode/unit/session truth is missing |
| Received Wi-Fi fixtures | 3 | Decoded ordinary-Wi-Fi waveform references from one session; platform/environment drone absence is not established |
| Mini 2 linked / target-off | 2 | One known development session with user-labelled target state; link protocol truth and independent validation remain missing |
| Total | 401 | 12 conservative source groups; zero sealed independent validation records |

All 401 files were readable, aligned under the declared representation, and finite in the selected probes. The audit read 39,124,992 bytes in total. This is bounded probing, not full integrity or acquisition-format verification. Probe coverage and exact sample-window hashes are saved per file. Historical full-file checksums remain labelled prior claims; the audit does not claim to recompute them.

398 records have no link-family truth, 400 have no physical-unit identity, and 399 have no session ID in the imported records. Every record is development-exposed. No automatic train/test split was created. Existing model names and operating-state labels never become accepted link labels or enter the DSP worker.

The preferred first drone collection target remains the user's Mini 2 when available. It has no accepted protocol name in the contract. Ordinary Wi-Fi remains the first concrete protocol-confusable reference. The contract preserves ambiguity until mode-level reference evidence and independent comparisons justify a link label.

## 10 GbE update

The user now has a 10 GbE NIC and SFP. The old X310 1 GbE ceiling is not a design constraint for this work. The scope file labels the reviewed 20/56 Msps configuration values historical, uses actual capture metadata for interpretation, and lists 20/25/56/60/100/120/200 Msps software scenarios. These are not hardware certification or new receiver settings.

The same bounded sample count spans less elapsed RF time at higher rates. Candidate support therefore continues to report sample count, mapped rate and missing temporal evidence, without assuming a faster transport creates more observed symbols. Future observation-duration or routing changes must be evaluated as software/coverage decisions against the actual capture metadata.

## Candidate qualification results

The final [benchmark](../data/wifi_drone_analysis/candidate_qualification_10gbe_2026-10-08/summary.json) contains 90 cases in 16 conservative source groups:

- Four synthetic kinds: tone, two-tone mixture, two-frequency signal and three gated bursts; 8,192/65,536 samples; 20/56/100/200 Msps scenarios; unknown or hypothetical declared passband.
- One representative recording from each of the 12 development source groups, plus the same-session Mini 2 target-off comparison, each with unknown/hypothetical width. This is a small software sample, not a full dataset performance evaluation.
- Hypothetical width is 80% of declared sample rate. It is explicitly not an analog bandwidth calibration and must not be used as a physical eligibility claim.

All previous JSON fields match the pre-change executable exactly in all 90 cases. Selected source hashes and file size/inode/mtime/ctime remain unchanged. There are 76 returned `passband_unknown` statuses, 64 successful preparations, 11 outside-declared-passband statuses and one filter-unavailable status. These are candidate counts across related test variants, not independent events.

19 of the 64 prepared candidates have insufficient samples for the existing cyclic partitions after decimation. Preparation success therefore does not imply adequate cyclic evidence. In the 65,536-sample three-burst control, the current two-candidate selection covers the first two burst regions and omits the third; the budget omission is now explicit. The smaller three-burst control's individual regions are below the preparation duration requirement, and no region is proposed. These tests disclose selection limitations without estimating real-world detection recall.

The new report distinguishes:

1. Retained regions, missing spectra and short regions.
2. All proposals, geometry-eligible proposals, returned proposals, duplicates of returned proposals and budget omissions. Returned + duplicate + budget-omitted counts exactly equal proposals seen. Budget omissions are not a count of distinct emitters.
3. Successful preparation versus sufficient cyclic support. Neither is an accepted waveform or link identity.
4. Union of retained/prepared region time extents, filter-input extents and output-centre extents. These count positions on the analysis grid; they are not frequency coverage, unique emitted signals or detection recall. Overlapping candidate bands are not double-counted in time unions.
5. Immediate filter-input centres mapped to original samples, plus raw dependency bounds that include an upstream analysis filter when present. No transient padding, phase concatenation or sample-rate relabelling is introduced.

Raw/context measurements stay available. Receiver passband qualification and link-signature validation remain explicit blockers. `signal_detection_recall` is null; the tool does not invent truth from the regions it selected.

## Verification

- 17 standalone and 31 integration CTests pass, including existing scanner/panel, process fault and DSP suites.
- Audit tests check bounded large-file reads, a nonfinite middle probe, short files, missing/invalid inputs, aliases, source preservation and refusal to promote development data into a sealed test.
- Candidate tests check stable ordering, complete duplicate/budget accounting, disjoint skip counts, nested filter coordinates and the distinction between preparation and cyclic support.
- Existing filter tests include known-tone amplitude/phase/frequency checks and out-of-band alias attenuation. This change does not alter the filter implementation.
- Address/undefined-behaviour sanitizer checks cover candidate processing and nested-filter qualification with leak detection disabled as in earlier repository runs.
- No radio was opened, monitor restarted, model trained or Remote ID decoded. No acquisition/transport configuration was changed.

The reproducible verification record and source/tool hashes are in [verification.json](../data/wifi_drone_analysis/link_l01_verification_2026-10-08/verification.json). Worker/GUI changes are not part of this offline package, so the existing running GUI does not gain a named-link result from this work.

## Remaining work and next package

L0 now has a reproducible development corpus/profile contract and a concrete gap report. It still needs validated mode labels, independent references, representative background, and the final event/group/confidence specification before L5. L1 now exposes preparation losses and provenance; real signal-selection recall, fair routing and receiver qualification remain open.

Next software package:

1. Define branch-specific minimum observation duration and bandwidth for the first reference family, rather than treating filter success as universal eligibility.
2. Evaluate fair bounded candidate routing against the documented weak/third-burst misses while retaining a path for unresolved activity. Compare all old measurements and both selected and missed truth intervals.
3. Specify the first ordinary-Wi-Fi protocol evidence association and first drone-link signature hypothesis from reference metadata, with confusable comparisons and no accepted identity until validated.

No further Mini 2-specific replay campaign is needed now. Existing recordings remain development examples. Live receiver/background qualification and later independent drone sessions can follow when convenient; software work does not assume the historical 1 GbE rate limit.

## Reproduction

From the repository root:

```sh
python3 tools/cyclostationary/audit_link_references.py --output /tmp/link-reference-audit-new
build/cyclostationary-offline/wifi_cyclo_replay analyze \
  --input /path/to/recording.cf32 --format cf32_le \
  --sample-rate 100000000 --samples 65536 \
  --prepare-candidates --qualify-candidates
```

The example intentionally leaves usable bandwidth unknown. Use a documented declaration only when available; declaring a width does not certify the receiver. Benchmark reproduction requires the recorded pre-change executable or equivalent source snapshot; the verification artifact preserves the compared binary hashes and development report.
