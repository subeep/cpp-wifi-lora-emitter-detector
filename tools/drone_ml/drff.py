"""Frozen, publisher-labelled cross-unit experiment; bounded HDF5 IQ reads."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import os
import h5py
import numpy as np

from .common import identity, new_output, read_source, sha256, stamp, write_json
from .preprocess import STFTConfig, select_offset, spectrogram

PAPER = 'https://arxiv.org/html/2603.00106v2'
KNOWN = {'mavicAir2': 'DJI_Air_2', 'mavicAir2s': 'DJI_Air_2S', 'mini4PRO': 'DJI_Mini_4_Pro'}
PATTERN = re.compile(r'(?P<model>mavicAir2s?|mini4PRO|mini3pro|mavic3S)_(?P<unit>[1-8])_hover_c(?P<c>[1-3])_u2_d1\.mat')


def vector(h, key):
    if key not in h or isinstance(h.get(key, getlink=True), (h5py.ExternalLink, h5py.SoftLink)):
        raise ValueError('missing or indirect HDF5 field: ' + key)
    d = h[key]
    if not isinstance(d, h5py.Dataset):
        raise ValueError('HDF5 field must be a dataset')
    if d.is_virtual or d.external:
        raise ValueError('indirect HDF5 storage unsupported')
    return d


def scalar(h, key):
    d = vector(h, key)
    if d.size != 1:
        raise ValueError('metadata scalar expected: ' + key)
    value = float(np.asarray(d[()]).reshape(-1)[0])
    if not np.isfinite(value):
        raise ValueError('nonfinite metadata')
    return value


def text(h, key):
    d = vector(h, key)
    if not 1 <= d.size <= 128 or d.dtype.kind not in 'ui':
        raise ValueError('bounded MATLAB character field expected')
    return ''.join(chr(int(v)) for v in np.asarray(d[()]).reshape(-1) if v)


def metadata(path):
    with read_source(path) as f, h5py.File(f, 'r') as h:
        centers = [scalar(h, k) for k in ('CenterFrequence', 'CenterFreq') if k in h]
        if not centers or len(set(centers)) != 1:
            raise ValueError('missing/conflicting centre metadata')
        i, q = vector(h, 'RF0_I'), vector(h, 'RF0_Q')
        if i.shape != q.shape or len(i.shape) != 2 or min(i.shape) != 1 or i.dtype.kind != 'f' or q.dtype.kind != 'f':
            raise ValueError('unsupported IQ vectors')
        condition = scalar(h, 'C')
        if condition != int(condition) or not 1 <= condition <= 3:
            raise ValueError('invalid hover condition')
        return dict(target=text(h, 'TD'), state=text(h, 'State'), receiver=text(h, 'U'),
                    day=text(h, 'D'), condition=int(condition), rate_hz=scalar(h, 'Fs'),
                    center_hz=centers[0], total_samples=i.size, iq_shape=list(i.shape))


def curate(root, output):
    root = Path(root).resolve()
    folder = root / 'DRFF-R2/DRFF-R2/dataset3-single_drone_hover'
    if not folder.is_dir() or not os.statvfs(folder).f_flag & os.ST_RDONLY:
        raise ValueError('requires mounted read-only SDA DRFF hover source')
    records, groups = [], {}
    for path in sorted(folder.glob('*.mat')):
        match = PATTERN.fullmatch(path.name)
        if not match:
            continue
        model, unit, condition = match['model'], int(match['unit']), int(match['c'])
        if model in KNOWN:
            if unit > 5:
                continue
            label = KNOWN[model]
            partition = 'train' if unit <= 2 else 'calibration' if unit <= 4 else 'test'
        elif model == 'mini3pro':
            label, partition = 'DJI_Mini_3_Pro', 'unknown_calibration'
        else:
            # Publisher token is retained: do not infer Air 3S vs Mavic variants.
            label, partition = 'DRFF_mavic3S_publisher_context', 'unknown_test'
        before = stamp(path)
        m = metadata(path)
        if (m['target'] != model + '_' + str(unit) or m['state'] != 'hover'
                or m['receiver'] != 'u2' or m['day'] != 'd1' or m['condition'] != condition
                or m['rate_hz'] != 100_000_000 or m['center_hz'] != 5_745_000_000
                or m['total_samples'] < 200_000):
            raise ValueError('publisher filename/metadata disagreement: ' + str(path))
        if stamp(path) != before:
            raise ValueError('source changed during review')
        group = 'DRFF-R2:' + model + ':aircraft_unit_' + str(unit)
        groups[group] = dict(partition=partition, label_evidence='filename agrees with embedded TD/State and publisher collection schema',
                             independence_evidence='different publisher-declared aircraft unit; same receiver/day; not independent deployment')
        records.append(dict(recording_id=identity(path), source=str(path), source_stat=before,
            label=label, split_group=group, partition=partition, metadata=m,
            label_status='publisher_label_checked_against_embedded_metadata',
            label_scope='recording_context_not_protocol_or_window_activity_truth',
            independent_deployment_verified=False, release_validation_eligible=False))
    counts = Counter(r['label'] for r in records)
    expected = {v: 15 for v in KNOWN.values()}
    expected.update(DJI_Mini_3_Pro=6, DRFF_mavic3S_publisher_context=3)
    if dict(counts) != expected:
        raise ValueError('frozen experiment requires exactly the specified 54 sources')
    if any(stamp(r['source']) != r['source_stat'] for r in records):
        raise ValueError('source changed before curation completed')
    scripts = [root / 'DRFF-R2/code/signal_collection' / name for name in ('signal_collection1.py', 'signal_collection2.py')]
    out = new_output(output, [root])
    plan = dict(schema='rfmon.drone_ml.split.v1', groups=groups,
                scope='publisher-declared cross-aircraft-unit, same receiver/day, offline exploratory',
                frozen_before_iq_preparation=True)
    write_json(out / 'split_plan.json', plan)
    report = dict(schema='rfmon.drone_ml.drff_review.v1', records=records, paper=PAPER,
        source_script_sha256={str(p): sha256(p) for p in scripts},
        sources=54, groups=len(groups), classes=dict(counts),
        exclusions=['mixtures', 'environment_controls', 'other_receivers_days', 'conflicting_labels', 'units_outside_frozen_selection'],
        full_source_checksums_verified=False, independently_verified_truth=False)
    write_json(out / 'review.json', report)
    return report


def interval(h, start, count):
    if type(start) is not int or type(count) is not int or start < 0 or not 1 <= count <= 4_000_000:
        raise ValueError('invalid or over-budget interval')
    values = []
    for key in ('RF0_I', 'RF0_Q'):
        d = vector(h, key)
        if start + count > d.size:
            raise ValueError('interval exceeds recording')
        values.append(np.asarray(d[start:start + count, 0] if d.shape[1] == 1 else d[0, start:start + count], dtype=np.float64))
    if any(v.size != count or not np.isfinite(v).all() for v in values):
        raise ValueError('short/nonfinite source interval')
    return values[0] + 1j * values[1]


def prepare(review_path, output, windows=12):
    if type(windows) is not int or not 5 <= windows <= 24:
        raise ValueError('bounded preparation requires 5..24 windows per recording')
    review_path = Path(review_path).resolve()
    review = json.loads(review_path.read_text())
    if review.get('schema') != 'rfmon.drone_ml.drff_review.v1' or len(review['records']) != 54:
        raise ValueError('requires frozen DRFF review')
    config = STFTConfig(auto_frequency_view=True)
    images, records, rejected = [], [], []
    out = new_output(output, [Path('/media/sudeep/Storage/datasets'), review_path.parent])
    for r in review['records']:
        path = Path(r['source'])
        if not os.statvfs(path).f_flag & os.ST_RDONLY or stamp(path) != r['source_stat']:
            raise ValueError('source writable or changed since curation')
        m = metadata(path)
        if m != r['metadata']:
            raise ValueError('metadata changed since curation')
        size = round(m['rate_hz'] * config.duration_seconds)
        # Keep a margin from capture boundaries; no adjacent-window random split.
        offsets = np.unique(np.linspace(size, m['total_samples'] - 2 * size, windows, dtype=np.int64))
        with read_source(path) as f, h5py.File(f, 'r') as h:
            for start in offsets:
                iq = interval(h, int(start), size)
                try:
                    image = spectrogram(iq, m['rate_hz'], config)
                except ValueError as e:
                    rejected.append(dict(recording_id=r['recording_id'], offset_samples=int(start), reason=str(e)))
                    continue
                images.append(image)
                records.append(dict(index=len(images) - 1, recording_id=r['recording_id'], source=str(path),
                    original_source_group=r['split_group'], split_group=r['split_group'],
                    label=r['label'], partition='unassigned', source_sha256=hashlib.sha256(iq.astype('<c8').tobytes()).hexdigest(),
                    checksum_scope='selected_interval_not_full_recording', offset_samples=int(start), samples=size,
                    label_status=r['label_status'], independence_verified=False,
                    selected_offset_hz=select_offset(iq, m['rate_hz'], config),
                    source_rate_hz=m['rate_hz'], source_center_hz=m['center_hz']))
        if stamp(path) != r['source_stat']:
            raise ValueError('source changed during preparation')
    if not images or sum(x.size for x in images) > 64_000_000:
        raise ValueError('empty or over-budget corpus')
    if any(stamp(r['source']) != r['source_stat'] for r in review['records']):
        raise ValueError('source changed before preparation completed')
    np.savez_compressed(out / 'spectrograms.npz', images=np.stack(images))
    corpus = dict(schema='rfmon.drone_ml.corpus.v1', records=records, preprocessing=config.metadata(),
        tensor_sha256=sha256(out / 'spectrograms.npz'), origin_manifest_sha256=sha256(review_path),
        origin_split_plan_sha256=sha256(review_path.parent / 'split_plan.json'),
        label_scope='publisher_recording_context_not_verified_window_activity', release_validation_eligible=False,
        rejected=rejected, source_stats_preserved=True)
    write_json(out / 'corpus.json', corpus)
    return corpus


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='action', required=True)
    c = sub.add_parser('curate')
    c.add_argument('--root', type=Path, required=True)
    c.add_argument('--output', type=Path, required=True)
    q = sub.add_parser('prepare')
    q.add_argument('--review', type=Path, required=True)
    q.add_argument('--output', type=Path, required=True)
    q.add_argument('--windows-per-record', type=int, default=12)
    a = p.parse_args()
    result = curate(a.root, a.output) if a.action == 'curate' else prepare(a.review, a.output, a.windows_per_record)
    print(json.dumps(dict(records=len(result['records']), release_validation_eligible=False)))


if __name__ == '__main__':
    main()
