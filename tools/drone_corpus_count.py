"""Phase 3: drone count on drone_corpus_feed output, through the app's rows and the Drones page.

Each recording's offset-log lines are replayed by build/drone_log_replay (drone_claims::Store,
detections, filter_rows, group_rows: what the page would show) and the drone count at the end of
the recording is compared with the truth: the number of drones in a DRFF-R2 mixed recording, 1 for
a single drone recording, 0 for a recording with no drone. The dense lines are left out (they are
not in the live log).

--pairs also overlays two single-drone DRFF-R2 hover recordings on one time line (as if heard
together): two different units of one model (truth 2) and one unit in two recordings (truth 1).
Frame timing cannot join recordings from different times, so symbol phases are dropped there.

  python3 tools/drone_corpus_count.py DIR [--pairs] [--json OUT]
"""
import argparse
import collections
import itertools
import json
from pathlib import Path
import re
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]
REPLAY = REPO / 'build/drone_log_replay'
STATE = re.compile(r'^(\d\d:\d\d:\d\d)\s+(\d+) rows, likely (\d+) drones? \(perhaps (\d+)\):(.*)$')


def replay(lines):
    with tempfile.NamedTemporaryFile('w', suffix='.jsonl', delete=False) as f:
        for l in lines:
            f.write(json.dumps(l) + '\n')
        name = f.name
    try:
        out = subprocess.run([str(REPLAY), name, '--step', '1', '--verbose'], capture_output=True, text=True, check=True).stdout
    finally:
        Path(name).unlink()
    states = []
    for line in out.splitlines():
        m = STATE.match(line)
        if m:
            states.append(dict(rows=int(m[2]), drones=int(m[3]), fewest=int(m[4]), detail=m[5].strip()))
    return states


def live_lines(path):
    return [json.loads(l) for l in open(path) if l.strip() and json.loads(l)['kind'] in ('link', 'droneid')]


def truth_of(m):
    if m.get('kind') == 'mixed':
        return len(m['truth_units'])
    if m['family'] in ('not_a_drone', 'hobby_link'):
        return 0
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--pairs', action='store_true')
    ap.add_argument('--json')
    args = ap.parse_args()
    d = Path(args.dir)
    manifest = [json.loads(l) for l in open(d / 'manifest.jsonl')]
    results = []
    for m in manifest:
        f = d / m['dataset'] / (m['run'] + '.jsonl')
        if not f.exists():
            continue
        states = replay(live_lines(f))
        last = states[-1] if states else dict(rows=0, drones=0, fewest=0, detail='')
        results.append(dict(run=m['run'], dataset=m['dataset'], model=m['model'], family=m['family'], kind=m.get('kind', ''),
                            truth=truth_of(m), shown=last['drones'], fewest=last['fewest'], rows=last['rows'],
                            detail=last['detail'], most=max((s['drones'] for s in states), default=0)))
    groups = collections.defaultdict(list)
    for r in results:
        key = 'DRFF mixed, %d drones' % r['truth'] if r['kind'] == 'mixed' else '%s / %s' % (r['family'], r['model'])
        groups[key].append(r)
    print('Drone count at the end of each recording (app rows -> Drones page), against the truth')
    print('%-48s %5s %6s %8s %8s %8s  %s' % ('group', 'recs', 'truth', 'right', 'too many', 'too few', 'shown counts'))
    for key, rs in sorted(groups.items()):
        right = sum(r['shown'] == r['truth'] for r in rs)
        many = sum(r['shown'] > r['truth'] for r in rs)
        few = sum(r['shown'] < r['truth'] for r in rs)
        counts = collections.Counter(r['shown'] for r in rs)
        print('%-48s %5d %6s %8d %8d %8d  %s' % (key[:48], len(rs), sorted({r['truth'] for r in rs}), right, many, few,
                                              dict(sorted(counts.items()))))
    wrong = [r for r in results if r['shown'] != r['truth']]
    if wrong:
        print('\nWrong counts (rows as shown):')
        for r in wrong[:60]:
            print('  %-70s truth %d shown %d (perhaps %d): %s' % (r['run'][-70:], r['truth'], r['shown'], r['fewest'], r['detail'][:150]))
    pair_results = []
    if args.pairs:
        hover = collections.defaultdict(list)
        for m in manifest:
            if m['dataset'] == 'drff' and m.get('kind') == 'hover':
                mm = re.search(r'(mavicAir2s|mavicAir2|mini4PRO|mini5PRO|mavic3|mini3pro)_(\d)_hover_(c\d)_(u\d)_(d\d)', m['run'])
                if mm:
                    hover[mm[1]].append((mm[2], mm[3] + mm[4] + mm[5], m))
        for model, items in sorted(hover.items()):
            for (u1, c1, m1), (u2, c2, m2) in itertools.combinations(items, 2):
                truth = 1 if u1 == u2 else 2
                lines = []
                for k, m in enumerate((m1, m2)):
                    for l in live_lines(d / m['dataset'] / (m['run'] + '.jsonl')):
                        l = dict(l)
                        l['run'] = 'pair%d' % k
                        if 'link_clock' in l:
                            l['link_clock'] = {k2: v for k2, v in l['link_clock'].items() if k2 != 'symbol_phase_s'}
                        lines.append(l)
                lines.sort(key=lambda l: l['unix_ms'])
                states = replay(lines)
                last = states[-1] if states else dict(drones=0, fewest=0, detail='')
                pair_results.append(dict(model=model, a=m1['run'], b=m2['run'], truth=truth, shown=last['drones'],
                                         fewest=last['fewest'], detail=last['detail']))
        print('\nOverlaid DRFF hover pairs (same model): truth 2 = two units, 1 = one unit in two recordings')
        agg = collections.defaultdict(collections.Counter)
        for p in pair_results:
            agg[(p['model'], p['truth'])][p['shown']] += 1
        for (model, truth), c in sorted(agg.items()):
            n = sum(c.values())
            print('  %-12s truth %d: %3d pairs, shown %s, right %d%%' % (model, truth, n, dict(sorted(c.items())),
                                                                       round(100 * c[truth] / n)))
    if args.json:
        Path(args.json).write_text(json.dumps(dict(recordings=results, pairs=pair_results), indent=1))


if __name__ == '__main__':
    main()
