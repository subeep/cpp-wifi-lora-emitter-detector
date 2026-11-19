"""D1: one model file for the app: network, thresholds, naming limits and measured accuracy.

The package holds everything the live worker needs, so nothing is hard-coded in C++:
  - the C2 network weights, input normalisation, model list, model types and timing keys;
  - per-model mean embeddings and the pooled precision (the distance check);
  - the C4 decision file (type thresholds, per-type naming limits, row rule);
  - the measured accuracy per model from the C4 report of the same seed (shown in the app);
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
        out[name] = entry
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


def runner_from(ck, mask):
    from .universal_c2_train import Runner, TwoLevel
    from .universal_features import NAMES
    from .universal_train import Norm
    net = TwoLevel(len(NAMES), len(ck['models']), ck.get('use_facts', True)); net.load_state_dict(ck['state_dict'])
    state = dict(ck['norm'])
    if mask is not None:
        state['scale_mask'] = np.asarray(mask, bool).tolist()
    return Runner(net, Norm.from_state(state), ck['models'], ck['timing']), ck


def build(models_dir, output, cache, c2, seed=None):
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
    package = dict(
        format=FORMAT, architecture=ARCHITECTURE, development_only=True,
        state_dict=ck['state_dict'], types=ck['types'], models=ck['models'], model_type=ck['model_type'],
        timing=ck['timing'], norm=norm, use_facts=ck.get('use_facts', True), open_set=ck.get('open_set'),
        class_stats=ck['class_stats'], decision=decision, accuracy=accuracy(report, ck['models']),
        background=dict((k, report['background'][k]) for k in ('captures', 'typed_captures', 'rows', 'named_rows', 'hours')),
        input=dict(rate_hz=20_000_000, samples=160_000, duration_seconds=.008, intervals_per_capture=3,
                   scale='complex float, full scale 1.0 (int16 / 32768)'),
        seed=seed, seed_choice='highest validation selection score of the trained seeds',
        scale_mask_check=f'fixed mask reproduces the evaluated outputs on {checked} validation views',
        selection={s: r['selection'] for s, r in runs.items()},
        sources={p.name: sha256(p) for p in (checkpoint, decision_path, report_path)},
        gates=dict(background_false_rows='not established (0 rows in 0.33 h)', naming_point='strict (C4 v3)'),
        created=datetime.now().astimezone().isoformat())
    output = Path(output); output.parent.mkdir(parents=True, exist_ok=True)
    torch.save(package, output)
    summary = {k: package[k] for k in ('format', 'architecture', 'seed', 'seed_choice', 'selection', 'scale_mask_check',
                                       'sources', 'gates', 'background')}
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
    a = p.parse_args()
    print(json.dumps(build(a.models, a.output, a.cache, a.c2, a.seed), indent=1))


if __name__ == '__main__':
    main()
