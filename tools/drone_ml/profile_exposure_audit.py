"""Metadata-only audit of imported DSP evaluation overlap with frozen ML splits.

No IQ is read, relabelled or moved. A match means prior exposure, not that the
profile was necessarily tuned on that row. An unmatched row is not cleared.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path

from .common import new_output, read_source, sha256, stamp, write_json


def match_rows(records, evaluated):
    index = {}
    for r in records:
        index.setdefault(os.path.normpath(r['source']), []).append(r)
    matches = []
    for n, row in enumerate(evaluated):
        path = row.get('path', '')
        source_rows = index.get(os.path.normpath(path), []) if path else []
        # Missing member metadata conservatively exposes the whole container.
        if row.get('member') is not None:
            source_rows = [r for r in source_rows if r.get('member') == row['member']]
        hits = [dict(recording_id=r['recording_id'], partition=r['partition'],
                     split_group=r['split_group'], member=r.get('member')) for r in source_rows]
        partitions = sorted({r['partition'] for r in hits})
        matches.append(dict(row=n, path=path, member=row.get('member'), matches=hits,
            partitions=partitions, locked_overlap=any(p in ('test_locked', 'unknown_test_locked') for p in partitions),
            status='prior_profile_evaluation_exposure' if hits else 'unmatched_not_cleared'))
    return matches


def audit(review, reports, output):
    paths = [Path(review)]+[Path(p) for p in reports]
    before = {str(p): dict(sha256=sha256(p), stat=stamp(p)) for p in paths}
    with read_source(paths[0]) as f:
        records = json.load(f)['records']
    sources = []
    for p in paths[1:]:
        with read_source(p) as f:
            rows = json.load(f)
        if not isinstance(rows, list):
            raise ValueError('expected an evaluation result array')
        matches = match_rows(records, rows)
        counts = Counter(part for row in matches for part in (row['partitions'] or ['unmatched']))
        sources.append(dict(path=str(p), rows=len(rows), partition_row_counts=dict(counts),
                            locked_overlap_rows=sum(r['locked_overlap'] for r in matches), records=matches))
    if any(stamp(p) != before[str(p)]['stat'] or sha256(p) != before[str(p)]['sha256'] for p in paths):
        raise ValueError('metadata changed during audit')
    result = dict(schema='rfmon.drone_ml.profile_exposure.v1', inputs=before, reports=sources,
        iq_read=False, partitions_changed=False, training_started=False, training_ready=False,
        independent_final_evaluation_eligible=False,
        limitation='Exact metadata path/member matches only; aliases, derivatives and other upstream runs may add exposure.',
        action='Keep reserved sources out of training. Obtain an unexposed final evaluation for the imported profile/hybrid system.')
    out = new_output(output, [p.parent for p in paths])
    write_json(out/'audit.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--review', type=Path, required=True)
    parser.add_argument('--reports', type=Path, nargs='+', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.review, args.reports, args.output)
    print(json.dumps([{k: r[k] for k in ('path', 'rows', 'partition_row_counts', 'locked_overlap_rows')}
                      for r in result['reports']], indent=2))


if __name__ == '__main__':
    main()
