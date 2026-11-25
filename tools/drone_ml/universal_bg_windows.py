"""Site background views with the current measured facts (facts v2, 24 Nov 2026).

`universal_c2_data` takes the 13 Nov no-drone X310 hour as spectrograms + facts from one file
(made by data/drone_ml/2026-11-13/background_x310/score.py with facts v1). The spectrograms do
not depend on the facts version, so they are copied; the facts are recomputed from the session's
IQ (same 8 ms views: capture file, view index) with `universal_features.extract`.

  python -m tools.drone_ml.universal_bg_windows --output OUT.npz [--source IN.npz] [--workers 12]
"""
import argparse
from concurrent.futures import ProcessPoolExecutor
import json
from pathlib import Path
import numpy as np
from .universal_c2_data import BG_SESSION, BG_WINDOWS, VIEW
from .universal_features import NAMES, VERSION, extract


def capture_facts(job):
    file, views = job
    a = np.fromfile(BG_SESSION / file, '<i2').astype(np.float32) / 32768.
    x = (a[0::2] + 1j * a[1::2]).astype(np.complex64)
    out = []
    for k in views:
        f = extract(x[k * VIEW:(k + 1) * VIEW])
        out.append((np.asarray(f['values'], np.float32), np.asarray(f['available'], bool)))
    return file, views, out


def build(source, output, workers=12):
    d = np.load(source)
    meta = [json.loads(m) for m in d['meta']]
    rows = {}
    for i, m in enumerate(meta):
        rows.setdefault(m['file'], []).append((m['view'], i))
    facts = np.zeros((len(meta), len(NAMES)), np.float32); available = np.zeros((len(meta), len(NAMES)), bool)
    jobs = [(f, [v for v, _ in r]) for f, r in sorted(rows.items())]
    with ProcessPoolExecutor(workers) as ex:
        for n, (file, views, out) in enumerate(ex.map(capture_facts, jobs)):
            index = dict(rows[file])
            for v, (f, a) in zip(views, out):
                facts[index[v]], available[index[v]] = f, a
            if n % 100 == 0:
                print(json.dumps(dict(captures=n, of=len(jobs))), flush=True)
    old = d['facts']
    # The v1 facts are a prefix of v2: they must come out the same (same IQ, same code).
    same = np.allclose(np.nan_to_num(facts[:, :old.shape[1]]), np.nan_to_num(old), rtol=1e-5, atol=1e-7)
    if not same:
        raise ValueError('recomputed v1 facts differ from the source file')
    np.savez(output, spec=d['spec'], facts=facts, available=available, meta=d['meta'])
    return dict(views=len(meta), captures=len(jobs), facts=VERSION, names=len(NAMES), v1_prefix_matches=bool(same))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True); p.add_argument('--source', type=Path, default=BG_WINDOWS)
    p.add_argument('--workers', type=int, default=12)
    a = p.parse_args()
    print(json.dumps(build(a.source, a.output, a.workers)))


if __name__ == '__main__':
    main()
