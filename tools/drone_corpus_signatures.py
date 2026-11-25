"""Step 1 of the classifier work: per-model link signatures from the corpus, and how far they
carry across recordings, aircraft units and datasets (receivers).

Input: drone_corpus_feed output made with `--extra "--traits-s 0.1"` ("traits" lines: periodicity
spectra of the link's cyclic-prefix metric and power envelope over 100 ms, 50 Hz bands from
50 Hz to 30 kHz, median band removed). A recording's signature is the median over its trait lines
of its dominant timing group, of the chosen band range. Matching: nearest model centroid by
correlation distance on features standardised over the training recordings.

Evaluations (a model is only scored where it has training data left):
  recording: leave one recording out;
  unit:      leave one aircraft unit out (DRFF-R2 units, DroneRFb aircraft numbers, RFUAV packs);
  dataset:   leave one dataset out (models recorded by two or more datasets).

  python3 tools/drone_corpus_signatures.py DIR [--bands cp:0-60,env:0-60] [--json OUT]
"""
import argparse
import collections
import json
from pathlib import Path
import re

import numpy as np

REPO = Path(__file__).resolve().parents[1]
# Corpus model names that the app's model table (evidence_claims.hpp) spells differently.
TABLE_NAME = {'Air 2': 'Mavic Air 2', 'Matrice 300': 'M300 RTK', 'Phantom 4 Pro+': 'Phantom 4 Pro', 'Matrice 210': 'M210RTK'}


def generation_table():
    """Model -> link generation from the app's own model table (advertised link names)."""
    text = (REPO / 'src/drones/evidence_claims.hpp').read_text()
    out = {}
    for model, link in re.findall(r'\{"([^"]+)", "([^"]+)", (?:"[^"]*"|nullptr), (?:true|false)', text):
        g = ('Lightbridge' if link.startswith('Lightbridge') else 'OcuSync 1' if link == 'OcuSync 1'
             else 'OcuSync 2' if link.startswith('OcuSync 2') else 'O3' if link.startswith('O3')
             else 'O4' if link.startswith('O4') else None)
        if g:
            out[model] = g
    return out


FAMILY_OF_GROUP = {'15kHz': 'ocusync', '15kHz+30kHz': 'ocusync', '30kHz': 'ocusync', '32.8kHz': 'lightbridge'}


def unit_of(m):
    run = m['run']
    if m['dataset'] == 'drff':
        u = re.search(r'(mavicAir2s|mavicAir2|mini4PRO|mini5PRO|mavic3|mini3pro)_(\d)_hover', run)
        return 'drff:%s_%s' % (u[1], u[2]) if u else run
    if m['dataset'] == 'dronerfb':
        u = re.search(r'dronerfb_([A-Z]\d)_', run)
        return 'dronerfb:' + (u[1] if u else run)
    if m['dataset'] == 'rfuav':
        return 'rfuav:' + run          # one pack = one session; units unknown
    return m['dataset'] + ':' + m['model']


def parse_bands(spec):
    out = []
    for part in spec.split(','):
        name, rng = part.split(':')
        lo, hi = (int(x) for x in rng.split('-'))
        out.append((name, lo, hi))
    return out


def load(d, bands):
    recs = []
    for line in open(Path(d) / 'manifest.jsonl'):
        m = json.loads(line)
        if m['family'] not in ('ocusync', 'lightbridge'):
            continue
        f = Path(d) / m['dataset'] / (m['run'] + '.jsonl')
        if not f.exists():
            continue
        T = [json.loads(l) for l in open(f) if '"traits"' in l]
        T = [t for t in T if t['group'] in FAMILY_OF_GROUP]
        if not T:
            continue
        group = collections.Counter(t['group'] for t in T).most_common(1)[0][0]
        T = [t for t in T if t['group'] == group]
        vec = []
        for name, lo, hi in bands:
            arr = np.array([t[name + '_bands'][lo:hi] for t in T], float)
            vec.append(np.median(arr, axis=0))
        peaks = collections.Counter()
        for t in T:
            for f_hz, db in t['cp_peaks'][:8]:
                peaks[round(f_hz / 50) * 50] += 1
        recs.append(dict(run=m['run'], dataset=m['dataset'], model=m['model'], family=FAMILY_OF_GROUP[group], group=group,
                         unit=unit_of(m), lines=len(T), x=np.concatenate(vec), peaks=peaks))
    return recs


def classify(train, test):
    """Nearest centroid, correlation distance on standardised features. Returns (pred, ranked)."""
    X = np.array([r['x'] for r in train])
    mu, sd = X.mean(0), X.std(0) + 1e-6
    cents = {}
    for model in sorted({r['model'] for r in train}):
        Z = (np.array([r['x'] for r in train if r['model'] == model]) - mu) / sd
        cents[model] = Z.mean(0)
    z = (test['x'] - mu) / sd
    def corr(a, b):
        a = a - a.mean(); b = b - b.mean()
        return float(a @ b / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-12))
    ranked = sorted(cents, key=lambda m: -corr(z, cents[m]))
    return ranked[0], ranked


