"""Compare leave-one-dataset-out runs (`universal_lodo`): per held-out model and fold, and totals.

Per (model, held-out dataset): views typed right; model top-1 and top-3 per view (given the right
type); views named at the score limit and how many of those were right; recordings right (mean
model probability over the recording's views). Gates: never-seen models named, X310 background
typed as a drone. The app's naming rule is applied per model over all its held-out folds, both
ways: at least 20 of its views named, at least 80% of them named right, and at least 80% of every
time its name was given right (names given to other held-out drones and to never-seen models count
against it).

  python -m tools.drone_ml.universal_lodo_report RUN_DIR [RUN_DIR ...]
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path
from .universal_lodo import given_to_others

APP_NAMED, APP_TOP1 = 20, .8      # model_verdict.hpp: minimum_other_dataset_named / _top1


def pct(a, b):
    return '%3.0f%%' % (100 * a / b) if b else '  - '


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('runs', nargs='+', type=Path)
    a = p.parse_args()
    summary = []
    for run in a.runs:
        folds = [json.loads(f.read_text()) for f in sorted(run.glob('*.json'))]
        if not folds:
            continue
        print('\n== %s (%s; %d folds)' % (run.name, ', '.join(folds[0]['variants']) or 'v3 recipe', len(folds)))
        print('  %-14s %-16s %5s %6s %6s %6s %12s %9s  %s' % ('held out', 'model', 'views', 'type', 'top-1', 'top-3',
                                                              'named/right', 'rec top-1', 'mostly called'))
        tot = defaultdict(int); per_model = defaultdict(lambda: [0, 0]); given = defaultdict(int)
        for f in folds:
            for name, n in given_to_others(f).items():
                given[name] += n
            for model, s in f['held'].items():
                tr = s['type_right']
                print('  %-14s %-16s %5d %6s %6s %6s %5d/%-6s %4d/%-4d  %s' % (
                    f['fold'][:14], model[:16], s['views'], pct(tr, s['views']), pct(s['top1'], tr), pct(s['top3'], tr),
                    s['named'], pct(s['named_right'], s['named']).strip(), s['recording_top1'], s['recordings'],
                    ', '.join('%s %d' % kv for kv in list(s['predicted'].items())[:3])))
                for k in ('views', 'type_right', 'top1', 'top3', 'named', 'named_right', 'recordings', 'recording_top1'):
                    tot[k] += s[k]
                per_model[model][0] += s['named']; per_model[model][1] += s['named_right']
        g = [f['gates'] for f in folds]
        own_only = sorted(m for m, (n, r) in per_model.items() if n >= APP_NAMED and r >= APP_TOP1 * n)
        carries = sorted(m for m in own_only if per_model[m][1] >= APP_TOP1 * (per_model[m][1] + given[m]))
        line = dict(run=run.name, views=tot['views'], type=tot['type_right'] / max(1, tot['views']),
                    top1=tot['top1'] / max(1, tot['type_right']), top3=tot['top3'] / max(1, tot['type_right']),
                    named=tot['named'], named_right=tot['named_right'],
                    rec=tot['recording_top1'] / max(1, tot['recordings']), recordings=tot['recordings'],
                    open_named=sum(x['open_test']['named_at_limit'] for x in g) / max(1, sum(x['open_test']['drone_views_typed'] for x in g)),
                    bg_false=sum(x['bg_site_test']['false_rate'] for x in g) / len(g), carries=carries)
        summary.append(line)
        print('  all: top-1 %.0f%%, top-3 %.0f%% per view; named %d, right %s; recordings right %d of %d' % (
            100 * line['top1'], 100 * line['top3'], tot['named'], pct(tot['named_right'], tot['named']).strip(),
            tot['recording_top1'], tot['recordings']))
        print('  gates: never-seen models named %.1f%% of typed views; X310 background typed as drone %.2f%%' % (
            100 * line['open_named'], 100 * line['bg_false']))
        print('  per name: own views named right / named; the name given to other drones (wrong)')
        for m in sorted(per_model):
            n, r = per_model[m]
            print('    %-16s %4d / %-5d %s; given to others %4d -> right when given %s' % (
                m[:16], r, n, pct(r, n).strip(), given[m], pct(r, r + given[m]).strip()))
        print('  models passing the app rule (>= %d named, >= %.0f%% right both ways): %s%s' % (
            APP_NAMED, 100 * APP_TOP1, ', '.join(carries) or 'none',
            ('  (own views only would also pass: %s)' % ', '.join(sorted(set(own_only) - set(carries)))) if set(own_only) - set(carries) else ''))
    print('\n== Summary')
    print('  %-12s %6s %6s %6s %13s %9s %9s %8s  %s' % ('run', 'type', 'top-1', 'top-3', 'named/right', 'rec', 'open nm', 'bg', 'passes app rule'))
    for s in summary:
        print('  %-12s %5.1f%% %5.0f%% %5.0f%% %6d/%-6s %8.0f%% %8.1f%% %7.2f%%  %s' % (
            s['run'][:12], 100 * s['type'], 100 * s['top1'], 100 * s['top3'], s['named'], pct(s['named_right'], s['named']).strip(),
            100 * s['rec'], 100 * s['open_named'], 100 * s['bg_false'], ', '.join(s['carries']) or 'none'))


if __name__ == '__main__':
    main()
