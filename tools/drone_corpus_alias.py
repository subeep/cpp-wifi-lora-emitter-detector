"""Phase 4: the frame-timing alias rule (old: half a lobe excluded; new: beyond the lobe's valleys)
with real per-capture noise.

Residuals come from real Lightbridge lines: the corpus recordings' fits (drone_corpus_frames.py
--json) and, with --live LOG --from-line N, a live run's Lightbridge captures (frame fit per band
over the whole run). Each draw places 8-30 captures at the live gaps (0.7-13 s apart), a random
clock within +-8 ppm, a prior off by N(0, 1.2) ppm, and residuals drawn from the pool; the fit
is "good" as frame_timing::Fit::good(). Wrong: good but more than max(0.3, 3 sigma) ppm off.
Random: the same times with positions uniform over the symbol (no line): must not be good.

  python3 tools/drone_corpus_alias.py FRAMES.json [--live LOG --from-line N] [--draws 3000]
"""
import argparse
import json
import math

import numpy as np

from tools.drone_corpus_frames import fit, LB_P


def good(f):
    return (f and f['kept'] >= 8 and f['span'] >= 5 and f['rms_us'] <= 5 and f['kept'] >= 0.8 * f['used']
            and f['peak'] - f['alias'] >= 0.35)


def residual_pool_corpus(rows):
    pool = []
    for r in rows:
        if r['link'] != 'lightbridge':
            continue
        t = np.array(r['times']); p = np.array(r['positions_us']) * 1e-6
        f = r['fit']
        slope = -f['ppm'] * 1e-6
        res = (p - slope * t)
        res = (res - np.angle(np.exp(2j * np.pi * res / LB_P).mean()) / (2 * np.pi) * LB_P + LB_P / 2) % LB_P - LB_P / 2
        pool += [x for x in res if abs(x) <= LB_P / 8]
    return np.array(pool)


def residual_pool_live(path, from_line):
    by_band = {}
    for i, line in enumerate(open(path), 1):
        if i < from_line:
            continue
        d = json.loads(line)
        lc = d.get('link_clock') or {}
        if d.get('kind') != 'link' or lc.get('link') != 'lightbridge' or 'symbol_phase_s' not in lc or not lc.get('usable'):
            continue
        dev = d['device_ns'] * 1e-9
        by_band.setdefault((d['session'], d['center_hz'] > 3e9), []).append((dev, ((dev % LB_P) + lc['symbol_phase_s']) % LB_P, lc['ppm']))
    pool = []
    for key, s in by_band.items():
        if len(s) < 8:
            continue
        s.sort()
        t = np.array([x[0] for x in s]); p = np.array([x[1] for x in s])
        prior = float(np.median([x[2] for x in s]))
        f = fit(t, p, LB_P, prior, 1.5, 2.5e-6)
        if not f or f['rms_us'] > 5:
            continue
        tt = t - t.min()
        res = p + f['ppm'] * 1e-6 * tt
        res = (res - np.angle(np.exp(2j * np.pi * res / LB_P).mean()) / (2 * np.pi) * LB_P + LB_P / 2) % LB_P - LB_P / 2
        pool += [x for x in res if abs(x) <= LB_P / 8]
    return np.array(pool)


def trial(pool, draws, rng):
    out = {}
    for n in (8, 10, 12, 16, 30):
        c = {'old': [0, 0, 0], 'valley': [0, 0, 0]}
        for _ in range(draws):
            t = np.cumsum(rng.uniform(0.7, 13, n))
            true = rng.uniform(-8, 8); prior = true + rng.normal(0, 1.2)
            pos = (rng.uniform(0, LB_P) - true * 1e-6 * t + rng.choice(pool, n)) % LB_P
            rnd = rng.uniform(0, LB_P, n)
            for rule in c:
                f = fit(t, pos, LB_P, prior, 1.5, 2.5e-6, 'valley' if rule == 'valley' else 'half')
                if good(f):
                    c[rule][0] += 1
                    if abs(f['ppm'] - true) > max(0.3, 3 * f['sigma']):
                        c[rule][1] += 1
                c[rule][2] += good(fit(t, rnd, LB_P, prior, 1.5, 2.5e-6, 'valley' if rule == 'valley' else 'half'))
        out[n] = c
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames')
    ap.add_argument('--live')
    ap.add_argument('--from-line', type=int, default=1)
    ap.add_argument('--draws', type=int, default=3000)
    args = ap.parse_args()
    rows = json.load(open(args.frames))
    pools = {'corpus Lightbridge (0.1 s captures)': residual_pool_corpus(rows)}
    if args.live:
        pools['live X310 Lightbridge (%s from line %d)' % (args.live.split('/')[-1], args.from_line)] = \
            residual_pool_live(args.live, args.from_line)
    rng = np.random.default_rng(23)
    for name, pool in pools.items():
        if len(pool) < 20:
            print(name, ': too few residuals', len(pool)); continue
        print('%s: %d residuals, rms %.2f us' % (name, len(pool), math.sqrt(np.mean(pool ** 2)) * 1e6))
        res = trial(pool, args.draws, rng)
        print('  %-4s  %-28s  %-28s' % ('n', 'old rule good/wrong/random', 'valley rule good/wrong/random'))
        for n, c in res.items():
            print('  %-4d  %-28s  %-28s' % (n, '%d / %d / %d' % tuple(c['old']), '%d / %d / %d' % tuple(c['valley'])))
        print('  (of %d draws each)' % args.draws)


if __name__ == '__main__':
    main()
