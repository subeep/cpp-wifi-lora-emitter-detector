"""Paired, frozen four-state test on +6/0 and +6/+5 carrier phases."""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy.special import i0, logsumexp

from tools.lightbridge_carrier_holdout import extract
from tools.lightbridge_third_phase import timing_for


def evaluate(train, test):
    ref = np.exp(1j * np.angle(train[:, 0])).mean(axis=0)
    pilots = np.argsort(-abs(ref))[:64]

    def observations(rows):
        rotation = np.angle((np.exp(1j * np.angle(rows[:, 0, pilots]))
                             * np.exp(-1j * np.angle(ref[pilots]))).mean(axis=1))
        baseline = np.angle(rows[:, 2]) - rotation[:, None]
        adjacent = np.angle(rows[:, 2] * rows[:, 1].conj())
        return baseline, adjacent

    baseline, differential = observations(train)
    rank = abs(np.exp(4j * baseline).mean(axis=0)) - abs(np.exp(2j * baseline).mean(axis=0))
    rank[pilots] = -np.inf
    bins = np.argsort(-rank)[:64]
    scores = {}
    arrays = dict(selected_indices=bins, reference_indices=pilots, cross_spectra=test)
    for name, tr, te in zip(("relative_to_zero", "relative_to_five"),
                            (baseline, differential), observations(test)):
        axis = np.angle(np.exp(4j * tr[:, bins]).mean(axis=0)) / 4
        def logits(axes):
            return 4 * np.cos(te[:, bins, None] - axes[:, None] - np.arange(4)*np.pi/2)
        values = logits(axis)
        null = logits(np.random.default_rng(42).permutation(axis))
        def per_burst(v):
            return (logsumexp(v,axis=2) - np.log(4*i0(4))).mean(axis=1)
        observed, shuffled = per_burst(values), per_burst(null)
        scores[name] = dict(mean=float(observed.mean()), shuffled=float(shuffled.mean()),
                            above_shuffle=int(sum(observed > shuffled)), per_burst=observed.tolist())
        arrays[name+"_axis"] = axis
        arrays[name+"_relative_logits"] = values - values.max(axis=2, keepdims=True)
    return scores, arrays


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("manifest", "hits", "output"):
        parser.add_argument(arg, type=Path)
    args = parser.parse_args()
    m = json.loads(args.manifest.read_text())
    entries = {}
    for e in m["entries"]:
        if e["path"].endswith("PHA_0000_01.dat") and e["passes_gate"]:
            source = Path(e["path"])
            entries.setdefault(e["source_offset_sample"], {})[e["offset"]] = e
    # Preserve the original training split, then require +5 timing too.
    ids = sorted(a for a, v in entries.items() if {0,4,6} <= v.keys())
    ids = [a for a in ids[:len(ids)//2] if 5 in entries[a]]
    def read(path, a, v):
        return extract(path, a, v, m, (4,5,6), complex_grid=True)
    train = np.array([read(source, a, entries[a]) for a in ids])
    bins = np.array(m["carrier_bins"])
    for row in map(json.loads, args.hits.read_text().splitlines()):
        path = Path(row["path"])
        anchors, rows = [], []
        for hit in row["hits"]:
            a = hit["sample"]
            if path.stem == "PHA_1000_01" and 50772032 <= a <= 51972032:
                continue
            timing = timing_for(path, a, bins)
            if {0,4,5,6} <= timing.keys():
                anchors.append(a)
                rows.append(read(path, a, timing))
        scores = {}
        if rows:
            scores, arrays = evaluate(train, np.array(rows))
            np.savez_compressed(args.output / (path.stem+".npz"), anchors=anchors,
                                train_anchors=ids, carrier_bins=bins, **arrays)
        print(json.dumps(dict(path=str(path), acquired=len(row["hits"]), eligible=len(rows),
                              train=ids, scores=scores)))


if __name__ == "__main__":
    main()
