"""Diagnostic: can a drone model be named from a single enrolled aircraft?

DroneRFb-DIR link-present views (six types, three aircraft each). Aircraft 3 of every type is used
only for testing. Three conditions:
  two_aircraft_classifier  ordinary classifier trained on aircraft 1 and 2 of all six types
  one_aircraft_classifier  ordinary classifier trained on aircraft 1 only
  enrol_new_type           the network never sees one type at all (trained on the other five,
                           aircraft 1 and 2); every type is then enrolled as a stored reference
                           from aircraft 1 only, and test windows take the nearest reference
No deployable weights are written.
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

CAP = 400


def train(x, y, cells, classes, seed, epochs, device):
    count = Counter(cells); per_class = Counter(c[0] for c in count)
    weights = torch.tensor([1 / (classes * per_class[c[0]] * count[c]) for c in cells], dtype=torch.double)
    torch.manual_seed(seed)
    model = SpectrogramCNN(classes).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=1e-3, weight_decay=.01)
    steps = epochs * max(1, len(y) // 32)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=1e-3, total_steps=steps)
    yt = torch.from_numpy(y); gen = torch.Generator().manual_seed(seed)
    model.train()
    for _ in range(steps):
        idx = torch.multinomial(weights, 32, replacement=True, generator=gen)
        xb = torch.roll(x[idx][:, None].to(device), int(torch.randint(0, 625, (1,), generator=gen)), dims=3)
        loss = nn.functional.cross_entropy(model(xb)[0], yt[idx].to(device), label_smoothing=.05)
        opt.zero_grad(); loss.backward(); opt.step(); sched.step()
    return model.eval()


def forward(model, x, device):
    logits, emb = [], []
    with torch.no_grad():
        for i in range(0, len(x), 64):
            a, b = model(x[i:i + 64][:, None].to(device))
            logits.append(a.cpu()); emb.append(b.cpu())
    return torch.cat(logits).numpy(), torch.cat(emb).numpy()


def score(rows, pred, types):
    """Window and segment-vote results per type; pred is a list of predicted type names."""
    out = {}
    for t in types:
        idx = [i for i, r in enumerate(rows) if r['type'] == t]
        seg = defaultdict(list)
        for i in idx:
            seg[rows[i]['original']].append(pred[i])
        votes = [Counter(v).most_common(1)[0][0] == t for v in seg.values()]
        out[t] = dict(windows=[sum(pred[i] == t for i in idx), len(idx)], segments=[sum(votes), len(votes)],
                      called=dict(Counter(pred[i] for i in idx)))
    out['balanced_window_accuracy'] = float(np.mean([out[t]['windows'][0] / out[t]['windows'][1] for t in types]))
    out['balanced_segment_accuracy'] = float(np.mean([out[t]['segments'][0] / out[t]['segments'][1] for t in types]))
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--views', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--seeds', type=int, default=2)
    p.add_argument('--epochs', type=int, default=12)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    meta = json.loads((a.views / 'meta.json').read_text())
    images = np.load(a.views / 'images.npy', mmap_mode='r')
    rows = [r for r in meta['rows'] if r['type'] != 'background' and r['link']]
    types = sorted({r['type'] for r in rows})
    rng = np.random.default_rng(2026)
    pools = defaultdict(list)
    for r in rows:
        pools[(r['type'], r['individual'])].append(r)
    capped = {k: (v if len(v) <= CAP else [v[i] for i in sorted(rng.choice(len(v), CAP, replace=False))]) for k, v in pools.items()}
    test = [r for r in rows if r['individual'] == '3']
    load = lambda rs: np.stack([images[r['image_index']] for r in rs]).astype(np.float32)
    xte_raw = load(test)
    device = 'cuda' if torch.cuda.is_available() else 'cpu'
    torch.set_num_threads(4)
    summary = dict(schema='rfmon.drone_ml.enrolment_experiment.v1', views_sha256=sha256(a.views / 'meta.json'), device=device, epochs=a.epochs,
                   architecture='SpectrogramCNN', types=types, link_present_windows={f'{k[0]}{k[1]}': len(v) for k, v in sorted(pools.items())},
                   test_windows=len(test), cap=CAP, runs=[])

    def fit(train_rows, class_types, seed):
        x = load(train_rows); mean, scale = float(x.mean()), float(x.std())
        y = np.array([class_types.index(r['type']) for r in train_rows])
        cells = [(class_types.index(r['type']), r['individual']) for r in train_rows]
        model = train(torch.from_numpy((x - mean) / scale), y, cells, len(class_types), seed, a.epochs, device)
        return model, (lambda z: torch.from_numpy((z - mean) / scale))

    for seed in range(a.seeds):
        for name, aircraft in (('two_aircraft_classifier', ('1', '2')), ('one_aircraft_classifier', ('1',))):
            rows_tr = [r for t in types for i in aircraft for r in capped[(t, i)]]
            model, norm = fit(rows_tr, types, seed)
            logits, emb = forward(model, norm(xte_raw), device)
            res = score(test, [types[k] for k in logits.argmax(1)], types)
            # Same network, but naming by nearest reference built from aircraft 1 only.
            ref = {t: forward(model, norm(load(capped[(t, '1')])), device)[1].mean(0) for t in types}
            proto = np.stack([ref[t] / np.linalg.norm(ref[t]) for t in types])
            res_ref = score(test, [types[k] for k in (emb @ proto.T).argmax(1)], types)
            summary['runs'].append(dict(condition=name, seed=seed, train_windows=len(rows_tr), classifier=res, nearest_reference_from_aircraft_1=res_ref))
            print(name, seed, 'classifier', round(res['balanced_window_accuracy'], 3), 'segments', round(res['balanced_segment_accuracy'], 3),
                  '| reference', round(res_ref['balanced_window_accuracy'], 3), flush=True)
            write_json(a.output / 'results.json', summary)
        for held in types:
            others = [t for t in types if t != held]
            rows_tr = [r for t in others for i in ('1', '2') for r in capped[(t, i)]]
            model, norm = fit(rows_tr, others, seed)
            emb = forward(model, norm(xte_raw), device)[1]
            out = dict(condition='enrol_new_type', seed=seed, held_out_type=held, train_windows=len(rows_tr))
            for label, aircraft in (('enrolled_from_aircraft_1', ('1',)), ('enrolled_from_aircraft_1_and_2', ('1', '2'))):
                ref = {t: forward(model, norm(load([r for i in aircraft for r in capped[(t, i)]])), device)[1].mean(0) for t in types}
                proto = np.stack([ref[t] / np.linalg.norm(ref[t]) for t in types])
                res = score(test, [types[k] for k in (emb @ proto.T).argmax(1)], types)
                out[label] = dict(held_out=res[held], balanced_window_accuracy=res['balanced_window_accuracy'],
                                  balanced_segment_accuracy=res['balanced_segment_accuracy'], all_types={t: res[t] for t in types})
            summary['runs'].append(out)
            h = out['enrolled_from_aircraft_1']['held_out']
            print('enrol_new_type', seed, held, 'held-out windows', h['windows'], 'segments', h['segments'], 'called', h['called'],
                  '| all six balanced', round(out['enrolled_from_aircraft_1']['balanced_window_accuracy'], 3), flush=True)
            write_json(a.output / 'results.json', summary)


if __name__ == '__main__':
    main()
