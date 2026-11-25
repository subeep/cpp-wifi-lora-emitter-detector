"""Phase 3 extension: can OcuSync frame timing tell simultaneous drones apart?

In one recording (one receiver clock), every capture's symbol-grid position (drone_corpus_check
--symbol-grid, dense captures, every view with a 15 or 30 kHz group) is placed on the device
clock modulo the symbol period. One transmitter's positions lie on one line (slope = its clock,
within ~0.1-0.3 us); two transmitters started their frames independently, so they sit on
different lines even at the same clock. Positions are corrected by a common slope (the median
clock of the recording) and clustered on the circle (gap > 3 us, cluster >= 3 captures); the
number of clusters per spacing is compared with the number of drones.

  python3 tools/drone_corpus_separation.py DIR [--min-coherence 0.1] [--gap-us 3]
"""
import argparse
import collections
import json
import math
from pathlib import Path
import statistics as stt


def clusters(values, period, gap, min_size):
    """Clusters of points on a circle of `period`: split at gaps larger than `gap`."""
    if not values:
        return []
    v = sorted(x % period for x in values)
    gaps = [(v[(i + 1) % len(v)] - v[i]) % period for i in range(len(v))]
    if len(v) == 1:
        return [v]
    start = max(range(len(v)), key=lambda i: gaps[i])  # biggest gap: cut there
    order = [v[(start + 1 + k) % len(v)] for k in range(len(v))]
    out, cur = [], [order[0]]
    for a, b in zip(order, order[1:]):
        if (b - a) % period > gap:
            out.append(cur); cur = [b]
        else:
            cur.append(b)
    out.append(cur)
    return [c for c in out if len(c) >= min_size]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--min-coherence', type=float, default=0.1)
    ap.add_argument('--gap-us', type=float, default=3.0)
    ap.add_argument('--min-size', type=int, default=3)
    args = ap.parse_args()
    d = Path(args.dir)
    rows = []
    for line in open(d / 'manifest.jsonl'):
        m = json.loads(line)
        if m['dataset'] != 'drff' or m.get('kind') not in ('mixed', 'hover'):
            continue
        f = d / 'drff' / (m['run'] + '.jsonl')
        if not f.exists():
            continue
        L = [json.loads(l) for l in open(f)]
        clk = [x['link_clock']['ppm'] for x in L if x['kind'] == 'link' and (x.get('link_clock') or {}).get('usable')]
        did = [x['offset_ppm'] for x in L if x['kind'] == 'droneid' and x.get('offset_ppm') is not None]
        ppm = stt.median(clk + did) if clk + did else 1.0
        per = collections.defaultdict(list)
        for x in L:
            if x['kind'] != 'dense':
                continue
            for g in x.get('symbol_grid', []):
                if g['coherence'] < args.min_coherence:
                    continue
                P = 1 / g['nominal_hz']
                t = x['device_ns'] * 1e-9
                pos = ((t % P) + g['phase_at_nominal_s']) % P
                pos = (pos + ppm * 1e-6 * (t - 1.0)) % P  # take out the common clock
                per[g['spacing_hz']].append(pos * 1e6)
        found = {int(s / 1000): len(clusters(v, (1e6 / (14000 if s == 15000 else 28000)), args.gap_us, args.min_size))
                 for s, v in per.items()}
        truth = len(m['truth_units']) if m.get('kind') == 'mixed' else 1
        rows.append((m['kind'], truth, m['run'], found, {int(s / 1000): len(v) for s, v in per.items()}))
    agg = collections.defaultdict(collections.Counter)
    for kind, truth, run, found, n in rows:
        best = max(found.values(), default=0)
        agg[(kind, truth)][best] += 1
        print('%-6s truth %d  clusters %s  (positions %s)  %s' % (kind, truth, found, n, run[-60:]))
    print('\nLargest cluster count per recording against the number of drones:')
    for (kind, truth), c in sorted(agg.items()):
        n = sum(c.values())
        print('  %-6s %d drone(s): %3d recordings, clusters %s, equal %d%%' % (kind, truth, n, dict(sorted(c.items())),
                                                                           round(100 * c[truth] / n)))


if __name__ == '__main__':
    main()
