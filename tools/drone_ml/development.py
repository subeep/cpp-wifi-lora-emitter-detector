"""Development-only short/long context corpora; never read final-test IQ."""
import argparse
from collections import Counter
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import h5py
import numpy as np

from .activity import POLICY, assess, rank
from .common import new_output, read_source, sha256, stamp, write_json
from .drff import interval, metadata
from .preprocess import STFTConfig, spectrogram

DEV_PARTITIONS = {'train', 'calibration', 'unknown_calibration'}


def permitted(review):
    if review.get('schema') != 'rfmon.drone_ml.drff_review.v1':
        raise ValueError('requires reviewed DRFF metadata')
    rows = [r for r in review['records'] if r['partition'] in DEV_PARTITIONS]
    if len(rows) != 42:
        raise ValueError('development selection must contain the frozen 42 sources')
    return rows


def prepare(review_path, output, anchors=6):
    if type(anchors) is not int or not 2 <= anchors <= 6:
        raise ValueError('development preparation requires 2..6 anchors')
    review_path = Path(review_path).resolve()
    review = json.loads(review_path.read_text())
    source_plan = json.loads((review_path.parent / 'split_plan.json').read_text())
    rows = permitted(review)
    if any(source_plan['groups'][r['split_group']]['partition'] != r['partition'] for r in rows):
        raise ValueError('review and split disagree')
    out = new_output(output, [Path('/media/sudeep/Storage/datasets'), review_path.parent])
    plan = dict(schema='rfmon.drone_ml.split.v1', development_only=True,
                groups={g: source_plan['groups'][g] for g in sorted({r['split_group'] for r in rows})})
    write_json(out / 'split_plan.json', plan)
    long_cfg = STFTConfig(duration_seconds=.008, hop=256, auto_frequency_view=True)
    short_cfg = replace(long_cfg, duration_seconds=.001, hop=128)
    configurations = {'short_activity': short_cfg, 'long_activity': long_cfg}
    images = {k: [] for k in configurations}
    records = {k: [] for k in configurations}
    diagnostics = []
    for r in rows:
        path = Path(r['source'])
        if not os.statvfs(path).f_flag & os.ST_RDONLY or stamp(path) != r['source_stat']:
            raise ValueError('source writable or changed')
        m = metadata(path)
        if m != r['metadata']:
            raise ValueError('metadata changed')
        count = round(long_cfg.duration_seconds * m['rate_hz'])
        starts = np.unique(np.linspace(count, m['total_samples'] - 4 * count, anchors, dtype=np.int64))
        with read_source(path) as f, h5py.File(f, 'r') as h:
            for anchor in starts:
                candidates = []
                for j in range(3):
                    offset = int(anchor) + j * count
                    iq = interval(h, offset, count)
                    image = spectrogram(iq, m['rate_hz'], long_cfg)
                    activity = assess(image)
                    candidates.append((rank(activity), offset, iq, image, activity))
                # Stable tie break, no class label or prediction participates in selection.
                _, offset, iq, long_image, activity = max(candidates, key=lambda x: x[0])
                short_count = round(short_cfg.duration_seconds * m['rate_hz'])
                short_image = spectrogram(iq[:short_count], m['rate_hz'], short_cfg)
                diagnostics.append(dict(recording_id=r['recording_id'], split_group=r['split_group'],
                    partition=r['partition'], label=r['label'], anchor_samples=int(anchor),
                    candidates=[dict(offset_samples=c[1], activity=c[4]) for c in candidates],
                    selected_offset_samples=offset))
                for name, image, cfg, n in (('short_activity', short_image, short_cfg, short_count),
                                             ('long_activity', long_image, long_cfg, count)):
                    quality = assess(image)
                    images[name].append(image)
                    records[name].append(dict(index=len(images[name]) - 1, recording_id=r['recording_id'],
                        source=str(path), original_source_group=r['split_group'], split_group=r['split_group'],
                        label=r['label'], partition='unassigned', source_sha256=hashlib.sha256(iq[:n].astype('<c8').tobytes()).hexdigest(),
                        checksum_scope='selected_interval_not_full_recording', offset_samples=offset, samples=n,
                        activity=quality, model_eligible=quality['eligible'],
                        source_rate_hz=m['rate_hz'], source_center_hz=m['center_hz']))
        if stamp(path) != r['source_stat']:
            raise ValueError('source changed during development preparation')
        print(path.name, 'prepared', flush=True)
    if any(stamp(r['source']) != r['source_stat'] for r in rows):
        raise ValueError('source changed before preparation completed')
    for name, cfg in configurations.items():
        folder = out / name
        folder.mkdir()
        if sum(x.size for x in images[name]) > 64_000_000:
            raise ValueError('corpus exceeds memory budget')
        np.savez_compressed(folder / 'spectrograms.npz', images=np.stack(images[name]))
        write_json(folder / 'corpus.json', dict(schema='rfmon.drone_ml.corpus.v1', preprocessing=cfg.metadata(),
            records=records[name], activity_policy=POLICY, development_only=True,
            origin_split_plan_sha256=sha256(out / 'split_plan.json'),
            origin_manifest_sha256=sha256(review_path), tensor_sha256=sha256(folder / 'spectrograms.npz'),
            source_stats_preserved=True, release_validation_eligible=False,
            selection_scope='best_structured_rf_of_three_8ms_views_per_anchor_not_drone_truth'))
    summary = {name: dict(rows=len(records[name]), eligible=sum(r['model_eligible'] for r in records[name]),
        eligible_by_class=dict(Counter(r['label'] for r in records[name] if r['model_eligible']))) for name in configurations}
    write_json(out / 'activity_report.json', dict(summary=summary, selection=diagnostics,
        held_test_iq_read=False, source_stats_preserved=True))
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--review', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--anchors', type=int, default=6)
    a = p.parse_args()
    print(json.dumps(prepare(a.review, a.output, a.anchors), indent=2))


if __name__ == '__main__':
    main()
