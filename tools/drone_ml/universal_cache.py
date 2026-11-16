"""Training cache for the universal classifier: one spectrogram and one measured-fact vector per
split window. Reads the B1 window store and the B2 feature files; writes one .npz per partition.

Spectrogram: the existing preprocessing (`spectrogram`, 8 ms, nfft 256, hop 256, unit-RMS log
power), pooled 2 x 4 (frequency x time) to 128 x 156 and stored as float16. The pooling keeps the
78 kHz x 51 us resolution needed to see burst shapes while making CPU training practical.
"""
import argparse
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
import json
from pathlib import Path
import numpy as np
from .preprocess import spectrogram, STFTConfig
from .universal_inventory import read_json

VERSION = 'universal_cache_v1_pool2x4'
CONFIG = STFTConfig(duration_seconds=.008, hop=256)


def image(x):
    s = spectrogram(np.asarray(x), 20_000_000, CONFIG)            # (256 freq, 625 time), dB
    s = s[:, :624].reshape(128, 2, 156, 4).mean(axis=(1, 3))
    return s.astype(np.float16)


def one_record(args):
    store, presence, ident, indices = args
    iq = np.load(Path(store) / (ident + '.npy'), mmap_mode='r', allow_pickle=False)
    facts = np.load(Path(presence) / (ident + '.npz'), allow_pickle=False)
    return ident, [(i, image(iq[i]), facts['values'][i], facts['available'][i]) for i in indices]


def build(splits, store, presence, output, workers=8):
    output = Path(output); output.mkdir(parents=True, exist_ok=True)
    rows = read_json(splits)['rows']
    by_record = defaultdict(list)
    for r in rows:
        by_record[r['recording_id']].append(r['view_index'])
    done = {}
    with ProcessPoolExecutor(workers) as pool:
        jobs = [(str(store), str(presence), ident, sorted(set(ix))) for ident, ix in by_record.items()]
        for n, (ident, items) in enumerate(pool.map(one_record, jobs, chunksize=4)):
            for i, img, val, ava in items:
                done[(ident, i)] = (img, val, ava)
            if n % 200 == 0:
                print(json.dumps(dict(records=n, of=len(jobs))), flush=True)
    by_part = defaultdict(list)
    for r in rows:
        by_part[r['partition']].append(r)
    summary = {}
    for part, items in by_part.items():
        img = np.stack([done[(r['recording_id'], r['view_index'])][0] for r in items])
        val = np.stack([done[(r['recording_id'], r['view_index'])][1] for r in items]).astype(np.float32)
        ava = np.stack([done[(r['recording_id'], r['view_index'])][2] for r in items])
        meta = np.array([json.dumps({k: r[k] for k in ('recording_id', 'view_index', 'type', 'model', 'dataset', 'aircraft')})
                         for r in items])
        np.savez(output / (part + '.npz'), spec=img, facts=val, available=ava, meta=meta)
        summary[part] = len(items)
    (output / 'cache.json').write_text(json.dumps(dict(version=VERSION, splits=str(splits), partitions=summary,
                                                       spectrogram=dict(CONFIG.metadata(), pooled_shape=[128, 156])), indent=1))
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('splits', 'store', 'presence', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--workers', type=int, default=8)
    a = p.parse_args()
    print(json.dumps(build(a.splits, a.store, a.presence, a.output, a.workers)))


if __name__ == '__main__':
    main()
