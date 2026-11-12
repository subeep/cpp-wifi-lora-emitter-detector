"""Diagnostic: does training a model name only on windows where its link is measured present
improve naming on unseen aircraft? DRFF-R2 development data only; no locked unit, no deployable weights.

Two training conditions on identical units, sampler, epochs and seeds:
  unfiltered     - every activity-eligible original window (the current practice)
  link_filtered  - only windows whose own 20 MSps view shows the class's measured symbol timing
Evaluation is on calibration units (aircraft never trained), split by link present / absent.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import numpy as np
import torch
from torch import nn
from .common import sha256, write_json
from .model import SpectrogramCNN

# Expected timing per class, measured on training units (docs/DRONE_RF_T07_CP_LAG_PROBE_2026-11-10.md).
EXPECTED = {'DJI_Air_2': 'lte_like_15kHz', 'DJI_Air_2S': 'lte_like_15kHz', 'DJI_Mavic_3': 'lte_like_15kHz',
            'DJI_Mini_5_Pro': 'lte_like_15kHz', 'DJI_Mini_4_Pro': 'lte_like_30kHz'}
CAP = 500        # max training windows per (class, unit, receiver)


def receiver(row):
    return row['source'].rsplit('_', 2)[-2]


def unit(row):
    return row['split_group'].split(':', 1)[1].replace(':aircraft_unit_', '_')


def load(root, rows):
    cache, x = {}, np.empty((len(rows), 256, 625), np.float32)
    for i, r in enumerate(rows):
        if r['shard'] not in cache:
            cache[r['shard']] = np.load(root / r['shard'], mmap_mode='r', allow_pickle=False)
        x[i] = cache[r['shard']][r['shard_index']]
    return x


def train_eval(xtr, ytr, cells, xte, classes, seed, epochs, device):
    mean, scale = float(xtr.mean()), float(xtr.std())
    count = Counter(cells); per_class = Counter(c[0] for c in count)
    weights = torch.tensor([1 / (len(classes) * per_class[c[0]] * count[c]) for c in cells], dtype=torch.double)
    torch.manual_seed(seed)
    model = SpectrogramCNN(len(classes)).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=1e-3, weight_decay=.01)
    steps = epochs * max(1, len(ytr) // 32)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=1e-3, total_steps=steps)
    xt, yt = torch.from_numpy((xtr - mean) / scale), torch.from_numpy(ytr)
    gen = torch.Generator().manual_seed(seed)
    model.train()
    for _ in range(steps):
        idx = torch.multinomial(weights, 32, replacement=True, generator=gen)
        xb = torch.roll(xt[idx][:, None].to(device), int(torch.randint(0, 625, (1,), generator=gen)), dims=3)
        loss = nn.functional.cross_entropy(model(xb)[0], yt[idx].to(device), label_smoothing=.05)
        opt.zero_grad(); loss.backward(); opt.step(); sched.step()
    model.eval()
    xe = torch.from_numpy((xte - mean) / scale)
    with torch.no_grad():
        return torch.cat([torch.softmax(model(xe[i:i + 64][:, None].to(device))[0], 1).cpu() for i in range(0, len(xe), 64)]).numpy()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus', type=Path, required=True)
    p.add_argument('--timing', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--seeds', type=int, default=3)
    p.add_argument('--epochs', type=int, default=12)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((a.corpus / 'corpus.json').read_text())
    timing = json.loads(a.timing.read_text())
    ratio = timing['ratio']
    view = {w['index']: w['view'] for w in timing['windows']}
    classes = sorted(EXPECTED)
    rows = [r for r in manifest['records'] if r['dataset'] == 'DRFF-R2' and r['augmentation'] is None
            and r['model_eligible'] and r['label'] in EXPECTED and r['index'] in view]
    present = lambda r: view[r['index']].get(EXPECTED[r['label']], 0) > ratio
    rng = np.random.default_rng(2026)
    pools = defaultdict(list)
    for r in rows:
        if r['partition'] == 'train':
            pools[(r['label'], unit(r), receiver(r))].append(r)
    test = [r for r in rows if r['partition'] == 'calibration']
    xte = load(a.corpus, test)
    device = 'cuda' if torch.cuda.is_available() else 'cpu'
    torch.set_num_threads(4)
    summary = dict(schema='rfmon.drone_ml.link_filtered_identity.v1', classes=classes, expected_timing=EXPECTED, ratio=ratio,
                   corpus_sha256=sha256(a.corpus / 'corpus.json'), timing_sha256=sha256(a.timing), device=device,
                   epochs=a.epochs, architecture='SpectrogramCNN', locked_test_read=False,
                   train_link_present={f'{k[0]}|{k[1]}|{k[2]}': [sum(map(present, v)), len(v)] for k, v in sorted(pools.items())},
                   eval_link_present={u: [sum(present(r) for r in test if unit(r) == u), sum(unit(r) == u for r in test)] for u in sorted({unit(r) for r in test})},
                   runs=[])
    for condition in ('unfiltered', 'link_filtered'):
        chosen = []
        for key, items in sorted(pools.items()):
            items = [r for r in items if condition == 'unfiltered' or present(r)]
            if len(items) > CAP:
                items = [items[i] for i in sorted(rng.choice(len(items), CAP, replace=False))]
            chosen += items
        xtr = load(a.corpus, chosen)
        ytr = np.array([classes.index(r['label']) for r in chosen])
        cells = [(classes.index(r['label']), unit(r), receiver(r)) for r in chosen]
        for seed in range(a.seeds):
            probs = train_eval(xtr, ytr, cells, xte, classes, seed, a.epochs, device)
            pred = probs.argmax(1)
            out = dict(condition=condition, seed=seed, train_windows=dict(Counter(f"{r['label']}|{unit(r)}|{receiver(r)}" for r in chosen)), subsets={})
            for name, keep in (('all', lambda r: True), ('link_present', present), ('link_absent', lambda r: not present(r))):
                idx = [i for i, r in enumerate(test) if keep(r)]
                per_unit, votes, recs = defaultdict(lambda: [0, 0]), defaultdict(lambda: [0, 0]), defaultdict(list)
                confusion = Counter()
                for i in idx:
                    r = test[i]; ok = classes[pred[i]] == r['label']
                    c = per_unit[unit(r)]; c[0] += int(ok); c[1] += 1
                    recs[(unit(r), r['recording_id'], r['label'])].append(probs[i])
                    confusion[f"{r['label']}->{classes[pred[i]]}"] += 1
                for (u, _, label), ps in recs.items():
                    v = votes[u]; v[0] += int(classes[int(np.mean(ps, 0).argmax())] == label); v[1] += 1
                by_class = defaultdict(lambda: [0, 0])
                for i in idx:
                    c = by_class[test[i]['label']]; c[0] += int(classes[pred[i]] == test[i]['label']); c[1] += 1
                out['subsets'][name] = dict(windows=len(idx), per_unit=dict(per_unit), recording_votes=dict(votes), confusion=dict(confusion),
                    balanced_accuracy=float(np.mean([c[0] / c[1] for c in by_class.values()])) if by_class else None)
            summary['runs'].append(out)
            print(condition, seed, {k: (v['windows'], None if v['balanced_accuracy'] is None else round(v['balanced_accuracy'], 3)) for k, v in out['subsets'].items()}, flush=True)
            write_json(a.output / 'results.json', summary)


if __name__ == '__main__':
    main()
