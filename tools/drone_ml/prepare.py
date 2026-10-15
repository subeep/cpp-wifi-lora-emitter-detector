"""Bounded development smoke corpus from existing read-only derived IQ."""
import argparse
import json
from pathlib import Path
import numpy as np

from .common import identity, new_output, read_source, sha256, stamp, write_json
from .preprocess import STFTConfig, spectrogram

# Candidate contexts, deliberately not verified signal/model ground truth.
CONTEXTS = {
    'mini2vid.zip': ('DJI_Mini_2', 'video_context'),
    'mini2RC.zip': ('DJI_Mini_2', 'controller_context'),
    'matricevid.zip': ('DJI_Matrice_300_context', 'video_context'),
    'inspire2Vid.zip': ('DJI_Inspire_2', 'video_context'),
    'DJI_inspire_2_2G.bin': ('DJI_Inspire_2', 'combined_context'),
    'DJI_mavic_pro_2G.bin': ('DJI_Mavic_Pro', 'combined_context'),
    'Parrot_disco_2G.bin': ('Parrot_Disco', 'combined_context'),
    'Parrot_mambo_control_2G.bin': ('Parrot_Mambo', 'controller_context'),
}


def prepare(manifest, output, windows=24, config=STFTConfig()):
    if type(windows) is not int or not 1 <= windows <= 128:
        raise ValueError('windows per recording must be 1..128')
    manifest = Path(manifest).resolve()
    before_manifest = stamp(manifest)
    with read_source(manifest) as f:
        original = json.load(f)
    if original.get('schema') != 'local_link_references.v1' or len(original['records']) > 32:
        raise ValueError('requires the bounded local Leuven/Tampere development manifest')
    sources = [Path(r['path']).resolve() for r in original['records']]
    roots = [Path('/media/sudeep/Storage/datasets'), manifest.parent, *sources]
    out = new_output(output, roots)
    images, entries, skipped, preserved = [], [], [], []
    for record in original['records']:
        name = Path(record['source']).name
        if name not in CONTEXTS:
            raise ValueError('unreviewed reference context')
        if record.get('partition') != 'development_only':
            raise ValueError('cannot silently relabel held-out reference')
        path = Path(record['path']).resolve()
        before = stamp(path)
        if record['format'] not in ('cf32_le', 'ci16_le'):
            raise ValueError('unsupported declared IQ layout')
        width = 8 if record['format'] == 'cf32_le' else 4
        if before[2] != record['samples'] * width or sha256(path) != record['sha256']:
            raise ValueError('derived IQ size/hash mismatch')
        rate = record['sample_rate_hz']
        size = round(rate * config.duration_seconds)
        available = before[2] // width
        if size > available:
            raise ValueError('recording shorter than STFT interval')
        offsets = np.unique(np.linspace(0, available - size, windows, dtype=np.int64))
        model, role = CONTEXTS[name]
        # RC/video and all continuations of the same model/collection stay together.
        dataset = Path(record['source']).parent.name
        group = dataset + ':' + model + ':unit_session_unresolved'
        with read_source(path) as f:
            for start in offsets:
                f.seek(int(start) * width)
                payload = f.read(size * width)
                if len(payload) != size * width:
                    raise ValueError('short IQ read')
                if width == 8:
                    iq = np.frombuffer(payload, dtype='<c8')
                else:
                    pairs = np.frombuffer(payload, dtype='<i2').reshape(-1, 2).astype(np.float32)
                    iq = pairs[:, 0] + 1j * pairs[:, 1]
                try:
                    image = spectrogram(iq, rate, config)
                except ValueError as e:
                    skipped.append(dict(source=str(path), offset_samples=int(start), reason=str(e)))
                    continue
                images.append(image)
                if sum(x.size for x in images) > 64_000_000:
                    raise ValueError('prepared tensor exceeds memory budget')
                entries.append(dict(index=len(images) - 1, recording_id=identity(record['source'], record.get('archive_member')),
                    source=str(path), original_source=record['source'], original_member=record.get('archive_member'),
                    original_source_group=record['split_group'], split_group=group,
                    label=model, role=role, label_status='publisher_recording_context_not_window_truth',
                    partition='development_only', independence_verified=False,
                    source_rate_hz=rate, source_center_hz=record['capture_center_hz'],
                    offset_samples=int(start), original_offset_samples=record.get('source_sample_offset', 0) + int(start),
                    samples=size, source_sha256=record['sha256']))
        if stamp(path) != before:
            raise RuntimeError('source changed during preparation')
        preserved.append((path, before))
    if not images:
        raise ValueError('no eligible finite signal windows')
    if stamp(manifest) != before_manifest or any(stamp(p) != s for p, s in preserved):
        raise RuntimeError('source changed before preparation completed')
    np.savez_compressed(out / 'spectrograms.npz', images=np.stack(images))
    report = dict(schema='rfmon.drone_ml.corpus.v1', preprocessing=config.metadata(),
                  origin_manifest=str(manifest), origin_manifest_sha256=sha256(manifest),
                  tensor_sha256=sha256(out / 'spectrograms.npz'), records=entries, skipped=skipped,
                  source_stats_preserved=True, release_validation_eligible=False,
                  label_scope='weak_recording_context_for_development_smoke_only')
    write_json(out / 'corpus.json', report)
    groups = sorted({r['split_group'] for r in entries})
    write_json(out / 'split_template.json', dict(schema='rfmon.drone_ml.split.v1',
        groups={g: dict(partition='excluded', label_evidence=None, independence_evidence=None) for g in groups}))
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--references', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--windows-per-record', type=int, default=24)
    p.add_argument('--offset-hz', type=float, default=0)
    a = p.parse_args()
    report = prepare(a.references, a.output, a.windows_per_record, STFTConfig(offset_hz=a.offset_hz))
    print(json.dumps(dict(windows=len(report['records']), groups=len({r['split_group'] for r in report['records']}),
                         skipped=len(report['skipped']), release_validation_eligible=False)))


if __name__ == '__main__':
    main()
