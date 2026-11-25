"""Phase 2: frame timing per link type on drone_corpus_feed output ("dense" lines).

For every recording and link it takes one symbol-grid position per capture (the best view at
that time) and fits them as the app's frame_timing.hpp does (slope search around a prior, unwrap
at the best slope, least squares on captures within P/8, twice), with the symbol period P as a
parameter:
  Lightbridge: P = 7 ms / 68 and the app's own link_clock symbol phase;
  OcuSync 15 / 30 kHz: P = 1 / nominal symbol rate and the prototype's phase at that rate
  (drone_corpus_check --symbol-grid); the symbol rate each capture measured is reported too.
A link whose captures lie on one line (small rms, most captures kept, clear peak) keeps its
frame timing across captures; the line's slope is its clock, checked against the link clock and
DroneID of the same recording.

  python3 tools/drone_corpus_frames.py DIR [--json OUT] [--min-captures 4]
"""
import argparse
import collections
import json
import math
from pathlib import Path
import statistics as stt

import numpy as np

LB_P = 7e-3 / 68


def fit(t, pos, P, prior, prior_sigma, noise, rule='valley', half_width=5.0):
    """frame_timing::fit in numpy with period P (s). t, pos in s. Returns dict or None."""
    t = np.asarray(t, float); pos = np.asarray(pos, float)
    if len(t) < 2:
        return None
    t0 = t.min(); t = t - t0
    span = t.max()
    if span <= 0:
        return None
    w = max(half_width, 3 * prior_sigma) * 1e-6
    step = min(0.05e-6, 0.1 * noise / span)
    slopes = np.arange(-prior * 1e-6 - w, -prior * 1e-6 + w + step, step)
    R = np.empty(len(slopes))
    for i in range(0, len(slopes), 2000):
        s = slopes[i:i + 2000]
        R[i:i + 2000] = np.abs(np.exp(2j * np.pi * (pos[None] - s[:, None] * t[None]) / P).mean(1))
    b = int(R.argmax())
    if rule == 'valley':
        lo = b
        while lo > 0 and R[lo - 1] <= R[lo]:
            lo -= 1
        hi = b
        while hi + 1 < len(R) and R[hi + 1] <= R[hi]:
            hi += 1
        mask = np.ones(len(R), bool); mask[lo:hi + 1] = False
    else:
        mask = np.abs(slopes - slopes[b]) > P / (2 * span)
    alias = R[mask].max() if mask.any() else 0.0
    slope = slopes[b]
    icpt = np.angle(np.exp(2j * np.pi * (pos - slope * t) / P).mean()) / (2 * np.pi) * P
    u = pos + np.round((icpt + slope * t - pos) / P) * P
    keep = np.ones(len(t), bool)
    for _ in range(2):
        keep = np.abs(u - icpt - slope * t) <= P / 8
        if keep.sum() < 2:
            break
        A = np.vstack([t[keep], np.ones(keep.sum())]).T
        (slope, icpt), *_ = np.linalg.lstsq(A, u[keep], rcond=None)
    res = (u - icpt - slope * t)[keep]
    tk = t[keep]
    rms = float(np.sqrt(np.mean(res ** 2))) if keep.any() else math.inf
    stt_ = float(((tk - tk.mean()) ** 2).sum()) if keep.any() else 0
    sigma = max(rms, noise) / math.sqrt(max(stt_, 1e-12)) * 1e6
    return dict(ppm=-slope * 1e6, sigma=sigma, rms_us=rms * 1e6, used=int(len(t)), kept=int(keep.sum()),
                span=float(tk.max() - tk.min()) if keep.any() else 0.0, peak=float(R[b]), alias=float(alias))


def median(v):
    return stt.median(v) if v else float('nan')


