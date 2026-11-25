"""Phase 1 summary of drone_corpus_feed output: per model, the app's timing group, link clock and
DroneID crystal offset.

Per recording: the timing group each view capture got; the link clock per capture time from the
view with the smallest sigma among the usable ones of the dominant link (so one transmitter seen
by several overlapping views counts once); its scatter between capture times against the sigma
the app reports; and the DroneID packets' ppm against the link clock (both measured against the
same receiver clock, so their difference is the check).

  python3 tools/drone_corpus_summary.py DIR [--json OUT]
"""
import argparse
import collections
import json
import math
from pathlib import Path
import statistics as stt


def median(v):
    return stt.median(v) if v else float('nan')


def load(d):
    manifest = {}
    for line in open(Path(d) / 'manifest.jsonl'):
        m = json.loads(line)
        manifest[m['run']] = m
    out = []
    for run, m in manifest.items():
        f = Path(d) / m['dataset'] / (run + '.jsonl')
        if not f.exists():
            continue
        lines = [json.loads(l) for l in open(f) if l.strip()]
        out.append((m, lines))
    return out


def summarise(m, lines):
    links = [l for l in lines if l['kind'] == 'link']
    packets = [l for l in lines if l['kind'] == 'droneid']
    groups = collections.Counter(l['group'] for l in links if l['group'])
    flagged = sum(groups.values())
    # Dominant link clock kind among usable readings.
    usable = [l for l in links if (l.get('link_clock') or {}).get('usable')]
    kinds = collections.Counter(l['link_clock']['link'] for l in usable)
    kind = kinds.most_common(1)[0][0] if kinds else None
    per_time = {}
    for l in usable:
        if l['link_clock']['link'] != kind:
            continue
        t = round(l['t_s'], 3)
        if t not in per_time or l['link_clock']['sigma_ppm'] < per_time[t]['link_clock']['sigma_ppm']:
            per_time[t] = l
    clocks = [per_time[t]['link_clock']['ppm'] for t in sorted(per_time)]
    sigmas = [per_time[t]['link_clock']['sigma_ppm'] for t in sorted(per_time)]
    all_clocks = [l['link_clock']['ppm'] for l in usable if l['link_clock']['link'] == kind]
    did = [p['offset_ppm'] for p in packets if p.get('offset_ppm') is not None]
    serials = sorted({p['serial'] for p in packets if p.get('serial')})
    return dict(run=m['run'], dataset=m['dataset'], model=m['model'], family=m['family'], role=m.get('role', ''),
                kind=m.get('kind', ''), truth=m.get('truth_units', []), views=len(links), flagged=flagged,
                groups=dict(groups), clock_kind=kind, clock_captures=len(clocks), clock_median=median(all_clocks),
                clock_values=clocks, clock_sigmas=sigmas, all_clock_count=len(all_clocks),
                droneid=len(did), droneid_median=median(did), droneid_values=did, serials=serials)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--json')
    args = ap.parse_args()
    recs = [summarise(m, l) for m, l in load(args.dir)]
    by_model = collections.defaultdict(list)
    for r in recs:
        key = (r['family'], r['model'] + (' (controller)' if r['role'] == 'controller' else ''))
        if r['kind'] == 'mixed':
            key = ('mixed', 'DRFF mixed (2-3 drones)')
        by_model[key].append(r)
    print('A/B. Timing group and link clock per model (views: 20 MSps view captures; clock: usable readings)')
    print('%-12s %-34s %4s %6s %-38s %-12s %6s %8s %7s %7s' % (
        'family', 'model', 'recs', 'views', 'timing groups (share of view captures)', 'clock link', 'clocks',
        'median', 'scatter', 'sigma'))
    for (family, model), rs in sorted(by_model.items()):
        views = sum(r['views'] for r in rs)
        groups = collections.Counter()
        for r in rs:
            groups.update(r['groups'])
        gtext = ', '.join('%s %.0f%%' % (g, 100 * n / views) for g, n in groups.most_common(3)) or '-'
        kinds = collections.Counter(r['clock_kind'] for r in rs if r['clock_kind'])
        # Scatter between capture times within one recording, pooled: residuals from each recording's mean.
        res, sig = [], []
        for r in rs:
            v = r['clock_values']
            if len(v) >= 2:
                mu = sum(v) / len(v)
                res += [x - mu for x in v]
                sig += r['clock_sigmas']
        dof = sum(max(len(r['clock_values']) - 1, 0) for r in rs if len(r['clock_values']) >= 2)
        scatter = math.sqrt(sum(x * x for x in res) / dof) if dof else float('nan')
        clocks = sum(r['all_clock_count'] for r in rs)
        med = median([r['clock_median'] for r in rs if not math.isnan(r['clock_median'])])
        print('%-12s %-34s %4d %6d %-38s %-12s %6d %+8.2f %7.2f %7.2f' % (
            family, model[:34], len(rs), views, gtext[:38], ','.join(kinds) or '-', clocks, med, scatter,
            median(sig)))
    print('\nC. Link clock against DroneID (same recording, same receiver clock)')
    print('%-34s %-44s %8s %9s %9s %8s' % ('model', 'recording', 'packets', 'DroneID', 'link', 'diff'))
    diffs = collections.defaultdict(list)
    for r in sorted(recs, key=lambda r: (r['model'], r['run'])):
        if r['droneid'] and r['clock_captures'] and r['kind'] != 'mixed':
            d = r['clock_median'] - r['droneid_median']
            diffs[r['model']].append(d)
            print('%-34s %-44s %8d %+9.3f %+9.3f %+8.3f' % (r['model'][:34], r['run'][-44:], r['droneid'], r['droneid_median'],
                                                         r['clock_median'], d))
    print('\nPer model: median |link - DroneID|')
    for model, d in sorted(diffs.items()):
        print('  %-34s %d recordings, median diff %+.3f, max |diff| %.3f' % (model, len(d), median(d), max(abs(x) for x in d)))
    print('\nDroneID packets without a link clock in the same recording:')
    for r in recs:
        if r['droneid'] and not r['clock_captures']:
            print('  %-34s %-50s %d packets, %+.3f ppm, groups %s' % (r['model'], r['run'][-50:], r['droneid'],
                                                                     r['droneid_median'], r['groups']))
    if args.json:
        Path(args.json).write_text(json.dumps(recs, indent=1))


if __name__ == '__main__':
    main()
