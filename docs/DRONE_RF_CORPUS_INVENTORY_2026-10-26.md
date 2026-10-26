# Versioned corpus inventory and label expansion — 26 October 2026

Created corpus inventory version **2026-10-26.01**, covering 755 saved recording/member/context entries. Expanded the scoped supervision export from one to **18 checksum-verified recording-level units across ten drone models**. These are whole-recording drone/controller-system labels, not individual packet, crop or aircraft-emitter labels.

The dataset drive was initially unavailable and was remounted during this work. The 18 admitted Tampere training files were then read once for paired MD5/SHA-256 verification. No IQ was decoded, no original files or frozen partitions changed, no tensors were created, and no production model was trained or promoted. The existing application remains unchanged.

## Verified recording-level supervision

The [Tampere publisher record, version 1.0.1](https://zenodo.org/records/4264467), describes isolated-chamber drone/controller recordings and provides file checksums. All 18 files already admitted by the frozen review matched their published MD5 values. New SHA-256 hashes pin their contents, and current file stat tuples were preserved across reading. The canonical model names were separately reconciled against the publisher list.

| Model context | Verified recordings |
|---|---:|
| DJI Inspire 2 | 3 |
| DJI Matrice 100 | 1 |
| DJI Matrice 210 | 3 |
| DJI Mavic Mini | 1 |
| DJI Mavic Pro | 1 |
| DJI Phantom 4 | 1 |
| DJI Phantom 4 Pro Plus | 3 |
| Parrot Disco | 1 |
| Parrot Mambo | 1 |
| Yuneec Typhoon H | 3 |
| **Total** | **18** |

These are ten model contexts, not ten proven independent physical-aircraft groups or a newly trained ten-class classifier. Mavic Mini is distinct from Mini 2; this step does not newly qualify Mini 2 recognition. Mini 2 remains represented in the legacy inventory and diagnostic material, awaiting the expanded scope-aware corpus review.

The publisher lists 19 IQ files. `Parrot_mambo_control_2G.bin` remains outside this export because the existing review excluded it from aircraft-model training as controller-only data. Its checksum and prior exclusion are recorded for reconciliation; it was not hashed or silently admitted during this run. A later controller/activity task can revisit its label semantics without changing the historical experiment.

Split recordings are not treated as consecutive. Whole-recording labels may support future multiple-instance training, but they cannot be copied into every window. Transmitter endpoint and physical-aircraft independence remain unresolved. [Publisher scope and continuity description](https://zenodo.org/records/4264467).

## Inventory coverage

`tools/drone_ml/corpus_inventory.py` validates the old review against its frozen split plan, verifies selected source files, builds a separate inventory, and calls the scoped-label exporter. The inventory includes:

| Metadata source | Entries | Current use |
|---|---:|---|
| KULeuven DroneRF | 64 | Existing recording contexts; expanded evidence/scope review pending |
| Tampere DroneRF | 18 | Verified recording-level bags, training partition only |
| RFUAV | 9 | Existing recording contexts |
| DRFF-R2 | 426 | Original training/calibration/test metadata preserved |
| Diagnostic derivatives | 8 | Review only; never independent evaluation |
| NIST Wi-Fi captures | 180 | Unlabelled background-context stress review |
| FPV analog sessions | 48 | Format and label scope unresolved |
| Local target-on/off contexts | 2 | Context only; no scene-wide truth established |
| **Total** | **755** | Mixed metadata units, not 755 aircraft or independent recordings |

Only the 18 Tampere files received fresh source-content verification here. Other source hashes/metadata, where present, are inherited and explicitly marked as not reverified. Archive-member identity, recording groups, source paths and available lineage remain visible. The inventory is not the legacy per-window training manifest.

## Split and leakage audit

The original 517 entries retain their assignments:

| Original partition | Entries |
|---|---:|
| Train | 331 |
| Known calibration | 36 |
| Unknown calibration | 44 |
| Locked known test | 24 |
| Locked unknown test | 82 |

All 42 declared groups remain unchanged. No duplicate logical recording/member was found across partitions, and declared group labels/partitions agree with the frozen plan. This checks metadata consistency, not physical-unit independence. Different members of the same archive are not automatically the same recording.

The eight derivatives retain parent lineage where resolvable and are explicitly ineligible for independent evaluation, even when their diagnostic partition is called `development_only`. They must not be counted as new held-out positives. Two earlier reserved-source plot exposures (`mavic3C` and `mavic3S`) remain disclosed in the split audit; their raw test IQ was not opened or moved into training.

The legacy inventory has 21 training-context classes. Only **Air 2, Air 2S and Mini 4 Pro** have existing separate known-calibration groups; the other 18 lack that coverage. These group assignments are inherited from previous metadata, not newly validated aircraft independence. All newly verified Tampere bags stay in training; there is no new independent calibration or test set for them.

## Background evaluation is still incomplete

No verified negative label was created in this step:

- **NIST:** 180 capture entries in one conservative collection group, all development-only. Their Wi-Fi collection context supports stress review but does not independently establish drone absence. They are not 180 independent environments.
- **FPV:** 48 sessions in two conservative environment groups. Ten sessions declare the target absent, across those same two groups. All ten previously audited first-block probes showed zero tails, and those probes were at 100 MHz. Neither relevant-band format correctness nor scene-wide drone absence follows from them. This run did not reread FPV IQ or trim/concatenate any blocks.
- **Local:** target/controller state is useful context, but does not identify every nearby emitter or establish scene-wide absence.

These are limits of the reviewed evidence, not a claim that no useful negatives exist anywhere on the drive. Unlabelled RF data can support later self-supervised or weak-supervision work, provided it is not scored as confirmed negative truth. There is currently no justified specificity/false-alarm-rate estimate from this new manifest.

## Deliverables

The separate scoped export contains 18 recording declarations and per-record evidence references. It requires a scope-aware trainer and fresh source-hash checks before future IQ extraction. It does not contain STFT tensors or enable legacy per-window training.

- [Inventory](../data/drone_ml/2026-10-26/corpus_inventory_01/inventory.json) and [summary](../data/drone_ml/2026-10-26/corpus_inventory_01/summary.json).
- [Source verification receipts](../data/drone_ml/2026-10-26/corpus_inventory_01/source_verification.json) and [publisher/model reconciliation](../data/drone_ml/2026-10-26/corpus_inventory_01/publisher_label_reconciliation.json).
- [Split audit](../data/drone_ml/2026-10-26/corpus_inventory_01/split_audit.json) and [background audit](../data/drone_ml/2026-10-26/corpus_inventory_01/background_audit.json).
- [Scoped supervision units](../data/drone_ml/2026-10-26/corpus_inventory_01_scoped_units/scoped_units.json) and [acceptance decisions](../data/drone_ml/2026-10-26/corpus_inventory_01_scoped_units/decisions.json).
- [Extraction requirements](../data/drone_ml/2026-10-26/corpus_inventory_01/extraction_plan.json).
- [Corpus lock](../data/drone_ml/2026-10-26/corpus_inventory_01/corpus.lock.json), pinning inventory, evidence, audits and export artifacts by SHA-256.
- [Verification](../data/drone_ml/2026-10-26/corpus_inventory_01/verification.json): **86 tests passed**, including eight new inventory tests. Existing checkpoint hashes match prior records. The existing one-epoch synthetic training test uses temporary fixtures only.

The tests cover scope preservation, reserved-source rejection, frozen split/label consistency, missing/duplicate/mismatched verification receipts, derivative dependence, exclusion preservation, catalog/format mismatch, checksum failure and source-stat preservation.

## Next work

1. Reconcile the Leuven/Mini 2 and DRFF-R2 publisher evidence with the same recording-level contract, prioritizing model/role aliases and independent physical-unit groups. Do not expand accepted labels merely from filename resemblance.
2. Resolve the FPV writer format, valid sample counts and relevant-band blocks. Preserve target-absence scope and missing-continuity masks. Add controlled no-drone sessions for independent rejection evaluation when available.
3. Freeze a numerical preprocessing contract for full-band/context inputs plus optional strong-core views, keeping recording-bag targets separate from instance features and preserving selector provenance. The context/localizer experiments do not themselves supply packet truth.
4. Build the numerical corpus only after those scope and preprocessing checks. Compare presence/rejection and identity models under the same group splits before starting the larger training run.

This step expanded trustworthy recording-level supervision and made the remaining blockers explicit. It did not fix the current live model's false drone alarms.

## Reproduction

From the repository root, with the dataset drive mounted:

```bash
data/drone_ml/runtime-2026-10-14/bin/python -m tools.drone_ml.corpus_inventory \
  --review data/drone_ml/2026-10-20/expanded_v2/review/review.json \
  --plan data/drone_ml/2026-10-20/expanded_v2/review/split_plan.json \
  --audit data/drone_ml/2026-10-24/recognition_review_01/review.json \
  --catalog data/drone_ml/2026-10-26/corpus_inputs_01/tampere_catalog.json \
  --local data/drone_ml/2026-10-25/supervision_inputs_01/contract.json \
  --output /tmp/drone-corpus-inventory-NEW
```

This produces the core inventory, source receipts and scoped export in new directories. The linked supplemental audits and lock capture the completed review of this particular run. Existing output directories are never overwritten.
