"""C2 data: site background split by time, and drone views mixed onto real X310 background.

Site background: the 13 Nov no-drone X310 hour (556 clean 0.5 s captures, 62 views each),
already turned into spectrograms + facts by `data/drone_ml/2026-11-13/background_x310/score.py`.
Split by capture start time so neighbouring captures never straddle a boundary much:
  minutes [0, 32)  -> bg_site_train
  minutes [32, 40) -> bg_site_val
  minutes [40, 60] -> bg_site_test   (never used for fitting, epoch choice or thresholds)

Excluded (14 Nov): 2.4 GHz captures from minute EXCLUDE_24_FROM_MIN on. The user reports a Phantom
was present during the "no-drone" hour; a persistent 32.8 kHz (Lightbridge) link appears at
2430/2450/2470 MHz from minute 18 (17-18% of 2.4 GHz views fire, none before minute 18 and none
at 5.8 GHz). Those captures are not background, so they are kept out of every split and mix.

Mixing: a drone view (unit RMS) plus a background view (unit RMS) at a random drone-to-
background power ratio, at the IQ level; spectrogram and facts are recomputed on the mix.
A mixed view keeps its drone label only if the drone is still measurable by the same rule as
B2: OFDM and Lightbridge views need their own timing ratio > 4 after mixing; hopping views
need a ratio of at least 0 dB. Views that fail are dropped (not relabelled: the drone is still
in them). Drone views come from the partition named; background views from the matching time
split only, so test mixes use only test-time background.
"""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from datetime import datetime
import json
from pathlib import Path
import numpy as np
from .universal_cache import image
from .universal_features import extract, NAMES
from .universal_presence import K15, K30, KLB, MODELS_15, MODELS_30, fired

VIEW = 160000
STORE = Path('/media/sudeep/Storage/drone_universal_classifier/2026-11-12/views-v1')   # default; --store (E4: store-e4)
BG_DIR = Path('/media/sudeep/Storage/drone_universal_classifier/background_x310/2026-11-13')
BG_SESSION = BG_DIR / 'nodrone-60min-20261113-093454'
BG_WINDOWS = BG_DIR / 'nodrone-60min-windows.npz'
SPLIT_MIN = {'bg_site_train': (0, 32), 'bg_site_val': (32, 40), 'bg_site_test': (40, 61)}
MIX_FROM = {'train': 'bg_site_train', 'val': 'bg_site_val', 'test': 'bg_site_test'}
SNR_DB = (-6., 12.)
EXCLUDE_24_FROM_MIN = 17.     # a Lightbridge link (user: a Phantom) at 2.4 GHz from minute 18


def expected_timing(m):
    if m['type'] == 'lightbridge':
        return KLB
    if m['dataset'] == 'DroneRFb-DIR' or m['model'] in MODELS_15:
        return K15
    if m['model'] in MODELS_30:
        return K30
    return None


def bg_minutes(meta):
    t = [datetime.fromisoformat(m['host_start']) for m in meta]
    t0 = min(t)
    return np.array([(x - t0).total_seconds() / 60 for x in t])


def site_background(out, windows=BG_WINDOWS):
    d = np.load(windows)
    meta = [json.loads(m) for m in d['meta']]
    minutes = bg_minutes(meta)
    summary = {}
    centre = np.array([m['centre_mhz'] for m in meta])
    excluded = (centre < 3000) & (minutes >= EXCLUDE_24_FROM_MIN)
    for name, (lo, hi) in SPLIT_MIN.items():
        ix = np.flatnonzero((minutes >= lo) & (minutes < hi) & ~excluded)
        m = [dict(type='not_a_drone', model='x310_site', dataset='Own_X310_site', recording_id=meta[i]['file'],
                  view_index=meta[i]['view'], centre_mhz=meta[i]['centre_mhz'], clipped=meta[i]['clipped'],
                  minute=round(float(minutes[i]), 2)) for i in ix]
        np.savez(out / (name + '.npz'), spec=d['spec'][ix], facts=d['facts'][ix], available=d['available'][ix],
                 meta=np.array([json.dumps(x) for x in m]))
        summary[name] = dict(windows=len(ix), captures=len({x['recording_id'] for x in m}),
                             excluded_windows=int(((minutes >= lo) & (minutes < hi) & excluded).sum()))
    return summary


