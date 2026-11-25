"""Classifier step 2: leave one dataset (receiver) out, and receiver-invariance variants.

Question: does a model name the classifier gives hold on a receiver it never trained on? Each
fold removes one dataset completely (every model, every partition, and its views mixed onto the
X310 background) and trains the v3 recipe (`universal_c2_train`: two-level network, open-set
rotation, site background, mixed views, same augmentation) on everything else. The fold then
scores the removed dataset's recordings of models that are still trained from another dataset.

Pool: drone windows of cache partitions train, val, test, unit_test and cross_test (the old
unseen-aircraft and unit claims are not made here, so their windows train); open_test models stay
out (never-seen-model check). Validation for epoch choice: the val partition and mix_val of the
remaining datasets, and bg_site_val. Also scored, to watch the gates: open_test (never-seen
models; named share at the score limit) and bg_site_test (X310 background typed as a drone).

Variants (phase 1):
  balance     a model's weight is shared equally by its datasets (default: by window count);
  response    receiver-response augmentation: a random smooth gain curve over frequency (tilt,
              ripple) on each training view, on top of the v3 augmentation;
  adversary   a dataset head behind gradient reversal on the embedding of drone views, so the
              features learn little that tells the datasets apart;
  cond_adversary  the same, but the head is told the model and trains only on models recorded by
              two or more datasets (removes the receiver, not the model).
The training loop is `universal_c2_train.fit` (one loop for both).

  python -m tools.drone_ml.universal_lodo --output DIR --fold DroneRFa [--variant balance ...] [--cache DIR --c2 DIR]
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import time
import numpy as np
import torch
from torch import nn
from .universal_c2_train import load, concat, model_probs, fit, NAD, VARIANTS
from .universal_c2_data import expected_timing
from .universal_features import NAMES
from .universal_train import TYPES, Norm, balanced

STORE = Path('/media/sudeep/Storage/drone_universal_classifier/2026-11-16')
CACHE, C2 = STORE / 'cache-v3', STORE / 'c2-data-v4'
POOL = ['train', 'val', 'test', 'unit_test', 'cross_test']
FOLDS = ['DroneRFa', 'KULeuven_DroneRF', 'DRFF-R2', 'DroneRFb-DIR', 'RFUAV']
NAME_LIMIT = .6      # a view counts as named when its top model probability reaches this (v3 DJI limit 0.57-0.64)


def subset(d, keep):
    keep = np.asarray(keep, bool)
    return dict(spec=d['spec'][keep], facts=d['facts'][keep], available=d['available'][keep],
                meta=[m for m, k in zip(d['meta'], keep) if k])


def fold_data(fold, variants):
    parts = {p: load(CACHE, p) for p in POOL}
    drone = lambda m: m['type'] != 'not_a_drone'
    out_d = lambda m: m['dataset'] == fold
    train = concat([subset(parts[p], [not out_d(m) for m in parts[p]['meta']]) for p in POOL if p != 'val'])
    cache_windows = len(train['meta'])         # input scaling from these, as v3 (the cache's own windows)
    mix_tr, mix_va = load(C2, 'mix_train'), load(C2, 'mix_val')
    train = concat([train, load(C2, 'bg_site_train'), subset(mix_tr, [not out_d(m) for m in mix_tr['meta']])])
    val = {'val': subset(parts['val'], [not out_d(m) for m in parts['val']['meta']]),
           'mix_val': subset(mix_va, [not out_d(m) for m in mix_va['meta']]), 'bg_site_val': load(C2, 'bg_site_val')}
    trained = {m['model'] for m in train['meta'] if drone(m)}
    held = concat([subset(parts[p], [out_d(m) and drone(m) and m['model'] in trained for m in parts[p]['meta']])
                   for p in POOL])
    return train, val, held, cache_windows


def recording_scores(meta, gt, ml, allowed_m, models, model_type):
    """Per held-out model: per view (correct type) top-1/top-3 and named at NAME_LIMIT; per recording top-1."""
    y = np.array([TYPES.index(m['type']) for m in meta]); p = gt.argmax(1)
    view = defaultdict(lambda: dict(views=0, type_right=0, top1=0, top3=0, named=0, named_right=0, predicted=Counter(),
                                    named_as=Counter()))
    rec = defaultdict(list)
    for i, m in enumerate(meta):
        v = view[m['model']]; v['views'] += 1
        if p[i] != y[i]:
            continue
        v['type_right'] += 1
        mp = model_probs(ml[i:i + 1], y[i], model_type, allowed_m[i:i + 1])[0]
        order = np.argsort(-mp); target = models.index(m['model'])
        v['top1'] += int(order[0] == target); v['top3'] += int(target in order[:3])
        v['predicted'][models[order[0]]] += 1
        if mp[order[0]] >= NAME_LIMIT:
            v['named'] += 1; v['named_right'] += int(order[0] == target); v['named_as'][models[order[0]]] += 1
        rec[(m['model'], m['recording_id'])].append(mp)
    out = {}
    for model, v in sorted(view.items()):
        recs = [(k, np.mean(ps, 0)) for k, ps in rec.items() if k[0] == model]
        r1 = sum(int(np.argmax(mp) == models.index(model)) for _, mp in recs)
        out[model] = dict(views=v['views'], type_right=v['type_right'], top1=v['top1'], top3=v['top3'],
                          named=v['named'], named_right=v['named_right'], recordings=len(recs), recording_top1=r1,
                          predicted=dict(v['predicted'].most_common(4)), named_as=dict(v['named_as']))
    return out


def gate_scores(run, models, model_type):
    out = {}
    d = load(CACHE, 'open_test'); tp, gt, ml, am = run.predict(d)
    y = np.array([TYPES.index(m['type']) for m in d['meta']]); p = gt.argmax(1)
    named = 0; drones = 0; named_as = Counter()
    for i, m in enumerate(d['meta']):
        if m['type'] in ('dji_ofdm', 'lightbridge') and p[i] == y[i]:
            drones += 1
            mp = model_probs(ml[i:i + 1], y[i], model_type, am[i:i + 1])[0]
            if mp.max() >= NAME_LIMIT:
                named += 1; named_as[models[int(mp.argmax())]] += 1
    out['open_test'] = dict(type_balanced=balanced(y, p), drone_views_typed=drones, named_at_limit=named,
                            named_as=dict(named_as))
    d = load(C2, 'bg_site_test'); _, gt, _, _ = run.predict(d)
    out['bg_site_test'] = dict(false_rate=float((gt.argmax(1) != NAD).mean()), views=len(d['meta']))
    return out


def given_to_others(fold_result):
    """Per model name: views of other drones given that name in one fold (held-out views of other
    models, and views of never-seen models). A name is only right if it is right every time it is
    given, so these count against it as much as its own views named wrong."""
    out = Counter()
    for model, s in fold_result['held'].items():
        for name, n in s['named_as'].items():
            if name != model:
                out[name] += n
    for name, n in fold_result['gates']['open_test']['named_as'].items():
        out[name] += n
    return out


def rescore(run_dirs, threads=16):
    """Re-score saved fold networks (`<run>/<fold>.pt`) and add the names given (`named_as`) to
    their results, without retraining. Refuses if the re-scored counts differ from the saved ones."""
    from .universal_c2_train import Runner, TwoLevel
    torch.set_num_threads(threads)
    for fold in FOLDS:
        todo = [Path(r) for r in run_dirs if (Path(r) / f'{fold}.pt').exists()]
        if not todo:
            continue
        _, _, held, _ = fold_data(fold, ())
        for run_dir in todo:
            ck = torch.load(run_dir / f'{fold}.pt', weights_only=False)
            res = json.loads((run_dir / f'{fold}.json').read_text())
            models, model_type = ck['models'], ck['model_type']
            net = TwoLevel(len(ck['norm']['mean']), len(models)); net.load_state_dict(ck['state_dict']); net.eval()
            run = Runner(net, Norm.from_state(ck['norm']), models, ck['timing'])
            tp, gt, ml, am = run.predict(held)
            scores = recording_scores(held['meta'], gt, ml, am, models, model_type)
            gates = gate_scores(run, models, model_type)
            for model, s in res['held'].items():
                new = scores[model]
                if (new['views'], new['named'], new['named_right']) != (s['views'], s['named'], s['named_right']):
                    raise ValueError(f'{run_dir.name}/{fold} {model}: re-scored {new} differs from saved {s}')
            if gates['open_test']['named_at_limit'] != res['gates']['open_test']['named_at_limit']:
                raise ValueError(f'{run_dir.name}/{fold}: open-set naming differs on re-scoring')
            res['held'], res['gates'] = scores, gates
            (run_dir / f'{fold}.json').write_text(json.dumps(res, indent=1))
            print(f'{run_dir.name}/{fold}: re-scored, counts match', flush=True)


def train(fold, variants, output, seed=1, epochs=20, threads=16):
    torch.manual_seed(seed); rng = np.random.default_rng(seed); torch.set_num_threads(threads)
    tr, val, held, cache_windows = fold_data(fold, variants)
    f = fit(tr, val, cache_windows, rng, epochs, variants)
    run, norm, models, model_type, timing, datasets, best, history = (
        f[k] for k in ('run', 'norm', 'models', 'model_type', 'timing', 'datasets', 'best', 'history'))
    tp, gt, ml, am = run.predict(held)
    result = dict(fold=fold, variants=sorted(variants), seed=seed, best_epoch=best[1], selection=best[0], models=models,
                  datasets=datasets, train_windows=len(tr['meta']), held_windows=len(held['meta']),
                  held_type_balanced=balanced(np.array([TYPES.index(m['type']) for m in held['meta']]), gt.argmax(1)),
                  held=recording_scores(held['meta'], gt, ml, am, models, model_type),
                  gates=gate_scores(run, models, model_type), history=history)
    output = Path(output); output.mkdir(parents=True, exist_ok=True)
    (output / f'{fold}.json').write_text(json.dumps(result, indent=1))
    torch.save(dict(state_dict=best[2], models=models, model_type=model_type, timing=timing, norm=norm.state(),
                    variants=sorted(variants), fold=fold, development_only=True), output / f'{fold}.pt')
    return result


def main():
    global CACHE, C2
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--fold', action='append', choices=FOLDS)
    p.add_argument('--variant', action='append', default=[], choices=VARIANTS)
    p.add_argument('--seed', type=int, default=1); p.add_argument('--epochs', type=int, default=20)
    p.add_argument('--threads', type=int, default=16)
    p.add_argument('--rescore', type=Path, action='append', help='re-score this saved run (adds named_as); no training')
    p.add_argument('--cache', type=Path, default=CACHE, help='cache partitions (default: v3 data)')
    p.add_argument('--c2', type=Path, default=C2, help='site background and mixed views (default: v3 data)')
    a = p.parse_args()
    CACHE, C2 = a.cache, a.c2
    if a.rescore:
        return rescore(a.rescore, a.threads)
    for fold in a.fold or FOLDS:
        r = train(fold, set(a.variant), a.output, a.seed, a.epochs, a.threads)
        print(json.dumps(dict(fold=fold, best_epoch=r['best_epoch'], held={k: (v['recording_top1'], v['recordings'],
              v['top1'], v['type_right']) for k, v in r['held'].items()}, gates=r['gates'])), flush=True)


if __name__ == '__main__':
    main()
