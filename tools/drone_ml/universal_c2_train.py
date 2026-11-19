"""C2: two-level universal classifier (type, then model inside the type), development only.

Open set (default; --closed-set gives the first C2 run): in every batch about a quarter of the
models of each type with 3+ models are hidden; views of a hidden model are trained to give no
visible model a lead (uniform target), so a model never trained on can be refused. Per-model
mean embeddings and a pooled covariance are saved for a distance check in C4.

Changes from C1:
  - training adds the site background (minutes 0-32 of the 13 Nov X310 hour) as not_a_drone
    and drone views mixed onto that background at the IQ level (`universal_c2_data`);
  - a model head, trained only against models of the example's own type;
  - spectrogram augmentation against dataset fingerprints: noise-floor lift, frequency-band
    masks (spur lines), time masks (zero stretches), plus the C1 time/frequency rolls;
  - the family gate, applied after the network and never learned: a type is allowed only when
    its deterministic measurement fired in that view (DJI OFDM: 15 or 30 kHz timing ratio > 4;
    Lightbridge: 32.8 kHz ratio > 4; hopping controller: a burst, duty cycle > 0). not_a_drone
    is always allowed. Inside DJI OFDM, models are limited to those whose timing fired.
Epoch choice uses only val, mix_val and bg_site_val. Scores are reported raw and gated.
Per-view probabilities are saved for threshold setting (C4).

C3 ablations (validation, epoch choice and every test partition stay the same):
  --no-mix         train without the drone views mixed onto X310 background (site background kept);
  --no-facts       the network sees the spectrogram only (the family gate still uses the facts:
                   it is a fixed rule, not learned);
  --train-cache    take the training partition from another cache, e.g. one built without
                   presence filtering (`universal_splits --no-presence-filter`).
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
from .universal_features import NAMES
from .universal_c2_data import expected_timing
from .universal_presence import K15, K30, KLB
from .universal_train import TYPES, Norm, balanced

NAD = TYPES.index('not_a_drone')
IX = {k: NAMES.index('lag_ratio:' + k) for k in (K15, K30, KLB)}
DUTY = NAMES.index('duty_cycle')
HIDE = .25            # share of a type's models hidden in each batch (open-set rotation)
EVAL = ['test', 'unit_test', 'cross_test', 'open_test', 'bg_test', 'bg_site_test', 'mix_test']


class TwoLevel(nn.Module):
    def __init__(self, facts, models, use_facts=True):
        super().__init__()
        self.use_facts = use_facts
        self.cnn = SpectrogramCNN(1).features
        self.mlp = nn.Sequential(nn.Linear(2 * facts, 64), nn.SiLU(), nn.Linear(64, 32), nn.SiLU())
        self.drop = nn.Dropout(.2)
        self.type_head = nn.Linear(96, len(TYPES)); self.model_head = nn.Linear(96, models)

    def forward(self, spec, facts, return_z=False):
        h = self.mlp(facts)
        z = torch.cat([self.cnn(spec), h if self.use_facts else torch.zeros_like(h)], 1)
        d = self.drop(z)
        return (self.type_head(d), self.model_head(d), z) if return_z else (self.type_head(d), self.model_head(d))


def load(root, part):
    d = np.load(Path(root) / (part + '.npz'), allow_pickle=False)
    return dict(spec=d['spec'], facts=d['facts'], available=d['available'], meta=[json.loads(m) for m in d['meta']])


def concat(parts):
    return dict(spec=np.concatenate([p['spec'] for p in parts]), facts=np.concatenate([p['facts'] for p in parts]),
                available=np.concatenate([p['available'] for p in parts]), meta=sum((p['meta'] for p in parts), []))


def gate(facts, timing_of_model, models):
    """Allowed types (n, 4) and allowed models (n, M) from the measured facts only."""
    f = np.nan_to_num(facts)
    fired = {k: f[:, i] > 4 for k, i in IX.items()}
    t = np.zeros((len(f), len(TYPES)), bool)
    t[:, TYPES.index('dji_ofdm')] = fired[K15] | fired[K30]
    t[:, TYPES.index('lightbridge')] = fired[KLB]
    t[:, TYPES.index('hopping_controller')] = f[:, DUTY] > 0
    t[:, NAD] = True
    m = np.ones((len(f), len(models)), bool)
    for j, name in enumerate(models):
        key = timing_of_model.get(name)
        if key in (K15, K30):
            m[:, j] = fired[key]
    return t, m


def weights(meta):
    """Each type equal; inside a type each weight key (model, or model|mix) equal."""
    keys = [m.get('model_weight_key', m['model']) for m in meta]
    per_type = defaultdict(set); count = Counter(keys)
    for m, k in zip(meta, keys):
        per_type[m['type']].add(k)
    return np.array([1 / (len(per_type) * len(per_type[m['type']]) * count[k]) for m, k in zip(meta, keys)])


def augment(s, rng):
    s = s.astype(np.float32)
    for i in range(len(s)):
        if rng.random() < .5:                                  # noise-floor lift
            lin = 10 ** (s[i] / 10)
            s[i] = 10 * np.log10(lin + np.median(lin) * 10 ** (rng.uniform(-20, 0) / 10))
        if rng.random() < .5:                                  # frequency bands (spur lines)
            for _ in range(rng.integers(1, 3)):
                w = rng.integers(1, 7); f0 = rng.integers(0, s.shape[1] - w)
                s[i, f0:f0 + w] = np.median(s[i])
        if rng.random() < .3:                                  # zero stretch
            w = rng.integers(5, 31); t0 = rng.integers(0, s.shape[2] - w)
            s[i, :, t0:t0 + w] = s[i].min()
    return s


class Runner:
    def __init__(self, net, norm, models, timing):
        self.net, self.norm, self.models, self.timing = net, norm, models, timing

    def predict(self, d, batch=512, return_z=False):
        self.net.eval(); tp, mp, zs = [], [], []
        with torch.no_grad():
            for i in range(0, len(d['spec']), batch):
                s, f = self.norm(d['spec'][i:i + batch], d['facts'][i:i + batch], d['available'][i:i + batch])
                a, b, z = self.net(s, f, return_z=True)
                tp.append(torch.softmax(a, 1).numpy()); mp.append(b.numpy()); zs.append(z.numpy())
        tp = np.concatenate(tp) if tp else np.zeros((0, len(TYPES)))
        ml = np.concatenate(mp) if mp else np.zeros((0, len(self.models)))
        allowed_t, allowed_m = gate(d['facts'], self.timing, self.models)
        gt = tp * allowed_t; total = gt.sum(1, keepdims=True)
        # Only not_a_drone allowed and its raw score underflowed to 0: the answer is not_a_drone.
        gt = np.where(total > 0, gt / np.where(total > 0, total, 1), np.eye(len(TYPES))[NAD])
        if return_z:
            return tp, gt, ml, allowed_m, (np.concatenate(zs) if zs else np.zeros((0, 96)))
        return tp, gt, ml, allowed_m


def model_probs(logits, type_index, model_type, allowed):
    """Softmax over the models of one type that the gate allows."""
    mask = np.array([t == type_index for t in model_type])[None] & allowed
    z = np.where(mask, logits, -np.inf)
    z = z - np.where(np.isfinite(z.max(1, keepdims=True)), z.max(1, keepdims=True), 0)
    e = np.where(mask, np.exp(z), 0); s = e.sum(1, keepdims=True)
    return np.divide(e, s, out=np.zeros_like(e), where=s > 0)


def score(meta, tp, gt, ml, allowed_m, models, model_type):
    y = np.array([TYPES.index(m['type']) for m in meta])
    out = dict(windows=len(meta))
    for name, prob in (('raw', tp), ('gated', gt)):
        p = prob.argmax(1)
        out[name] = dict(type_accuracy=float((p == y).mean()), type_balanced=balanced(y, p),
                         predicted=dict(Counter(TYPES[k] for k in p)))
        if (y == NAD).all():
            false = p != NAD
            caps = np.array([m['recording_id'] for m in meta])
            out[name].update(false_rate=float(false.mean()), false_windows=int(false.sum()),
                             captures_with_false=len(set(caps[false])), captures=len(set(caps)))
            if 'centre_mhz' in meta[0]:
                band = np.array([m['centre_mhz'] for m in meta])
                out[name]['false_by_band'] = {int(b): round(float(false[band == b].mean()), 4) for b in sorted(set(band))}
    p = gt.argmax(1); hits1 = hits3 = n = 0; by_model = defaultdict(lambda: [0, 0, 0])
    for i, m in enumerate(meta):
        if m['model'] not in models or p[i] != y[i]:
            continue
        mp = model_probs(ml[i:i + 1], y[i], model_type, allowed_m[i:i + 1])[0]
        order = np.argsort(-mp); target = models.index(m['model'])
        n += 1; hits1 += order[0] == target; hits3 += target in order[:3]
        b = by_model[m['model']]; b[0] += 1; b[1] += order[0] == target; b[2] += target in order[:3]
    if n:
        out['model_given_correct_type'] = dict(windows=n, top1=hits1 / n, top3=hits3 / n,
                                               by_model={k: dict(windows=v[0], top1=v[1] / v[0], top3=v[2] / v[0])
                                                         for k, v in sorted(by_model.items())})
    groups = defaultdict(list)
    for i, m in enumerate(meta):
        groups[m['model']].append(i)
    out['by_model_gated'] = {g: dict(windows=len(ix), correct=float((p[ix] == y[ix]).mean()),
                                     predicted=dict(Counter(TYPES[k] for k in p[ix]))) for g, ix in sorted(groups.items())}
    return out


def class_stats(run, d, models):
    """Per-model mean embedding and the pooled within-model covariance (clean training views)."""
    *_, z = run.predict(d, return_z=True)
    names = [m['model'] for m in d['meta']]
    means, resid = {}, []
    for name in models:
        ix = [i for i, n in enumerate(names) if n == name]
        if ix:
            means[name] = z[ix].mean(0); resid.append(z[ix] - means[name])
    r = np.concatenate(resid)
    cov = r.T @ r / len(r) + 1e-3 * np.eye(r.shape[1])
    return dict(means={k: v.tolist() for k, v in means.items()}, precision=np.linalg.inv(cov).tolist())


def train(cache, c2, output, seed, epochs, threads, open_set=True, mix=True, use_facts=True, train_cache=None):
    torch.manual_seed(seed); rng = np.random.default_rng(seed); torch.set_num_threads(threads)
    train_cache = train_cache or cache
    tr = concat([load(train_cache, 'train'), load(c2, 'bg_site_train')] + ([load(c2, 'mix_train')] if mix else []))
    val = {'val': load(cache, 'val'), 'mix_val': load(c2, 'mix_val'), 'bg_site_val': load(c2, 'bg_site_val')}
    models = sorted({m['model'] for m in tr['meta'] if m['type'] != 'not_a_drone'})
    model_type = [TYPES.index(next(m['type'] for m in tr['meta'] if m['model'] == name)) for name in models]
    timing = {}
    for m in tr['meta']:
        if m['type'] != 'not_a_drone':
            timing.setdefault(m['model'], expected_timing(m))
    norm = Norm(load(train_cache, 'train')['spec'], tr['facts'], tr['available'])
    y = torch.tensor([TYPES.index(m['type']) for m in tr['meta']])
    ym = torch.tensor([models.index(m['model']) if m['type'] != 'not_a_drone' else -1 for m in tr['meta']])
    type_mask = torch.tensor([[model_type[j] == t for j in range(len(models))] for t in range(len(TYPES))])
    rotatable = [np.flatnonzero(np.array(model_type) == t) for t in range(len(TYPES))]
    rotatable = [g for g in rotatable if len(g) >= 3]
    net = TwoLevel(len(NAMES), len(models), use_facts); run = Runner(net, norm, models, timing)
    opt = torch.optim.AdamW(net.parameters(), 2e-3, weight_decay=1e-3)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, 2e-3, total_steps=epochs * 63)
    w = torch.from_numpy(weights(tr['meta'])); best, history = (-1, None, None), []
    for epoch in range(epochs):
        net.train(); start = time.time(); loss_sum = 0
        idx = torch.multinomial(w, 63 * 128, replacement=True)
        for b in range(63):
            j = idx[b * 128:(b + 1) * 128].numpy()
            s, f = norm(augment(tr['spec'][j], rng), tr['facts'][j], tr['available'][j])
            s = torch.roll(s, int(rng.integers(0, s.shape[-1])), dims=-1)
            s = torch.roll(s, int(rng.integers(-4, 5)), dims=-2)
            tl, mlog = net(s, f)
            loss = nn.functional.cross_entropy(tl, y[j], label_smoothing=.05)
            drone = ym[j] >= 0
            if drone.any():
                cand = type_mask[y[j][drone]].clone(); target = ym[j][drone]
                if open_set:                       # hide ~25% of the models of each rotatable type
                    hidden = torch.zeros(len(models), dtype=torch.bool)
                    for group in rotatable:
                        k = max(1, round(len(group) * HIDE))
                        hidden[torch.tensor(rng.choice(group, k, replace=False))] = True
                    cand &= ~hidden
                    unknown = hidden[target]
                else:
                    unknown = torch.zeros(len(target), dtype=torch.bool)
                logp = torch.log_softmax(mlog[drone].masked_fill(~cand, -1e9), 1)
                if (~unknown).any():               # known: its own model among the visible ones
                    loss = loss + .5 * nn.functional.nll_loss(logp[~unknown], target[~unknown])
                if unknown.any():                  # hidden: no visible model should win (uniform target)
                    c = cand[unknown].float()
                    loss = loss + .5 * (-(logp[unknown] * c).sum(1) / c.sum(1)).mean()
            opt.zero_grad(); loss.backward(); opt.step(); sched.step(); loss_sum += float(loss.detach())
        parts = {}
        for name, d in val.items():
            _, gt, _, _ = run.predict(d); yy = np.array([TYPES.index(m['type']) for m in d['meta']])
            parts[name] = float((gt.argmax(1) == NAD).mean()) if name == 'bg_site_val' else balanced(yy, gt.argmax(1))
        sel = float(np.mean(list(parts.values())))
        history.append(dict(epoch=epoch, loss=loss_sum / 63, selection=sel, **parts, seconds=round(time.time() - start, 1)))
        print(json.dumps(history[-1]), flush=True)
        if sel > best[0]:
            best = (sel, epoch, {k: v.clone() for k, v in net.state_dict().items()})
    net.load_state_dict(best[2])
    output = Path(output); output.mkdir(parents=True, exist_ok=True)
    ablation = dict(mix=mix, use_facts=use_facts, train_cache=str(train_cache))
    result = dict(seed=seed, best_epoch=best[1], selection=best[0], models=models, ablation=ablation, history=history, partitions={})
    probs = {}
    for part in EVAL:
        d = load(c2 if part in ('bg_site_test', 'mix_test') else cache, part)
        tp, gt, ml, am = run.predict(d)
        result['partitions'][part] = score(d['meta'], tp, gt, ml, am, models, model_type)
        probs[part + '_type_raw'] = tp; probs[part + '_type_gated'] = gt; probs[part + '_model_logits'] = ml
    torch.save(dict(state_dict=best[2], types=TYPES, models=models, model_type=model_type, timing=timing,
                    norm=norm.state(), open_set=open_set, use_facts=use_facts, ablation=ablation,
                    class_stats=class_stats(run, load(train_cache, 'train'), models),
                    development_only=True), output / f'c2_seed{seed}.pt')
    np.savez_compressed(output / f'c2_seed{seed}_probs.npz', **probs)
    (output / f'c2_seed{seed}.json').write_text(json.dumps(result, indent=1))
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cache', type=Path, required=True); p.add_argument('--c2', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True); p.add_argument('--seed', type=int, action='append')
    p.add_argument('--epochs', type=int, default=20); p.add_argument('--threads', type=int, default=16)
    p.add_argument('--closed-set', action='store_true', help='no hidden-model rotation (the first C2 run)')
    p.add_argument('--no-mix', action='store_true', help='C3: no drone views mixed onto background in training')
    p.add_argument('--no-facts', action='store_true', help='C3: spectrogram only (gate unchanged)')
    p.add_argument('--train-cache', type=Path, help='C3: training partition from this cache')
    a = p.parse_args()
    for seed in a.seed or [1, 2, 3]:
        r = train(a.cache, a.c2, a.output, seed, a.epochs, a.threads, not a.closed_set,
                  not a.no_mix, not a.no_facts, a.train_cache)
        summary = {k: (v['gated'].get('false_rate', v['gated']['type_balanced'])) for k, v in r['partitions'].items()}
        print(json.dumps(dict(seed=seed, best_epoch=r['best_epoch'], gated=summary)), flush=True)


if __name__ == '__main__':
    main()
