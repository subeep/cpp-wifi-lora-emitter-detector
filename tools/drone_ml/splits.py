"""Require explicit group decisions; prevent derivative/source/class leakage."""
from collections import defaultdict

PARTITIONS = {'train', 'calibration', 'test', 'unknown_calibration', 'unknown_test', 'excluded'}


def assign(records, plan, development_only=False):
    if plan.get('schema') != 'rfmon.drone_ml.split.v1':
        raise ValueError('unsupported split plan')
    groups = plan['groups']
    if set(groups) != {r['split_group'] for r in records}:
        raise ValueError('split plan must account for every group exactly')
    assigned, partitions, labels, signatures = [], defaultdict(set), defaultdict(set), set()
    for r in records:
        decision = groups[r['split_group']]
        partition = decision['partition']
        if partition not in PARTITIONS:
            raise ValueError('unsupported partition')
        if development_only and partition in ('test', 'unknown_test'):
            raise ValueError('development run must not contain test partitions')
        if partition != 'excluded' and (not decision.get('label_evidence') or not decision.get('independence_evidence')):
            raise ValueError('eligible groups require label and independence evidence')
        signature = (r['recording_id'], r['offset_samples'], r['samples'])
        if signature in signatures:
            raise ValueError('duplicate source interval')
        signatures.add(signature)
        if partition != 'excluded':
            for key in ('recording_id', 'source_sha256', 'original_source_group'):
                partitions[(key, r[key])].add(partition)
            labels[r['label']].add(partition)
        assigned.append(dict(r, partition=partition))
    if any(len(p) > 1 for p in partitions.values()):
        raise ValueError('related original sources or derivatives cross partitions')
    known = {label for label, p in labels.items() if 'train' in p}
    if len(known) < 2:
        raise ValueError('training requires at least two known classes')
    for label, p in labels.items():
        if p & {'unknown_calibration', 'unknown_test'}:
            if p & {'train', 'calibration', 'test'} or len(p) > 1:
                raise ValueError('unknown classes must be entirely withheld and calibration/test disjoint')
        elif label not in known or not ({'train', 'calibration'} if development_only else {'train', 'calibration', 'test'}).issubset(p):
            raise ValueError('every known class needs separate train/calibration/test groups')
    required = ('unknown_calibration',) if development_only else ('unknown_calibration', 'unknown_test')
    if not all(any(r['partition'] == p for r in assigned) for p in required):
        raise ValueError('separate unknown calibration and test classes required')
    return assigned