def bg_iq(file, view):
    a = np.fromfile(BG_SESSION / file, '<i2', count=2 * VIEW, offset=4 * VIEW * view).astype(np.float32) / 32768.
    return (a[0::2] + 1j * a[1::2]).astype(np.complex64)


def unit(x):
    return x / max(float(np.sqrt(np.mean(np.abs(x) ** 2))), 1e-12)


def mix_one(job):
    drone_meta, bg_meta, snr_db, store = job        # store travels with the job (workers start fresh)
    d = unit(np.asarray(np.load(Path(store) / (drone_meta['recording_id'] + '.npy'), mmap_mode='r')[drone_meta['view_index']]))
    b = unit(bg_iq(bg_meta['recording_id'], bg_meta['view_index']))
    x = (d * np.float32(10 ** (snr_db / 20)) + b).astype(np.complex64)
    f = extract(x)
    key = expected_timing(drone_meta)
    if key is not None:
        keep = fired(f['lag_ratios'], key)
    else:
        keep = snr_db >= 0
    m = dict(drone_meta, model_weight_key=drone_meta['model'] + '|mix', mixed=True, bg_file=bg_meta['recording_id'],
             bg_view=bg_meta['view_index'], bg_centre_mhz=bg_meta['centre_mhz'], snr_db=round(float(snr_db), 2))
    return keep, image(x), np.asarray(f['values'], np.float32), np.asarray(f['available'], bool), m


def mixed(cache, out, part, count, seed, workers, store=STORE):
    rng = np.random.default_rng(seed)
    d = np.load(Path(cache) / (part + '.npz'))
    meta = [json.loads(m) for m in d['meta']]
    drones = [m for m in meta if m['type'] != 'not_a_drone']
    by_type = defaultdict(lambda: defaultdict(list))
    for m in drones:
        by_type[m['type']][m['model']].append(m)
    bg = [json.loads(m) for m in np.load(out / (MIX_FROM[part] + '.npz'))['meta']]
    jobs = []
    types = sorted(by_type)
    for _ in range(count):                        # balanced: type, then model, then a random view
        models = by_type[types[rng.integers(len(types))]]
        pool = models[sorted(models)[rng.integers(len(models))]]
        jobs.append((pool[rng.integers(len(pool))], bg[rng.integers(len(bg))], rng.uniform(*SNR_DB), str(store)))
    spec, facts, ava, keep_meta, dropped = [], [], [], [], Counter()
    with ProcessPoolExecutor(workers) as ex:
        for n, (keep, s, f, a, m) in enumerate(ex.map(mix_one, jobs, chunksize=16)):
            if keep:
                spec.append(s); facts.append(f); ava.append(a); keep_meta.append(m)
            else:
                dropped[m['type']] += 1
            if n % 2000 == 0:
                print(json.dumps(dict(part=part, done=n, of=count)), flush=True)
    name = 'mix_' + part
    np.savez(out / (name + '.npz'), spec=np.stack(spec), facts=np.stack(facts), available=np.stack(ava),
             meta=np.array([json.dumps(m) for m in keep_meta]))
    return dict(kept=len(keep_meta), dropped=dict(dropped), kept_by_type=dict(Counter(m['type'] for m in keep_meta)))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cache', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--train-mixes', type=int, default=12000); p.add_argument('--eval-mixes', type=int, default=3000)
    p.add_argument('--workers', type=int, default=12); p.add_argument('--seed', type=int, default=7)
    p.add_argument('--store', type=Path, default=STORE, help='B1 view store holding the cache\'s drone views')
    p.add_argument('--bg-windows', type=Path, default=BG_WINDOWS,
                   help='site background views with spectrogram and facts (facts v2: universal_bg_windows)')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    report = dict(version='universal_c2_data_v2', split_minutes=SPLIT_MIN, snr_db=SNR_DB, store=str(a.store),
                  excluded=f'2.4 GHz captures from minute {EXCLUDE_24_FROM_MIN} (Lightbridge link; user: a Phantom was present)',
                  site_background=site_background(a.output, a.bg_windows), bg_windows=str(a.bg_windows))
    for part, n in (('train', a.train_mixes), ('val', a.eval_mixes), ('test', a.eval_mixes)):
        report['mix_' + part] = mixed(a.cache, a.output, part, n, a.seed + len(part), a.workers, a.store)
        print(json.dumps({part: report['mix_' + part]}), flush=True)
    (a.output / 'c2_data.json').write_text(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
