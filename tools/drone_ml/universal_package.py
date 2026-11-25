"""D1: one model file for the app: network, thresholds, naming limits and measured accuracy.

The package holds everything the live worker needs, so nothing is hard-coded in C++:
  - the C2 network weights, input normalisation, model list, model types and timing keys;
  - per-model mean embeddings and the pooled precision (the distance check);
  - the C4 decision file (type thresholds, per-type naming limits, row rule);
  - the measured accuracy per model from the C4 report of the same seed (shown in the app); with
    --lodo, the other-dataset accuracy comes from a leave-one-dataset-out run of the same recipe
    (`universal_lodo`), counted in views (the app's naming gate reads it);
  - provenance: source files and their SHA-256, the seed and why it was chosen.
The seed is chosen on validation only (highest C2 selection score), never on test results.
The facts scale mask is fixed from the same training data (C2 checkpoints re-derived it per
batch); the build checks that the fixed mask gives the evaluated outputs on validation.
Development only: the background false-row gate is not yet established.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import numpy as np
import torch
from tools.drone_ml.common import sha256

FORMAT = 'rfmon.drone_ml.universal_package.v1'
ARCHITECTURE = 'universal_two_level_v1'


def accuracy(seed_report, models):
    """Per model: captures, named, top-1 right when named, on unseen aircraft and on held-out unit recordings."""
    out = {}
    for name in models:
        entry = {}
        for part, label in (('test', 'unseen_aircraft'), ('unit_test', 'same_unit'), ('cross_test', 'other_dataset')):
            c = seed_report[part]['by_model'].get(name)
            if c:
                entry[label] = dict(captures=c['captures'], named=c.get('captures_named', 0),
                                    top1_right=c.get('captures_top1_right', 0), top3_right=c.get('captures_top3_right', 0))
                if label == 'other_dataset':
                    entry[label].update(unit='captures', basis='cross_test partition, C4 rule')
        out[name] = entry
    return out


def lodo_accuracy(run_dir, variants):
    """Per model, summed over the folds of one `universal_lodo` run: views of the held-out dataset's
    recordings, views named (top model probability at the run's limit) and named right, and
    `given_to_others`: views of other drones given this model's name (other held-out models and
    never-seen models), so the app can judge a name over every time it is given. The run must use
    the package's recipe (same variants), so the numbers describe this kind of network."""
    folds = [json.loads(f.read_text()) for f in sorted(Path(run_dir).glob('*.json'))]
    if not folds:
        raise ValueError(f'no leave-one-dataset-out folds in {run_dir}')
    for f in folds:
        if sorted(f['variants']) != sorted(variants):
            raise ValueError(f"fold {f['fold']} used variants {f['variants']}, the package {sorted(variants)}")
    from .universal_lodo import NAME_LIMIT, given_to_others
    for f in folds:
        if 'named_as' not in f['gates']['open_test']:
            raise ValueError(f"fold {f['fold']} has no names-given counts: re-score it (universal_lodo --rescore)")
    out = {}
    given = {}
    for f in folds:
        for name, n in given_to_others(f).items():
            given[name] = given.get(name, 0) + n
    for f in folds:
        for model, s in f['held'].items():
            e = out.setdefault(model, dict(captures=0, named=0, top1_right=0, given_to_others=given.get(model, 0),
                                           unit='views', held_out=[],
                                           basis=f'leave one dataset out ({Path(run_dir).name}, seed {f["seed"]}); '
                                                 f'named at top model probability >= {NAME_LIMIT}'))
            e['captures'] += s['views']; e['named'] += s['named']; e['top1_right'] += s['named_right']
            e['held_out'].append(f['fold'])
    return out


def with_lodo(acc, lodo):
    """`other_dataset` from the leave-one-dataset-out run wherever it measured the model (it holds
    every dataset out in turn, not only the small cross_test partition); otherwise kept as is."""
    out = {name: dict(entry) for name, entry in acc.items()}
    for name, entry in out.items():
        if name in lodo:
            entry['other_dataset'] = lodo[name]
    return out


def scale_mask(cache, c2, mix=True):
    from .universal_c2_train import concat, load
    tr = concat([load(cache, 'train'), load(c2, 'bg_site_train')] + ([load(c2, 'mix_train')] if mix else []))
    return np.abs(np.nan_to_num(tr['facts'])).max(0) < 1e-2


def check_mask(ck, mask, cache):
    """The fixed mask must reproduce the batch-rule outputs the scorecard was measured with."""
    from .universal_c2_train import load
    run, _ = runner_from(ck, None); fixed, _ = runner_from(ck, mask)
    d = load(cache, 'val')
    a, b = run.predict(d)[1], fixed.predict(d)[1]
    if not np.allclose(a, b, atol=1e-6):
        raise ValueError('fixed scale mask changes validation outputs')
    return len(d['meta'])