def evaluate(recs, key, eligible):
    """Leave out every value of key(r) in turn; score test recordings whose model stays in training."""
    res = collections.defaultdict(lambda: [0, 0, 0, collections.Counter()])  # model -> n, top1, top3, confusions
    for held in sorted({key(r) for r in recs}):
        test = [r for r in recs if key(r) == held]
        train = [r for r in recs if key(r) != held and r['family'] == test[0]['family']]
        for t in test:
            if not eligible(t, train):
                continue
            pool = [r for r in train if r['family'] == t['family']]
            pred, ranked = classify(pool, t)
            s = res[t['model']]
            s[0] += 1; s[1] += pred == t['model']; s[2] += t['model'] in ranked[:3]
            s[3][pred] += 1
    return res


def show(title, res):
    print('\n' + title)
    tot = [0, 0, 0]
    for model, (n, a1, a3, conf) in sorted(res.items()):
        tot[0] += n; tot[1] += a1; tot[2] += a3
        wrong = [(m, c) for m, c in conf.most_common() if m != model][:3]
        print('  %-28s %3d recs  top-1 %3.0f%%  top-3 %3.0f%%  %s' % (
            model[:28], n, 100 * a1 / n, 100 * a3 / n, ('confused with ' + ', '.join('%s %d' % w for w in wrong)) if wrong else ''))
    if tot[0]:
        print('  %-28s %3d recs  top-1 %3.0f%%  top-3 %3.0f%%' % ('all', tot[0], 100 * tot[1] / tot[0], 100 * tot[2] / tot[0]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--bands', default='cp:0-60,env:0-60')
    ap.add_argument('--json')
    args = ap.parse_args()
    recs = load(args.dir, parse_bands(args.bands))
    print('%d recordings with link traits; models: %s' % (len(recs), dict(collections.Counter(r['model'] for r in recs))))
    # Signature table: the most common strong lines per model (cyclic-prefix metric).
    print('\nPer-model rhythm lines (Hz, share of trait lines with that line among the 8 strongest):')
    by_model = collections.defaultdict(list)
    for r in recs:
        by_model[(r['family'], r['model'])].append(r)
    for (fam, model), rs in sorted(by_model.items()):
        peaks = collections.Counter(); lines = sum(r['lines'] for r in rs)
        for r in rs:
            peaks.update(r['peaks'])
        top = ', '.join('%d (%.0f%%)' % (f, 100 * c / lines) for f, c in peaks.most_common(7))
        print('  %-11s %-28s %2d recs %-11s %s' % (fam, model[:28], len(rs), '/'.join(sorted({r['dataset'] for r in rs})), top))
    has_model = lambda t, train: any(r['model'] == t['model'] for r in train)
    show('Leave one recording out (same receiver and unit may stay in training):',
         evaluate(recs, lambda r: r['run'], has_model))
    show('Leave one aircraft unit out (models with more than one unit):',
         evaluate(recs, lambda r: r['unit'], has_model))
    show('Leave one dataset out (models recorded by two or more datasets: the receiver changes):',
         evaluate(recs, lambda r: r['dataset'], has_model))

    # Generation: the nearest model's generation (models with a known generation only).
    gens = generation_table()
    gen = lambda model: gens.get(TABLE_NAME.get(model, model)) or ('Lightbridge' if model == 'Inspire 2' else None)
    known = [r for r in recs if gen(r['model'])]
    print('\nGeneration labels (app model table):', dict(collections.Counter((r['model'], gen(r['model'])) for r in known)))
    def gen_eval(title, key):
        res = collections.defaultdict(lambda: [0, 0, collections.Counter()])
        for held in sorted({key(r) for r in known}):
            test = [r for r in known if key(r) == held]
            train = [r for r in known if key(r) != held]
            for t in test:
                pool = [r for r in train if r['family'] == t['family']]
                if not any(gen(r['model']) == gen(t['model']) for r in pool):
                    continue  # its generation has no other model or dataset left to learn from
                pred, _ = classify(pool, t)
                s_ = res[(gen(t['model']), t['model'])]
                s_[0] += 1; s_[1] += gen(pred) == gen(t['model']); s_[2][gen(pred)] += 1
        print('\n' + title)
        n = ok = 0
        for (g, model), (a, b, conf) in sorted(res.items()):
            n += a; ok += b
            print('  %-11s %-28s %3d recs  generation right %3.0f%%  %s' % (g, model[:28], a, 100 * b / a, dict(conf)))
        if n:
            print('  all: %d recs, generation right %.0f%%' % (n, 100 * ok / n))
    gen_eval('Generation, leave one dataset out:', lambda r: r['dataset'])
    gen_eval('Generation, leave one model out (a model never seen in training):', lambda r: r['model'])
    if args.json:
        Path(args.json).write_text(json.dumps([{k: (v.tolist() if isinstance(v, np.ndarray) else v)
                                                 for k, v in r.items() if k != 'peaks'} for r in recs]))


if __name__ == '__main__':
    main()
