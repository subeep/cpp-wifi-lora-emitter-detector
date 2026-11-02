"""Read-only Mini 2 integrity/shape audit. Does not export training labels or IQ."""
import argparse
import hashlib
import io
from pathlib import Path
import zipfile

import h5py

from .common import new_output, read_source, sha256, stamp, write_json
from .recognition_review import load_json, validate_frozen_review


PUBLISHER_MD5 = '2041fdaff63169879db6805835801fad'
PUBLISHER_URL = 'https://rdr.kuleuven.be/file.xhtml?fileId=154424&version=1.0'


def selected_members(review, plan):
    validate_frozen_review(review, plan)
    selected = [r for r in review['records'] if r['dataset'] == 'KULeuven_DroneRF'
                and r['label'] == 'DJI_Mini_2']
    if not selected or len({r['source'] for r in selected}) != 1:
        raise ValueError('one reviewed Mini 2 archive required')
    source = selected[0]['source']
    for r in review['records']:
        if r['source'] == source and r['partition'] != 'train':
            raise ValueError('archive contains a reserved reviewed source')
    for r in selected:
        if (Path(source).name != 'mini2vid.zip' or r['partition'] != 'train'
                or r['adapter'] != 'leuven' or r['rate_hz'] != 100_000_000
                or r['center_hz'] != 2_440_000_000
                or r['role'] != 'video_or_combined_context'
                or not r.get('member', '').startswith('mini2vid/mini2_')
                or not 0 < r['member_bytes'] <= 128 * 1024 * 1024):
            raise ValueError('unexpected Mini 2 source scope or format')
    return selected


def member_metadata(raw):
    # Metadata only: no HDF5 signal array is decoded into numerical IQ.
    with h5py.File(io.BytesIO(raw), 'r') as h:
        datasets = []
        def visit(name, item):
            if isinstance(item, h5py.Dataset) and item.dtype.names == ('real', 'imag'):
                datasets.append((name, item))
        h.visititems(visit)
        if len(datasets) != 1:
            raise ValueError('exactly one compound complex dataset required')
        name, d = datasets[0]
        if len(d.shape) != 2 or d.shape[0] != 1 or d.shape[1] <= 0:
            raise ValueError('unexpected complex vector shape')
        if any(d.dtype.fields[k][0].kind != 'f' for k in ('real', 'imag')):
            raise ValueError('floating point complex components required')
        return dict(dataset_key=name, shape=list(d.shape), dtype=str(d.dtype),
                    total_samples=d.shape[1], duration_seconds=d.shape[1]/100_000_000)


def audit(review_path, plan_path, output):
    inputs = [Path(review_path), Path(plan_path)]
    original = {str(p): dict(stat=stamp(p), sha256=sha256(p)) for p in inputs}
    rows = selected_members(load_json(review_path), load_json(plan_path))
    source = Path(rows[0]['source']); before = stamp(source)
    out = new_output(output, [source.parent] + [p.parent for p in inputs])
    write_json(out/'progress.json', dict(phase='hashing_archive', training_started=False))
    md5, strong = hashlib.md5(), hashlib.sha256()
    with read_source(source) as f:
        for block in iter(lambda: f.read(4*1024*1024), b''):
            md5.update(block); strong.update(block)
    if md5.hexdigest() != PUBLISHER_MD5:
        raise ValueError('publisher archive checksum mismatch')
    members = []
    with read_source(source) as f, zipfile.ZipFile(f) as z:
        names = z.namelist()
        for r in rows:
            if names.count(r['member']) != 1:
                raise ValueError('missing or duplicate selected member')
            entry = z.getinfo(r['member'])
            if entry.file_size != r['member_bytes'] or entry.CRC != r['member_crc32']:
                raise ValueError('frozen member metadata mismatch')
            raw = z.read(entry)  # Full selected-member decompression verifies ZIP CRC.
            members.append(dict(recording_id=r['recording_id'], member=r['member'],
                member_sha256=hashlib.sha256(raw).hexdigest(), member_bytes=len(raw),
                crc32=entry.CRC, crc_verified=True, partition='train',
                collection_group=r['split_group'], context_label=r['label'],
                **member_metadata(raw), per_crop_label_inheritance=False,
                independent_evaluation_eligible=False, training_exported=False))
            write_json(out/'progress.json', dict(phase='member_metadata', completed=len(members), total=len(rows)))
    if stamp(source) != before:
        raise ValueError('source stat changed during reading')
    for p in inputs:
        if stamp(p) != original[str(p)]['stat'] or sha256(p) != original[str(p)]['sha256']:
            raise ValueError('frozen input changed')
    result = dict(schema='rfmon.drone_ml.leuven_pretraining_audit.v1',
        publisher_url=PUBLISHER_URL, publisher_md5=PUBLISHER_MD5,
        archive=str(source), archive_sha256=strong.hexdigest(), archive_bytes=before[2],
        archive_stat=before, source_stat_preserved=True, publisher_checksum_match=True,
        input_provenance=original, auditor_sha256=sha256(__file__), members=members,
        training_ready=False, training_started=False, numerical_iq_decoded=False,
        final_test_members_read=False, scoped_training_exported=False,
        limitations=['recording-level system context only', 'unresolved physical unit/session independence',
                     'archive-member scope must be supported before supervision export',
                     'no signal-quality or field generalization validation', 'publisher CC-BY-NC-4.0'])
    write_json(out/'audit.json', result)
    write_json(out/'progress.json', dict(phase='complete', members=len(members), training_started=False))
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('review', 'plan', 'output'):
        p.add_argument('--'+key, type=Path, required=True)
    args = p.parse_args()
    r = audit(args.review, args.plan, args.output)
    print(dict(members=len(r['members']), publisher_checksum_match=True, training_started=False))


if __name__ == '__main__':
    main()
