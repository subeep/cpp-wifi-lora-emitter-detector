"""Cross-recording test for changing carriers on a frozen four-phase grid."""
import argparse
from collections import defaultdict
import json
from pathlib import Path

import numpy as np
from lightbridge_carrier_holdout import extract


def compare(source, target, offset):
    train = source[:len(source)//2]
    test = target[len(target)//2:]
    ref = np.exp(1j * train[:, 0]).mean(axis=0)
    pilots = np.argsort(-abs(ref))[:64]

    def corrected(rows):
        rotation = np.angle(np.exp(1j * (rows[:, 0, pilots] - np.angle(ref[pilots]))).mean(axis=1))
        return rows[:, 1] - rotation[:, None]

    phases = corrected(train)
    moment = np.exp(4j * phases).mean(axis=0)
    # Favor a fourfold pattern with variation beyond a single or antipodal state.
    rank = abs(moment) - abs(np.exp(2j * phases).mean(axis=0))
    rank[pilots] = -np.inf
    bins = np.argsort(-rank)[:64]
    axis = np.angle(moment[bins]) / 4
    frozen = corrected(test)[:, bins]
    def metrics(values, axes):
        delta = values - axes
        return dict(fourfold_alignment=float(np.cos(4*delta).mean()),
                    within_15_deg=float((np.cos(4*delta) > 0.5).mean()))
    labels = np.rint((frozen-axis)/(np.pi/2)).astype(int) % 4
    return dict(offset=offset, train=len(train), test=len(test),
                scored=metrics(frozen, axis),
                shuffled_axes=metrics(frozen, np.random.default_rng(42).permutation(axis)),
                uncorrected=metrics(test[:, 1, bins], axis),
                carriers_visiting_all_four=int(sum(len(np.unique(col)) == 4 for col in labels.T)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    m = json.loads(parser.parse_args().manifest.read_text())
    if (m["source_sample_rate_hz"], m["sample_rate_hz"]) != (60000000, 44800000):
        raise ValueError("Unexpected sampling grid")
    events = defaultdict(lambda: defaultdict(dict))
    for e in m["entries"]:
        if e["passes_gate"]:
            events[e["path"]][e["source_offset_sample"]][e["offset"]] = e
    for offset in (5, 6, 7):
        arrays = {}
        for path, anchors in events.items():
            rows = [extract(Path(path), a, entries, m, (4, offset))
                    for a, entries in sorted(anchors.items()) if {0, 4, offset} <= entries.keys()]
            if len(rows) >= 8:
                arrays[path] = np.asarray(rows)
        for src, source in arrays.items():
            for dst, target in arrays.items():
                if src != dst:
                    print(json.dumps(dict(source=src, target=dst, **compare(source, target, offset))))


if __name__ == "__main__":
    main()
