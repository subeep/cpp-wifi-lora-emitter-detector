"""C4: decision rule and thresholds for the C2 classifier, set on held-out data only.

Captures. The app classifies 3 disjoint 8 ms intervals per capture: start, middle, end
(`select_intervals` in src/ml_inference.hpp). A capture's type score is the mean of the gated
type probabilities over its 3 intervals (an interval where the family gate blocks the type adds
0). Its model scores are the mean, over the intervals where the gate allows that type, of the
model probabilities inside the type (family mask applied); its embedding is the mean embedding
over the same intervals.

Type thresholds, set on bg_site_val only (minutes 32-40 of the no-drone hour): every capture
there is cut into 21 three-interval pseudo-captures (views i, i+20, i+40); the threshold is the
lowest value above every val score, at least 0.5, at most 0.99.

Model naming (v2). A capture names models only if
  - its top model score >= the score threshold, and
  - its embedding is within the distance threshold of that model's training views
    (Mahalanobis distance, per-model means and pooled covariance saved with the model).
Then it gives a ranked list of up to 3 models of the type; otherwise "unknown <type> model".
Both thresholds are set per signal type (v3), on validation data only:
  - distance: the 95th percentile on val + mix_val captures of aircraft of that type not in
    training; a type with fewer than MIN_BASIS such captures (single-unit families: hopping
    controllers, Lightbridge) uses its clean (unmixed) val captures of known units instead, the
    easiest known case, so the limit is tight. v2 used one pooled limit, set almost entirely by
    unseen DJI aircraft, and it let an unknown controller (Taranis Q X7) through;
  - score: the lowest value at which simulated unknowns of that type (val and mix_val captures
    scored with their own model hidden) are named in at most UNKNOWN_TARGET of captures; a type
    with fewer than MIN_BASIS simulated unknowns uses the pooled value.

Rows. A typed capture is one whose type score passes its threshold. A row opens when a second
typed capture of the same type falls on the same band slot within ROW_WINDOW_S of the first;
typed captures within ROW_MERGE_S extend it. A row names models only if at least ROW_CAPTURES
of its captures name models; the list is ranked by the mean score over those captures.
"""
import argparse
from collections import Counter, defaultdict
from datetime import datetime
import json
from pathlib import Path
import numpy as np
import torch
from .universal_c2_train import EVAL, NAD, Runner, TwoLevel, load, model_probs
from .universal_features import NAMES
from .universal_train import TYPES, Norm

VERSION = 'universal_c4_decision_v3'
APP_VIEWS = (0, 31, 61)            # 62 views of a 0.5 s capture: start, middle, end
PSEUDO = [(i, i + 20, i + 40) for i in range(21)]
ROW_WINDOW_S, ROW_MERGE_S, ROW_CAPTURES = 200., 300., 2
UNKNOWN_TARGET, KNOWN_DISTANCE_Q, TOP, MIN_BASIS = .05, .95, 3, 20
DRONE_TYPES = [t for t in range(len(TYPES)) if t != NAD]


class Views:
    """Per-view network outputs for one partition."""

    def __init__(self, run, d, model_type):
        _, self.gt, self.ml, self.am, self.z = run.predict(d, return_z=True)
        self.model_type, self.meta = model_type, d['meta']
        self.allowed = self.gt > 0
        self.mp = {t: model_probs(self.ml, t, model_type, self.am) for t in DRONE_TYPES}

    def capture(self, ix, hide=None):
        """Type scores (4,), model scores {t: (M,)}, embeddings {t: (96,) or None}."""
        ix = list(ix); ts = self.gt[ix].mean(0); ms, zs = {}, {}
        for t in DRONE_TYPES:
            a = [i for i in ix if self.allowed[i, t]]
            if not a:
                ms[t] = np.zeros(self.ml.shape[1]); zs[t] = None; continue
            if hide is None:
                ms[t] = self.mp[t][a].mean(0)
            else:                                   # simulated unknown: its own model removed
                am = self.am[a].copy(); am[:, hide] = False
                ms[t] = model_probs(self.ml[a], t, self.model_type, am).mean(0)
            zs[t] = self.z[a].mean(0)
        return ts, ms, zs


class Namer:
    def __init__(self, stats, models):
        self.models = models
        self.means = {k: np.array(v) for k, v in stats['means'].items()}
        self.precision = np.array(stats['precision'])

    def distance(self, z, model):
        if z is None or model not in self.means:
            return np.inf
        d = z - self.means[model]
        return float(np.sqrt(d @ self.precision @ d))


def runner(path):
    ck = torch.load(path, weights_only=False)
    net = TwoLevel(len(NAMES), len(ck['models']), ck.get('use_facts', True)); net.load_state_dict(ck['state_dict'])
    return Runner(net, Norm.from_state(ck['norm']), ck['models'], ck['timing']), ck