def series(lines, which):
    """Best view per capture time: list of (device_s, phase_s, extra)."""
    best = {}
    for d in lines:
        if d.get('kind') != 'dense':
            continue
        t = round(d['t_s'], 4)
        dev = d['device_ns'] * 1e-9
        if which == 'lightbridge':
            lc = d.get('link_clock') or {}
            if lc.get('link') != 'lightbridge' or lc.get('symbol_phase_s') is None:
                continue
            score = -lc['sigma_ppm']
            item = (dev, lc['symbol_phase_s'], dict(ppm=lc['ppm'], sigma=lc['sigma_ppm'], view=d['center_hz']))
        else:
            spacing = 15000.0 if which == 'ocusync_15k' else 30000.0
            g = [g for g in d.get('symbol_grid', []) if g['spacing_hz'] == spacing]
            if not g:
                continue
            g = g[0]
            score = g['coherence']
            item = (dev, g['phase_at_nominal_s'], dict(rate=g['rate_hz'], coherence=g['coherence'], view=d['center_hz'],
                                                       nominal=g['nominal_hz']))
        if t not in best or score > best[t][0]:
            best[t] = (score, item)
    return [best[t][1] for t in sorted(best)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--json')
    ap.add_argument('--min-captures', type=int, default=4)
    ap.add_argument('--min-coherence', type=float, default=0.05)
    args = ap.parse_args()
    manifest = [json.loads(l) for l in open(Path(args.dir) / 'manifest.jsonl')]
    rows = []
    for m in manifest:
        f = Path(args.dir) / m['dataset'] / (m['run'] + '.jsonl')
        if not f.exists():
            continue
        lines = [json.loads(l) for l in open(f) if l.strip()]
        link_clocks = [d['link_clock']['ppm'] for d in lines if d['kind'] == 'link' and (d.get('link_clock') or {}).get('usable')]
        did = [d['offset_ppm'] for d in lines if d['kind'] == 'droneid' and d.get('offset_ppm') is not None]
        ref = median(did) if did else median(link_clocks)
        for which in ('lightbridge', 'ocusync_15k', 'ocusync_30k'):
            s = series(lines, which)
            if which != 'lightbridge':
                s = [x for x in s if x[2]['coherence'] >= args.min_coherence]
            if len(s) < args.min_captures:
                continue
            t = [x[0] for x in s]
            P = LB_P if which == 'lightbridge' else 1 / s[0][2]['nominal']
            pos = [((x[0] % P) + x[1]) % P for x in s]
            noise = 2.5e-6 if which == 'lightbridge' else 0.25e-6
            prior = ref if not math.isnan(ref) else 0.0
            prior_sigma = 1.5 if not math.isnan(ref) else 7.0
            f_new = fit(t, pos, P, prior, prior_sigma, noise, 'valley')
            f_old = fit(t, pos, P, prior, prior_sigma, noise, 'half')
            rates = [x[2]['rate'] for x in s] if which != 'lightbridge' else []
            rows.append(dict(run=m['run'], dataset=m['dataset'], model=m['model'], family=m['family'], link=which,
                             captures=len(s), span=t[-1] - t[0], P_us=P * 1e6, fit=f_new, fit_old_rule=f_old,
                             link_clock=median(link_clocks), droneid=median(did), droneid_packets=len(did),
                             rate_median=median(rates), rate_spread=(stt.pstdev(rates) if len(rates) > 1 else float('nan')),
                             positions_us=[p * 1e6 for p in pos], times=[x - t[0] for x in t]))
    print('%-28s %-11s %-46s %4s %5s %8s %6s %6s %6s %7s %8s %8s %10s' % (
        'model', 'link', 'recording', 'caps', 'span', 'fit ppm', 'rms us', 'kept', 'margin', 'oldmarg', 'link clk',
        'DroneID', 'rate Hz'))
    for r in sorted(rows, key=lambda r: (r['link'], r['model'], r['run'])):
        f, o = r['fit'], r['fit_old_rule']
        print('%-28s %-11s %-46s %4d %5.1f %+8.2f %6.2f %3d/%-3d %6.2f %7.2f %+8.2f %+8.2f %10.2f' % (
            r['model'][:28], r['link'], r['run'][-46:], r['captures'], r['span'], f['ppm'], f['rms_us'], f['kept'], f['used'],
            f['peak'] - f['alias'], o['peak'] - o['alias'], r['link_clock'], r['droneid'], r['rate_median']))
    print('\nPer model and link: recordings whose captures lie on one line (rms <= 5 us LB / 1 us OcuSync, >= 80% kept)')
    agg = collections.defaultdict(list)
    for r in rows:
        agg[(r['link'], r['model'])].append(r)
    for (link, model), rs in sorted(agg.items()):
        lim = 5.0 if link == 'lightbridge' else 1.0
        ok = [r for r in rs if r['fit']['rms_us'] <= lim and r['fit']['kept'] >= 0.8 * r['fit']['used']]
        d = [r['fit']['ppm'] - (r['droneid'] if not math.isnan(r['droneid']) else r['link_clock']) for r in ok
             if not (math.isnan(r['droneid']) and math.isnan(r['link_clock']))]
        print('  %-11s %-28s %2d/%-2d on a line; median rms %.2f us; fit - reference ppm: median %+.2f, max |%.2f|; rate %.2f Hz' % (
            link, model[:28], len(ok), len(rs), median([r['fit']['rms_us'] for r in rs]), median(d) if d else float('nan'),
            max((abs(x) for x in d), default=float('nan')), median([r['rate_median'] for r in rs])))
    if args.json:
        Path(args.json).write_text(json.dumps(rows))


if __name__ == '__main__':
    main()
