"""C1: type-only baseline of the universal classifier (development only; no deployable package).

Input per window: the cached 128 x 156 spectrogram and the 29 measured facts (log-compressed,
standardised on training data, unavailable values zeroed and flagged). Output: signal type.
Balanced sampling by type, then model. Model selection on the validation partition only.
Every held-out partition is scored once at the end, overall, per model and per source dataset.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import time
import numpy as np
import torch
from torch import nn
from .model import SpectrogramCNN

TYPES = ['dji_ofdm', 'lightbridge', 'hopping_controller', 'not_a_drone']
HELD_OUT = ['test', 'unit_test', 'cross_test', 'open_test', 'bg_test']


class TypeNet(nn.Module):
    def __init__(self, facts, classes=len(TYPES), use_spec=True, use_facts=True):
        super().__init__()
        self.use_spec, self.use_facts = use_spec, use_facts
        self.cnn = SpectrogramCNN(1).features if use_spec else None
        self.mlp = nn.Sequential(nn.Linear(2 * facts, 64), nn.SiLU(), nn.Linear(64, 32), nn.SiLU()) if use_facts else None
        self.head = nn.Sequential(nn.Dropout(.2), nn.Linear(64 * use_spec + 32 * use_facts, classes))

    def forward(self, spec, facts):
        parts = []
        if self.use_spec: parts.append(self.cnn(spec))
        if self.use_facts: parts.append(self.mlp(facts))
        return self.head(torch.cat(parts, 1))


def load(cache, part):
    d = np.load(Path(cache) / (part + '.npz'), allow_pickle=False)
    meta = [json.loads(m) for m in d['meta']]
    return d['spec'], d['facts'], d['available'], meta


class Norm:
    """Input scaling. Facts whose largest training value is below 1e-2 are scaled by 1e6 before
    log compression. The mask is fixed from the training set (`scale_mask`); before 14 Nov it was
    re-derived from each batch, which made a view's input depend on its batch mates (a single live
    view could be scaled differently). Saved norms without a mask keep the old per-batch rule."""
    def __init__(self, spec, facts, available):
        self.spec_mean, self.spec_std = float(spec.astype(np.float32).mean()), float(spec.astype(np.float32).std())
        self.scale_mask = np.abs(np.nan_to_num(facts)).max(0) < 1e-2
        f = self.compress(facts, self.scale_mask)
        f = np.where(available, f, np.nan)
        self.mean, self.std = np.nan_to_num(np.nanmean(f, 0)), np.nan_to_num(np.nanstd(f, 0)) + 1e-6

    @staticmethod
    def compress(facts, mask=None):
        small = np.abs(facts).max(0) < 1e-2 if mask is None else mask
        return np.sign(facts) * np.log1p(np.abs(facts) * np.where(small, 1e6, 1))

    def __call__(self, spec, facts, available):
        s = (spec.astype(np.float32) - self.spec_mean) / self.spec_std
        f = np.where(available, (self.compress(facts, getattr(self, 'scale_mask', None)) - self.mean) / self.std, 0)
        return torch.from_numpy(s[:, None]), torch.from_numpy(np.concatenate([f, available], 1).astype(np.float32))

    def state(self):
        out = dict(spec_mean=self.spec_mean, spec_std=self.spec_std, mean=self.mean.tolist(), std=self.std.tolist())
        if getattr(self, 'scale_mask', None) is not None:
            out['scale_mask'] = np.asarray(self.scale_mask, bool).tolist()
        return out

    @classmethod
    def from_state(cls, n):
        norm = cls.__new__(cls)
        norm.spec_mean, norm.spec_std = n['spec_mean'], n['spec_std']
        norm.mean, norm.std = np.array(n['mean'], np.float32), np.array(n['std'], np.float32)
        norm.scale_mask = np.array(n['scale_mask'], bool) if 'scale_mask' in n else None
        return norm


def weights(meta):
    """Each type equal, each model equal within its type."""
    per_type = defaultdict(set); count = Counter()
    for m in meta:
        per_type[m['type']].add(m['model']); count[m['model']] += 1
    return np.array([1 / (len(per_type) * len(per_type[m['type']]) * count[m['model']]) for m in meta])


def predict(net, norm, spec, facts, available, batch=512):
    net.eval(); out = []
    with torch.no_grad():
        for i in range(0, len(spec), batch):
            s, f = norm(spec[i:i + batch], facts[i:i + batch], available[i:i + batch])
            out.append(torch.softmax(net(s, f), 1).numpy())
    return np.concatenate(out) if out else np.zeros((0, len(TYPES)))


def balanced(y, p):
    classes = sorted(set(y))
    return float(np.mean([np.mean(p[y == c] == c) for c in classes])) if classes else None


def score(meta, prob):
    y = np.array([TYPES.index(m['type']) for m in meta]); p = prob.argmax(1)
    out = dict(windows=len(meta), type_accuracy=float((p == y).mean()) if len(y) else None, type_balanced=balanced(y, p),
               confusion={TYPES[t]: dict(Counter(TYPES[k] for k in p[y == t])) for t in sorted(set(y))})
    for key in ('model', 'dataset'):
        groups = defaultdict(list)
        for i, m in enumerate(meta):
            groups[m[key]].append(i)
        out['by_' + key] = {g: dict(windows=len(ix), correct=float((p[ix] == y[ix]).mean()),
                                    predicted=dict(Counter(TYPES[k] for k in p[ix])))
                            for g, ix in sorted(groups.items())}
    return out


def train(cache, output, seed, epochs=14, use_spec=True, use_facts=True, threads=16):
    torch.manual_seed(seed); np.random.seed(seed); torch.set_num_threads(threads)
    spec, facts, ava, meta = load(cache, 'train')
    vspec, vfacts, vava, vmeta = load(cache, 'val')
    norm = Norm(spec, facts, ava)
    y = torch.tensor([TYPES.index(m['type']) for m in meta])
    vy = np.array([TYPES.index(m['type']) for m in vmeta])
    net = TypeNet(facts.shape[1], use_spec=use_spec, use_facts=use_facts)
    opt = torch.optim.AdamW(net.parameters(), 2e-3, weight_decay=1e-3)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, 2e-3, total_steps=epochs * 63)
    w = torch.from_numpy(weights(meta))
    best, history = (-1, None, None), []
    for epoch in range(epochs):
        net.train(); start = time.time(); loss_sum = 0
        idx = torch.multinomial(w, 63 * 128, replacement=True)
        for b in range(63):
            j = idx[b * 128:(b + 1) * 128].numpy()
            s, f = norm(spec[j], facts[j], ava[j])
            s = torch.roll(s, int(np.random.randint(0, s.shape[-1])), dims=-1)          # time position is arbitrary
            s = torch.roll(s, int(np.random.randint(-4, 5)), dims=-2)                   # small frequency shift
            loss = nn.functional.cross_entropy(net(s, f), y[j], label_smoothing=.05)
            opt.zero_grad(); loss.backward(); opt.step(); sched.step(); loss_sum += float(loss)
        vb = balanced(vy, predict(net, norm, vspec, vfacts, vava).argmax(1))
        history.append(dict(epoch=epoch, loss=loss_sum / 63, val_balanced=vb, seconds=round(time.time() - start, 1)))
        print(json.dumps(history[-1]), flush=True)
        if vb > best[0]:
            best = (vb, epoch, {k: v.clone() for k, v in net.state_dict().items()})
    net.load_state_dict(best[2])
    result = dict(seed=seed, use_spec=use_spec, use_facts=use_facts, best_epoch=best[1], val_balanced=best[0], history=history,
                  partitions={part: score(load(cache, part)[3], predict(net, norm, *load(cache, part)[:3])) for part in HELD_OUT})
    output = Path(output); output.mkdir(parents=True, exist_ok=True)
    name = f"{'spec' if use_spec else ''}{'facts' if use_facts else ''}_seed{seed}"
    torch.save(dict(state_dict=best[2], types=TYPES, norm=norm.state(), development_only=True), output / (name + '.pt'))
    (output / (name + '.json')).write_text(json.dumps(result, indent=1))
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cache', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--seed', type=int, action='append'); p.add_argument('--epochs', type=int, default=14)
    p.add_argument('--no-spec', action='store_true'); p.add_argument('--no-facts', action='store_true')
    p.add_argument('--threads', type=int, default=16)
    a = p.parse_args()
    for seed in a.seed or [1, 2, 3]:
        r = train(a.cache, a.output, seed, a.epochs, not a.no_spec, not a.no_facts, a.threads)
        print(json.dumps(dict(seed=seed, val=r['val_balanced'], **{k: v['type_balanced'] for k, v in r['partitions'].items()})), flush=True)


if __name__ == '__main__':
    main()
