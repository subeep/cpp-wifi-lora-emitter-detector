"""Prepare the A3 class review; does not authorize training or choose splits."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
from .universal_inventory import TYPES, read_json

EXCLUDE = {'locked_unreadable', 'unresolved_or_mixed_identity', 'mixed_recording_not_clean_model_truth',
           'possible_extracted_duplicate', 'archive_XML_label_conflict', 'missing_pack_metadata',
           'pack_filename_mismatch_requires_review', 'excluded_format_and_zero_tail_unresolved',
           'Mavic_archive_model_not_confirmed_by_local_README', 'DAUTEL_alias_requires_review',
           'below_deployment_bandwidth', 'capture_not_ok', 'missing_file'}


def build(inventory):
    groups = defaultdict(list)
    excluded = Counter()
    for row in inventory['records']:
        reasons = sorted(EXCLUDE.intersection(row['flags']))
        if not row['model']: reasons.append('unresolved_model')
        if reasons:
            excluded.update(reasons)
            continue
        groups[row['model']].append(row)
    classes = []
    for model, rows in sorted(groups.items()):
        types = {r['signal_type'] for r in rows}
        if len(types) != 1: raise ValueError('conflicting model signal types: '+model)
        known = sorted({r['aircraft'] for r in rows if r['aircraft'] and 'physical_unit_unverified' not in r['flags']})
        unknown = sorted({r['aircraft'] for r in rows if r['aircraft'] and 'physical_unit_unverified' in r['flags']})
        # Different dataset names do not establish different physical aircraft. Eligibility
        # requires three explicit unit identities within at least one source.
        units_by_source = defaultdict(set)
        for row in rows:
            if row['aircraft'] in known:
                units_by_source[row['dataset']].add(row['aircraft'])
        if model == 'not_a_drone':
            flag, claim = None, 'Background scene only; no aircraft identity claim.'
        elif any(len(units) >= 3 for units in units_by_source.values()):
            flag, claim = 'multi_aircraft', 'Eligible for two training aircraft plus one held-out aircraft; B4 must enforce the split.'
        else:
            flag, claim = 'single_unit', 'Conservative unit-match reporting only; no unseen-aircraft accuracy claim.'
        classes.append(dict(model=model, signal_type=next(iter(types)), reporting_flag=flag,
                            reporting_claim=claim, known_aircraft=known, unresolved_unit_groups=unknown,
                            recordings=len(rows), sources=dict(Counter(r['dataset'] for r in rows)),
                            type_reasons=sorted({r['type_reason'] for r in rows}),
                            remaining_flags=sorted({f for r in rows for f in r['flags']}),
                            review_required=True, training_eligible=False))
    return dict(schema='rfmon.drone_ml.universal_classes.v1', status='awaiting_user_review',
                signal_types=list(TYPES), classes=classes, excluded_reason_counts=dict(excluded),
                reporting_note='single_unit is proposed as a conservative reporting tier, including unresolved cross-dataset units and the two-unit Mini 3 Pro; it does not assert exactly one physical aircraft.',
                split_chosen=False, training_authorized=False)


def markdown(data, inventory):
    s = inventory['summary']
    lines = ['# Universal classifier: A2 inventory and A3 class review', '', 'Date: 2026-11-12. **A3 awaits user review; no training has started.**', '',
             '## Inventory', '', f"The manifest lists {s['recordings']:,} recording entries, including {s['locked_unreadable']} locked DRFF-R2 entries marked unreadable. Counts include excluded entries; they are not training counts.", '',
             '| Source | Entries |', '|---|---:|']
    lines += [f'| {k} | {v:,} |' for k,v in s['by_source'].items()]
    lines += ['', 'Metadata comes from existing audited metadata (checked against source size and modification time), current archive directories, and recorder sidecars. No IQ was opened. Locked files were only listed/stat-ed; the guard rejects direct paths, symlink aliases, hardlink aliases and locked member names before opening.', '',
              'FPV entries stay excluded and their receiver/rate/centre remain unknown. DroneRFa is still an incomplete download, so it has a separate pending entry. Live checks on 12 November did not retain IQ and cannot supply training recordings. Derived windows are excluded from the original-recording inventory.', '',
              '## Proposed classes', '',
              'Seven signal types are retained. `analogue_video` and `other_drone` currently have no admitted candidate model. Type assignments below are proposals, not per-window signal truth. B2 still has to measure presence; controller-only captures must not inherit the aircraft OFDM type without that evidence.', '',
              '**Reporting proposal:** use `multi_aircraft` only where at least three explicitly identified, non-locked aircraft exist. Use `single_unit` as a conservative unit-match reporting tier for the rest. This includes Mini 3 Pro (two known aircraft), and models shared across datasets whose physical independence is unverified. This is a clarification of the plan’s two flags and needs your review; it does not claim those classes contain exactly one aircraft.', '',
              '| Model | Proposed type | Reporting tier | Known aircraft / unresolved groups | Candidate entries | Sources |',
              '|---|---|---|---:|---:|---|']
    for c in data['classes']:
        lines.append(f"| {c['model']} | {c['signal_type']} | {c['reporting_flag'] or 'background'} | {len(c['known_aircraft'])} / {len(c['unresolved_unit_groups'])} | {c['recordings']} | {', '.join(c['sources'])} |")
    lines += ['', '## Reasons for each assignment', '']
    for c in data['classes']:
        lines += [f"- **{c['model']} → {c['signal_type']}:** {' '.join(c['type_reasons'])}"]
    lines += ['', '## Exclusions and decisions needed', '',
              '- RFUAV archive/XML label conflicts are excluded; no filename is silently preferred over the XML.',
              '- Nine Mini 3 IQ members have a pack basename that differs from their folder’s XML. Their folder metadata is recorded, but they are excluded pending verification.',
              '- The 60 Leuven Mavic entries are excluded from the draft class list: their exact model is not established by the local README. Autel/DAUTEL alias entries are also excluded pending verification.',
              '- The 71 extracted Leuven files remain listed but are excluded as possible duplicates of archived recordings.',
              '- Mixed DRFF recordings are excluded from clean model candidates. DRFF environment files remain absence-unverified background candidates.',
              '- X310 scene approval is incomplete. The 10 November background session cannot yet be treated as approved negative truth; the pilot session is unlabelled. Existing October sessions also need scene review.',
              '- Aircraft from different datasets must not be counted as independent merely because the dataset names differ.',
              '- DroneRFb E is proposed as Mini 3 (code 87), G as Air 2S (66); A/C/D/F retain anonymous code classes. Sources: plan sections 3–4 and the local DroneID report.', '',
              'Please review the proposed class/type assignments, exclusions, and the conservative meaning of `single_unit`. A3 remains unchecked until reviewed. A4 must then preserve whole sessions and prior partitions; a session must never be split to manufacture independent training and evaluation background.', '',
              '## Reproduction', '', '```bash',
              'python3 -m tools.drone_ml.universal_inventory --output /tmp/universal-inventory.json',
              'python3 -m tools.drone_ml.universal_classes --inventory /tmp/universal-inventory.json --output /tmp/universal-classes.json --review /tmp/universal-review.md',
              'python3 -m unittest tests/test_drone_ml_universal_inventory.py', '```', '']
    return '\n'.join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inventory', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--review', type=Path, required=True)
    a = p.parse_args(); inventory = read_json(a.inventory); result = build(inventory)
    a.output.write_text(json.dumps(result, indent=2)+'\n')
    a.review.write_text(markdown(result, inventory))
    print(json.dumps(dict(classes=len(result['classes']), tiers=dict(Counter(c['reporting_flag'] or 'background' for c in result['classes'])))))


if __name__ == '__main__': main()
