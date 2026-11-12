"""Diagnostic: name drone types from physical segment descriptors instead of spectrogram pixels.

Input is the per-segment measurements written by descriptors_01/extract.py (autocorrelation lag
profile, mean power spectrum, power envelope). Features are built to ignore where in the band the
link sits and how strong it is. A plain softmax regression is trained on aircraft 1-2 (or 1 only)
and tested on aircraft 3 of every type. No deployable weights.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import numpy as np
from scipy.ndimage import median_filter
import torch
from .common import write_json


def lag_features(lag):
    """Peak-over-local-baseline of the lag profile, 5-150 us in 0.5 us bins (80 MSps input)."""
    ratio = lag / np.maximum(median_filter(lag, size=(1, 201), mode='nearest'), 1e-12)
    r = ratio[:, 400:12000]
    return np.log(np.maximum(r.reshape(len(r), -1, 40).max(axis=2), 1e-3))


def psd_features(psd):
    """Shift-invariant spectral shape: autocorrelation of the floor-removed spectrum over 0-40 MHz,
    plus occupied bandwidth and flatness."""
    floor = np.median(psd, axis=1, keepdims=True)
    c = np.maximum(psd - floor, 0)
    a = np.fft.ifft(np.abs(np.fft.fft(c, axis=1)) ** 2, axis=1).real[:, :512]
    a = a / np.maximum(a[:, :1], 1e-30)
    occupied = (psd > 4 * floor).mean(axis=1, keepdims=True)
    flat = np.exp(np.mean(np.log(np.maximum(psd, 1e-30)), axis=1, keepdims=True)) / psd.mean(axis=1, keepdims=True)
    return np.hstack([a.reshape(len(a), -1, 4).mean(axis=2), occupied, flat])


def env_features(env):
    """Burst and frame timing from the 10 us power envelope: autocorrelation at log-spaced lags up to
    20 ms, duty cycle, burst count and median burst length."""
    floor = np.percentile(env, 10, axis=1, keepdims=True)
    e = np.log(np.maximum(env / np.maximum(floor, 1e-30), 1e-3))
    c = e - e.mean(axis=1, keepdims=True)
    n = env.shape[1]
    acf = np.fft.irfft(np.abs(np.fft.rfft(c, 2 * n, axis=1)) ** 2, axis=1)[:, :2001]
    acf = acf / np.maximum(acf[:, :1], 1e-30)
    edges = np.unique(np.round(np.logspace(0, np.log10(2000), 61)).astype(int))
    pooled = np.stack([acf[:, a:b].mean(axis=1) for a, b in zip(edges[:-1], edges[1:])], axis=1)
    on = env > 4 * floor
    duty = on.mean(axis=1, keepdims=True)
    d = np.diff(np.pad(on.astype(np.int8), ((0, 0), (1, 1))), axis=1)
    count = (d == 1).sum(axis=1, keepdims=True)
    length = np.log1p(on.sum(axis=1, keepdims=True) / np.maximum(count, 1))
    return np.hstack([pooled, duty, np.log1p(count), length, e.std(axis=1, keepdims=True)])


def fit_predict(xtr, ytr, cells, xte, classes, seed, hidden=0):
    mean, std = xtr.mean(0), xtr.std(0) + 1e-6
    xt, xe = torch.tensor((xtr - mean) / std, dtype=torch.float32), torch.tensor((xte - mean) / std, dtype=torch.float32)
    count = Counter(cells); per_class = Counter(c[0] for c in count)
    w = torch.tensor([1 / (classes * per_class[c[0]] * count[c]) for c in cells], dtype=torch.float32)
    torch.manual_seed(seed)
    layers = [torch.nn.Linear(xt.shape[1], classes)] if not hidden else \
        [torch.nn.Linear(xt.shape[1], hidden), torch.nn.SiLU(), torch.nn.Dropout(.3), torch.nn.Linear(hidden, classes)]
    model = torch.nn.Sequential(*layers)
    opt = torch.optim.AdamW(model.parameters(), lr=3e-3, weight_decay=.1)
    y = torch.tensor(ytr)
    for _ in range(400):
        loss = (torch.nn.functional.cross_entropy(model(xt), y, reduction='none', label_smoothing=.05) * w).sum()
        opt.zero_grad(); loss.backward(); opt.step()
    model.eval()
    with torch.no_grad():
        return model(xe).argmax(1).numpy()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--features', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True, help='dronerfb_dir_probe results.json (link-found flag per segment)')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--seeds', type=int, default=3)
    a = p.parse_args()
    if a.output.exists():
        raise SystemExit('output exists; not overwritten')
    rows = json.loads((a.features / 'rows.json').read_text())
    raw = np.load(a.features / 'features.npz')
    found = {(r['split'], r['original']): r['group'] == '15kHz' for r in json.loads(a.probe.read_text())['results']}
    groups = dict(lag=lag_features(raw['lag']), psd=psd_features(raw['psd']), env=env_features(raw['env']))
    groups['all'] = np.hstack([groups['lag'], groups['psd'], groups['env']])
    types = sorted({r['type'] for r in rows} - {'background'})
    idx = lambda keep: np.array([i for i, r in enumerate(rows) if r['type'] != 'background' and keep(r)])
    test = idx(lambda r: r['individual'] == '3')
    link = np.array([found[(rows[i]['split'], rows[i]['original'])] for i in test])
    yte = np.array([types.index(rows[i]['type']) for i in test])
    out = dict(schema='rfmon.drone_ml.descriptor_naming.v1', types=types, feature_sizes={k: int(v.shape[1]) for k, v in groups.items()},
               test_segments=int(len(test)), test_link_found=int(link.sum()), runs=[])
    for label, aircraft in (('two_aircraft', ('1', '2')), ('one_aircraft', ('1',))):
        tr = idx(lambda r: r['individual'] in aircraft)
        ytr = np.array([types.index(rows[i]['type']) for i in tr])
        cells = [(types.index(rows[i]['type']), rows[i]['individual']) for i in tr]
        for name, x in groups.items():
            for model, hidden in (('softmax', 0), ('mlp64', 64)):
                for seed in range(a.seeds):
                    pred = fit_predict(x[tr], ytr, cells, x[test], len(types), seed, hidden)
                    res = {}
                    for subset, mask in (('all', np.ones(len(test), bool)), ('link_found', link), ('link_not_found', ~link)):
                        per = {t: [int((pred[mask & (yte == k)] == k).sum()), int((mask & (yte == k)).sum())] for k, t in enumerate(types)}
                        res[subset] = dict(per_type=per, balanced_accuracy=float(np.mean([v[0] / v[1] for v in per.values() if v[1]])),
                                           confusion={f'{types[t]}->{types[q]}': int(((yte == t) & (pred == q) & mask).sum())
                                                      for t in range(len(types)) for q in range(len(types)) if t != q and ((yte == t) & (pred == q) & mask).sum()})
                    out['runs'].append(dict(train=label, features=name, model=model, seed=seed, train_segments=int(len(tr)), **res))
                rs = out['runs'][-a.seeds:]
                print(f"{label:13} {name:4} {model:8} all={np.mean([r['all']['balanced_accuracy'] for r in rs]):.3f} "
                      f"link_found={np.mean([r['link_found']['balanced_accuracy'] for r in rs]):.3f} "
                      f"not_found={np.mean([r['link_not_found']['balanced_accuracy'] for r in rs]):.3f}", flush=True)
    write_json(a.output, out)


if __name__ == '__main__':
    main()