def site_captures(meta, views):
    """Real background captures: {(file, k): (minute, centre, [row indices of the views])}."""
    by = defaultdict(dict); info = {}
    for i, m in enumerate(meta):
        by[m['recording_id']][m['view_index']] = i
        info[m['recording_id']] = (m['minute'], m['centre_mhz'])
    out = {}
    for f, v in by.items():
        for k, ix in enumerate(views):
            if all(j in v for j in ix):
                out[(f, k)] = (*info[f], [v[j] for j in ix])
    return out


def recording_captures(meta):
    """Dataset views grouped per recording and label into captures of 3 consecutive views."""
    by = defaultdict(list)
    for i, m in enumerate(meta):        # a recording can hold both device bursts and chamber noise
        by[(m['recording_id'], m['model'])].append((m['view_index'], i))
    out = {}
    for r, items in by.items():
        items = [i for _, i in sorted(items)]
        for k in range(0, len(items) - 2, 3):
            out[(r, k // 3)] = items[k:k + 3]
        if len(items) < 3:
            out[(r, 0)] = items
    return out


def type_thresholds(v):
    caps = site_captures(v.meta, PSEUDO)
    scores = np.array([v.capture(ix)[0] for _, _, ix in caps.values()])
    th = {}
    for t in DRONE_TYPES:
        top = float(scores[:, t].max())
        th[TYPES[t]] = dict(threshold=round(min(.99, max(.5, top + .01)), 4), val_max=round(top, 4),
                            val_captures=len(scores), val_over_half=int((scores[:, t] >= .5).sum()))
    return th


def score_for(unknown, dist):
    """Lowest score at which at most UNKNOWN_TARGET of simulated unknowns pass both limits."""
    for s in sorted({round(u[0], 4) for u in unknown} | {0.}):
        if np.mean([u[0] >= s and u[1] <= dist for u in unknown]) <= UNKNOWN_TARGET:
            return s
    return 1.0


def naming_thresholds(vals, namer, train_aircraft):
    """Per type: distance on unseen-aircraft (or clean known) val captures; score on simulated unknowns."""
    known, unknown = defaultdict(list), defaultdict(list)
    for v in vals:
        for ix in recording_captures(v.meta).values():
            truth = v.meta[ix[0]]
            if truth['type'] == 'not_a_drone' or truth['model'] not in namer.models:
                continue
            t = TYPES.index(truth['type'])
            ts, ms, zs = v.capture(ix)
            if ts.argmax() != t:
                continue
            top = namer.models[int(ms[t].argmax())]
            new_aircraft = (truth['model'], truth.get('aircraft')) not in train_aircraft
            known[t].append((float(ms[t].max()), namer.distance(zs[t], top), top == truth['model'], new_aircraft,
                             bool(truth.get('mixed'))))
            _, hs, hz = v.capture(ix, hide=namer.models.index(truth['model']))
            if hs[t].max() > 0:
                unknown[t].append((float(hs[t].max()), namer.distance(hz[t], namer.models[int(hs[t].argmax())])))
    out = {}
    for t in DRONE_TYPES:
        if not known[t]:
            continue
        new = [k for k in known[t] if k[3]]
        basis, why = (new, 'val + mix_val captures of aircraft not in training') if len(new) >= MIN_BASIS else \
                     ([k for k in known[t] if not k[4]] or known[t], 'clean val captures of known units (no unseen aircraft)')
        dist = float(np.quantile([k[1] for k in basis], KNOWN_DISTANCE_Q))
        out[t] = dict(dist=dist, basis=basis, why=why)
    pooled_unknown = [u for t in out for u in unknown[t] if u[1] <= out[t]['dist']]
    pooled = score_for(pooled_unknown, np.inf) if pooled_unknown else 1.0
    by_type = {}
    for t, o in out.items():
        u = unknown[t]
        score, score_basis = (score_for(u, o['dist']), 'simulated unknowns of this type') if len(u) >= MIN_BASIS else \
                             (pooled, 'pooled (too few simulated unknowns)')
        ok = [k for k in o['basis'] if k[0] >= score and k[1] <= o['dist']]
        by_type[TYPES[t]] = dict(
            score_threshold=round(score, 4), distance_threshold=round(o['dist'], 3), distance_basis=o['why'],
            score_basis=score_basis, basis_captures=len(o['basis']), simulated_unknown_captures=len(u),
            simulated_unknown_named=round(float(np.mean([x[0] >= score and x[1] <= o['dist'] for x in u])), 4) if u else None,
            basis_named=round(len(ok) / len(o['basis']), 4),
            basis_top1_right_when_named=round(float(np.mean([k[2] for k in ok])), 4) if ok else None)
    return dict(unknown_target=UNKNOWN_TARGET, distance_quantile=KNOWN_DISTANCE_Q, by_type=by_type)


def limits(naming, type_name):
    """Naming limits for one type; a flat dict (v2 format) applies to every type."""
    return naming['by_type'].get(type_name) if 'by_type' in naming else naming


def decide(cap, th, naming, namer):
    """(type or None, ranked model list or None, top score) for one capture."""
    ts, ms, zs = cap
    t = int(ts.argmax())
    if t == NAD or ts[t] < th[TYPES[t]]['threshold']:
        return None, None, 0.
    order = [j for j in np.argsort(-ms[t]) if ms[t][j] > 0][:TOP]
    if not order:
        return TYPES[t], None, 0.
    top, lim = namer.models[order[0]], limits(naming, TYPES[t])
    ok = lim is not None and ms[t][order[0]] >= lim['score_threshold'] and namer.distance(zs[t], top) <= lim['distance_threshold']
    return TYPES[t], ([namer.models[j] for j in order] if ok else None), float(ms[t][order[0]])


def explain(cap, th, naming, namer):
    """The decide() answer for one capture, with the numbers behind it (for the live worker)."""
    ts, ms, zs = cap
    t = int(ts.argmax())
    out = dict(type=None, type_score=round(float(ts[t]), 4), best_type=TYPES[t], models=[], named=False,
               reason='', distance=None, score_limit=None, distance_limit=None)
    if t == NAD:
        out['reason'] = 'not_a_drone scores highest'
        return out
    limit = th[TYPES[t]]['threshold']
    out['type_threshold'] = limit
    if ts[t] < limit:
        out['reason'] = 'type score below its background threshold'
        return out
    kind, names, _ = decide(cap, th, naming, namer)
    out['type'] = kind
    order = [j for j in np.argsort(-ms[t]) if ms[t][j] > 0][:TOP]
    out['models'] = [dict(name=namer.models[j], score=round(float(ms[t][j]), 4)) for j in order]
    lim = limits(naming, TYPES[t])
    if not order or lim is None:
        out['reason'] = 'no model of this type allowed by the measured timing' if not order else 'no naming limits for this type'
        return out
    d = namer.distance(zs[t], namer.models[order[0]])
    out.update(distance=None if not np.isfinite(d) else round(d, 3), score_limit=lim['score_threshold'],
               distance_limit=lim['distance_threshold'], named=names is not None)
    out['reason'] = 'named' if names is not None else (
        'top score below the naming limit' if ms[t][order[0]] < lim['score_threshold'] else
        'too far from the training views of that model (unknown model or unit)')
    return out


def background_rows(v, th, naming, namer):
    caps = site_captures(v.meta, [APP_VIEWS])
    events = defaultdict(list)            # (slot, type) -> [(seconds, model list)]
    for _, (minute, centre, ix) in caps.items():
        t, names, _ = decide(v.capture(ix), th, naming, namer)
        if t:
            events[(centre, t)].append((minute * 60, names))
    rows = []
    for (centre, t), ev in events.items():
        ev.sort(key=lambda e: e[0]); open_row = None
        for k, (s, names) in enumerate(ev):
            if open_row and s - open_row['last'] <= ROW_MERGE_S:
                open_row['last'] = s; open_row['names'].append(names); continue
            if open_row:
                rows.append(open_row); open_row = None
            if k + 1 < len(ev) and ev[k + 1][0] - s <= ROW_WINDOW_S:
                open_row = dict(centre_mhz=centre, type=t, first=s, last=s, names=[names])
        if open_row:
            rows.append(open_row)
    for r in rows:
        r['named'] = sum(n is not None for n in r['names']) >= ROW_CAPTURES
    typed = sum(len(e) for e in events.values())
    minutes = [m['minute'] for m in v.meta]
    hours = (max(minutes) - min(minutes)) / 60
    named = sum(r['named'] for r in rows)
    return dict(captures=len(caps), typed_captures=typed,
                typed_by_type=dict(Counter(t for (_, t), e in events.items() for _ in e)),
                named_captures=sum(n is not None for e in events.values() for _, n in e),
                rows=len(rows), named_rows=named, hours=round(hours, 3),
                rows_per_hour=round(len(rows) / hours, 2), named_rows_per_hour=round(named / hours, 2), row_list=rows)


def drone_side(v, th, naming, namer, open_set):
    per_rec = defaultdict(list)
    for (rec, _), ix in recording_captures(v.meta).items():
        per_rec[rec].append((*decide(v.capture(ix), th, naming, namer)[:2], v.meta[ix[0]]))
    out = defaultdict(Counter); noise = Counter()
    for rec, items in per_rec.items():
        truth = items[0][2]
        if truth['type'] == 'not_a_drone':
            noise['captures'] += len(items); noise['typed'] += sum(t is not None for t, _, _ in items)
            noise['named'] += sum(n is not None for _, n, _ in items)
            continue
        c = out[truth['model']]; c['recordings'] += 1; c['captures'] += len(items)
        c['captures_right_type'] += sum(t == truth['type'] for t, _, _ in items)
        named = [n for t, n, _ in items if n]
        c['captures_named'] += len(named)
        c['captures_top1_right'] += sum(n[0] == truth['model'] for n in named)
        c['captures_top3_right'] += sum(truth['model'] in n for n in named)
        c['recordings_typed'] += sum(t == truth['type'] for t, _, _ in items) >= min(ROW_CAPTURES, len(items))
        if len(named) >= min(ROW_CAPTURES, len(items)) and named:
            c['recordings_named'] += 1
            rank = Counter(m for n in named for m in n[:1]).most_common(1)[0][0]
            c['recordings_top1_right'] += rank == truth['model']
    tot = Counter()
    for c in out.values():
        tot.update(c)
    n = max(tot['captures'], 1); nn = max(tot['captures_named'], 1)
    s = dict(captures=tot['captures'], capture_type_rate=tot['captures_right_type'] / n,
             recordings=tot['recordings'], recording_type_rate=tot['recordings_typed'] / max(tot['recordings'], 1),
             capture_named_rate=tot['captures_named'] / n)
    if open_set:
        s['unknown_named_as_known_captures'] = tot['captures_named'] / n
        s['unknown_named_as_known_recordings'] = tot['recordings_named'] / max(tot['recordings'], 1)
    else:
        s['top1_right_when_named'] = tot['captures_top1_right'] / nn if tot['captures_named'] else None
        s['top3_right_when_named'] = tot['captures_top3_right'] / nn if tot['captures_named'] else None
        s['top1_right_all_captures'] = tot['captures_top1_right'] / n
    if noise['captures']:
        s['dataset_noise'] = dict(noise)
    s['by_model'] = {k: dict(c) for k, c in sorted(out.items())}
    return s


def run_seed(path, cache, c2):
    run, ck = runner(path)
    models, model_type = ck['models'], ck['model_type']
    namer = Namer(ck['class_stats'], models)
    train_aircraft = {(m['model'], m.get('aircraft')) for m in load(cache, 'train')['meta']}
    v = {}
    for part in ['val', 'mix_val', 'bg_site_val'] + [p for p in EVAL if p != 'bg_test']:
        v[part] = Views(run, load(c2 if part.startswith(('bg_site', 'mix_')) else cache, part), model_type)
    th = type_thresholds(v['bg_site_val'])
    naming = naming_thresholds([v['val'], v['mix_val']], namer, train_aircraft)
    decision = dict(version=VERSION, model_file=Path(path).name, types=TYPES, models=models,
                    intervals_per_capture=len(APP_VIEWS), type_thresholds=th, naming=naming, list_length=TOP,
                    row_window_s=ROW_WINDOW_S, row_merge_s=ROW_MERGE_S, row_captures_for_name=ROW_CAPTURES,
                    set_on=dict(type='bg_site_val (X310 no-drone 2026-11-13, minutes 32-40)', naming='val + mix_val'),
                    created=datetime.now().astimezone().isoformat(), development_only=True)
    Path(path).with_name(Path(path).stem + '_decision.json').write_text(json.dumps(decision, indent=1))
    result = dict(decision=decision, background=background_rows(v['bg_site_test'], th, naming, namer))
    for part in ('test', 'unit_test', 'cross_test', 'open_test', 'mix_test'):
        result[part] = drone_side(v[part], th, naming, namer, part == 'open_test')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cache', type=Path, required=True); p.add_argument('--c2', type=Path, required=True)
    p.add_argument('--models', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); torch.set_num_threads(16)
    report = {}
    for path in sorted(a.models.glob('c2_seed?.pt')):
        r = run_seed(path, a.cache, a.c2); report[path.stem] = r
        bg = r['background']
        print(json.dumps(dict(seed=path.stem, naming={t: {k: n[k] for k in ('score_threshold', 'distance_threshold', 'simulated_unknown_named', 'basis_named')}
                                                      for t, n in r['decision']['naming']['by_type'].items()},
                              bg=f"rows {bg['rows']} named {bg['named_rows']} in {bg['hours']} h, typed {bg['typed_captures']}/{bg['captures']}",
                              **{k: {kk: (round(vv, 3) if isinstance(vv, float) else vv) for kk, vv in r[k].items() if kk != 'by_model'}
                                 for k in ('test', 'unit_test', 'cross_test', 'open_test', 'mix_test')})), flush=True)
    a.output.write_text(json.dumps(report, indent=1, default=str))


if __name__ == '__main__':
    main()
