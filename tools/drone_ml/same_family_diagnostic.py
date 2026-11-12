"""T09/T26 pairwise same-family diagnostic on existing reviewed tensors.

Separate, versioned experiment: it never writes weights for deployment, never reads
source IQ or locked test units, and never feeds the main identity model. It asks one
question: can a spectral model trained on some physical aircraft tell two models apart
on *other* physical aircraft, for a same-link-family pair versus a size-matched
cross-family control pair.
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

G = 'DRFF-R2:{}:aircraft_unit_{}'
A2S, M3P, A2 = 'mavicAir2s', 'mini3pro', 'mavicAir2'
# (pair, fold) -> {class: (train units, eval units)}. Frozen before any training.
SPLITS = {
    'same_family_O3': {
        'fold_X': {A2S: ((2, 3), (1, 4)), M3P: ((1,), (2,))},
        'fold_Y': {A2S: ((1, 2), (3, 4)), M3P: ((2,), (1,))}},
    'cross_family_control': {
        'fold_X': {A2S: ((2, 3), (1, 4)), A2: ((3,), (1,))},
        'fold_Y': {A2S: ((1, 2), (3, 4)), A2: ((1,), (3,))}},
    # Added 10 Nov after the lag probe: both models measure 15 kHz-only symbol timing.
    'same_timing_15kHz': {
        'fold_X': {A2: ((2, 3), (1, 4)), M3P: ((1,), (2,))},
        'fold_Y': {A2: ((1, 2), (3, 4)), M3P: ((2,), (1,))}},
}
# The control's second class is subsampled to the Mini 3 Pro unit's per-receiver counts.
MATCH = {1: {'u2': 108}, 2: {'u1': 336, 'u2': 36}}     # Mini 3 Pro unit -> receiver -> windows
CONTROL_AS = {3: 1, 1: 2}                               # Air 2 unit plays the role of this Mini 3 Pro unit
CAP = 400                                               # max train windows per (class, unit, receiver)


def receiver(row):
    return row['source'].rsplit('_', 2)[-2]


def select(rows, model, units, rng, cap=None, match=None):
    out = []
    for unit in units:
        by_rx = defaultdict(list)
        for r in rows:
            if r['split_group'] == G.format(model, unit):
                by_rx[receiver(r)].append(r)
        for rx, items in sorted(by_rx.items()):
            limit = match[CONTROL_AS[unit]].get(rx, 0) if match else cap
            if limit is not None and len(items) > limit:
                items = [items[i] for i in sorted(rng.choice(len(items), limit, replace=False))]
            out += [(r, unit, rx) for r in items]
    return out


def load(root, items):
    cache, x = {}, np.empty((len(items), 256, 625), np.float32)
    for i, (r, _, _) in enumerate(items):
        if r['shard'] not in cache:
            cache[r['shard']] = np.load(root / r['shard'], mmap_mode='r', allow_pickle=False)
        x[i] = cache[r['shard']][r['shard_index']]
    return x


def run(root, pair, fold, seed, epochs, device):
    manifest = json.loads((root / 'corpus.json').read_text())
    rows = [r for r in manifest['records'] if r['model_eligible'] and r['augmentation'] is None
            and r['partition'] in ('train', 'calibration', 'unknown_calibration')]
    rng = np.random.default_rng(1000 + seed)
    spec = SPLITS[pair][fold]
    classes = list(spec)
    train, test = [], []
    for label, model in enumerate(classes):
        tr_units, ev_units = spec[model]
        control = model == A2 and pair == 'cross_family_control'
        tr = select(rows, model, tr_units, rng, cap=CAP, match=MATCH if control else None)
        ev = select(rows, model, ev_units, rng, match=MATCH if control else None)
        train += [(r, u, rx, label) for r, u, rx in tr]
        test += [(r, u, rx, label) for r, u, rx in ev]
    if {t[0]['split_group'] for t in train} & {t[0]['split_group'] for t in test}:
        raise ValueError('physical unit leaked between train and evaluation')
    xtr, xte = load(root, [t[:3] for t in train]), load(root, [t[:3] for t in test])
    mean, scale = float(xtr.mean()), float(xtr.std())           # training data only
    xtr, xte = (xtr - mean) / scale, (xte - mean) / scale
    ytr = np.array([t[3] for t in train])
    # Balance class x receiver so receiver cannot stand in for the label during training.
    cells = Counter((t[3], t[2]) for t in train)
    per_class_rx = Counter(c for c, _ in cells)
    weights = torch.tensor([1 / (len(classes) * per_class_rx[t[3]] * cells[(t[3], t[2])]) for t in train], dtype=torch.double)
    torch.manual_seed(seed)
    model = SpectrogramCNN(len(classes)).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=1e-3, weight_decay=.01)
    steps = epochs * max(1, len(train) // 32)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=1e-3, total_steps=steps)
    xt, yt = torch.from_numpy(xtr), torch.from_numpy(ytr)
    gen = torch.Generator().manual_seed(seed)
    model.train()
    for _ in range(steps):
        idx = torch.multinomial(weights, 32, replacement=True, generator=gen)
        xb = xt[idx][:, None].to(device)
        xb = torch.roll(xb, int(torch.randint(0, 625, (1,), generator=gen)), dims=3)   # time shift only
        loss = nn.functional.cross_entropy(model(xb)[0], yt[idx].to(device), label_smoothing=.05)
        opt.zero_grad(); loss.backward(); opt.step(); sched.step()
    model.eval()
    with torch.no_grad():
        probs = torch.cat([torch.softmax(model(torch.from_numpy(xte[i:i + 64])[:, None].to(device))[0], 1).cpu()
                           for i in range(0, len(xte), 64)]).numpy()
        tr_acc = float(np.mean(np.concatenate([model(xt[i:i + 64][:, None].to(device))[0].argmax(1).cpu().numpy()
                                               for i in range(0, len(xt), 64)]) == ytr))
    pred = probs.argmax(1)
    cells_out, recs = defaultdict(lambda: [0, 0]), defaultdict(list)
    for (r, unit, rx, label), p, pr in zip(test, pred, probs):
        c = cells_out[f'{classes[label]}_{unit}/{rx}']; c[0] += int(p == label); c[1] += 1
        recs[(classes[label], unit, r['recording_id'], label)].append(pr)
    per_class = [np.mean([p == t[3] for t, p in zip(test, pred) if t[3] == k]) for k in range(len(classes))]
    votes = defaultdict(lambda: [0, 0])
    for (name, unit, _, label), ps in recs.items():
        v = votes[f'{name}_{unit}']; v[0] += int(np.mean(ps, 0).argmax() == label); v[1] += 1
    return dict(pair=pair, fold=fold, seed=seed, classes=classes, train_windows=dict(Counter(f'{classes[t[3]]}_{t[1]}/{t[2]}' for t in train)),
                eval_windows_correct_total=dict(cells_out), recording_votes_correct_total=dict(votes),
                balanced_window_accuracy=float(np.mean(per_class)), per_class_recall=[float(v) for v in per_class],
                final_train_loss=float(loss), train_window_accuracy=tr_acc, normalization=dict(mean=mean, scale=scale))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--seeds', type=int, default=3)
    p.add_argument('--epochs', type=int, default=12)
    p.add_argument('--pairs', nargs='+', choices=list(SPLITS), default=list(SPLITS))
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    write_json(a.output / 'split.json', dict(schema='rfmon.drone_ml.same_family_diagnostic_split.v1', splits=SPLITS, pairs_run=a.pairs,
        control_match=MATCH, train_cap=CAP, corpus_sha256=sha256(a.corpus / 'corpus.json'),
        rows='model_eligible original (non-augmented) windows only', locked_test_read=False, source_iq_read=False,
        note='diagnostic only; Mini 3 Pro remains the unknown model in the main experiment'))
    device = 'cuda' if torch.cuda.is_available() else 'cpu'
    torch.set_num_threads(4)
    results = []
    for pair in a.pairs:
        for fold in SPLITS[pair]:
            for seed in range(a.seeds):
                results.append(run(a.corpus, pair, fold, seed, a.epochs, device))
                r = results[-1]
                print(pair, fold, seed, 'balanced', round(r['balanced_window_accuracy'], 3), r['eval_windows_correct_total'], flush=True)
                write_json(a.output / 'results.json', dict(schema='rfmon.drone_ml.same_family_diagnostic.v1', device=device,
                    torch=torch.__version__, epochs=a.epochs, architecture='SpectrogramCNN', runs=results, complete=False))
    write_json(a.output / 'results.json', dict(schema='rfmon.drone_ml.same_family_diagnostic.v1', device=device,
        torch=torch.__version__, epochs=a.epochs, architecture='SpectrogramCNN', runs=results, complete=True))


if __name__ == '__main__':
    main()