def facts_of(ck):
    """The measured facts the network reads (v1: 29; v2 appends the Lightbridge symbol lag)."""
    from .universal_features import NAMES
    n = len(ck['norm']['mean'])
    return dict(version='universal_measured_facts_v1' if n == 29 else 'universal_measured_facts_v2', names=NAMES[:n])


def runner_from(ck, mask):
    from .universal_c2_train import Runner, TwoLevel
    from .universal_features import NAMES
    from .universal_train import Norm
    net = TwoLevel(len(ck['norm']['mean']), len(ck['models']), ck.get('use_facts', True)); net.load_state_dict(ck['state_dict'])
    state = dict(ck['norm'])
    if mask is not None:
        state['scale_mask'] = np.asarray(mask, bool).tolist()
    return Runner(net, Norm.from_state(state), ck['models'], ck['timing']), ck


def build(models_dir, output, cache, c2, seed=None, lodo=None):
    models_dir = Path(models_dir)
    runs = {int(p.stem.split('seed')[1]): json.loads(p.read_text()) for p in sorted(models_dir.glob('c2_seed?.json'))}
    if seed is None:
        seed = max(runs, key=lambda s: runs[s]['selection'])
    checkpoint = models_dir / f'c2_seed{seed}.pt'
    decision_path = models_dir / f'c2_seed{seed}_decision.json'
    report_path = models_dir / 'c4_report.json'
    ck = torch.load(checkpoint, weights_only=False)
    decision = json.loads(decision_path.read_text())
    report = json.loads(report_path.read_text())[f'c2_seed{seed}']
    if decision['models'] != ck['models']:
        raise ValueError('decision file does not match the checkpoint')
    mask = scale_mask(cache, c2, runs[seed].get('ablation', {}).get('mix', True))
    checked = check_mask(ck, mask, cache)
    norm = dict(ck['norm'], scale_mask=mask.tolist())
    acc = accuracy(report, ck['models'])
    if lodo:
        acc = with_lodo(acc, lodo_accuracy(lodo, ck.get('ablation', {}).get('variants', [])))
    package = dict(
        format=FORMAT, architecture=ARCHITECTURE, development_only=True,
        state_dict=ck['state_dict'], types=ck['types'], models=ck['models'], model_type=ck['model_type'],
        timing=ck['timing'], norm=norm, use_facts=ck.get('use_facts', True), open_set=ck.get('open_set'),
        class_stats=ck['class_stats'], decision=decision, accuracy=acc,
        background=dict((k, report['background'][k]) for k in ('captures', 'typed_captures', 'rows', 'named_rows', 'hours')),
        input=dict(rate_hz=20_000_000, samples=160_000, duration_seconds=.008, intervals_per_capture=3,
                   scale='complex float, full scale 1.0 (int16 / 32768)'),
        seed=seed, seed_choice='highest validation selection score of the trained seeds',
        scale_mask_check=f'fixed mask reproduces the evaluated outputs on {checked} validation views',
        selection={s: r['selection'] for s, r in runs.items()},
        recipe=dict(variants=ck.get('ablation', {}).get('variants', []), other_dataset_run=str(lodo) if lodo else None),
        facts=facts_of(ck),
        sources={p.name: sha256(p) for p in (checkpoint, decision_path, report_path)},
        gates=dict(background_false_rows='not established (0 rows in 0.33 h)', naming_point='strict (C4 v3)'),
        created=datetime.now().astimezone().isoformat())
    output = Path(output); output.parent.mkdir(parents=True, exist_ok=True)
    torch.save(package, output)
    summary = {k: package[k] for k in ('format', 'architecture', 'seed', 'seed_choice', 'selection', 'recipe', 'facts',
                                       'scale_mask_check', 'sources', 'gates', 'background')}
    summary['other_dataset'] = {m: {k: e['other_dataset'].get(k) for k in ('captures', 'named', 'top1_right', 'given_to_others', 'unit')}
                                for m, e in acc.items() if 'other_dataset' in e}
    from .universal_features import NAMES
    summary['scaled_facts'] = [n for n, m in zip(NAMES, mask) if m]
    summary['sha256'] = sha256(output)
    output.with_suffix('.json').write_text(json.dumps(summary, indent=1))
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--models', type=Path, required=True, help='directory with c2_seed*.pt/.json, decisions and c4_report.json')
    p.add_argument('--cache', type=Path, required=True); p.add_argument('--c2', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True); p.add_argument('--seed', type=int)
    p.add_argument('--lodo', type=Path, help='a `universal_lodo` run of the same recipe: its per-model results '
                                             'become the other_dataset accuracy')
    a = p.parse_args()
    print(json.dumps(build(a.models, a.output, a.cache, a.c2, a.seed, a.lodo), indent=1))


if __name__ == '__main__':
    main()
