"""Metadata inventory only: never assigns ground truth or random window splits."""
import argparse
from collections import Counter
import json
from pathlib import Path
import zipfile

from .common import identity, new_output, read_source, stamp, write_json

DATASETS = ('KULeuven_DroneRF', 'Tampere_DroneRF', 'RFUAV', 'DRFF-R2', 'FPV_Analog_Video')


def inventory(root):
    root = Path(root).resolve()
    records, preserved = [], []
    for dataset in DATASETS:
        folder = root / dataset
        if not folder.is_dir():
            continue
        files = sorted(p for p in folder.rglob('*') if p.suffix.lower() in {'.bin', '.mat', '.zip', '.rar'})
        if len(files) > 2000:
            raise ValueError('inventory file budget exceeded')
        for path in files:
            if path.is_symlink() or folder.resolve() not in path.resolve().parents:
                raise ValueError('source symlink outside inventory boundary')
            before = stamp(path)
            row = dict(recording_id=identity(path), dataset=dataset, source=str(path), bytes=before[2],
                       relative_path=str(path.relative_to(folder)), source_stat=before,
                       partition='excluded', label_status='unreviewed_publisher_context',
                       aircraft_model=None, controller_model=None, link_family=None,
                       physical_unit_id=None, session_id=None, negative_eligible=False,
                       release_validation_eligible=False, blockers=['labels_and_independence_unreviewed'])
            # List ZIP directories only, never decompress all IQ or execute source scripts.
            if path.suffix.lower() == '.zip':
                with read_source(path) as f, zipfile.ZipFile(f) as archive:
                    members = archive.infolist()
                    if len(members) > 10000:
                        raise ValueError('archive member budget exceeded')
                    row['members'] = [dict(name=m.filename, bytes=m.file_size, crc32=m.CRC)
                                      for m in members if not m.is_dir()]
                    row['archive_crc_verified'] = False
            if dataset == 'DRFF-R2' and path.suffix.lower() == '.mat':
                import h5py
                import numpy as np
                with read_source(path) as f, h5py.File(f, 'r') as h:
                    fields = {}
                    for key in ('Fs', 'CenterFrequence', 'CenterFreq', 'TD', 'State'):
                        if key in h and isinstance(h[key], h5py.Dataset) and h[key].size <= 128:
                            value = np.asarray(h[key][()]).reshape(-1)
                            fields[key] = (''.join(chr(int(c)) for c in value if c)
                                           if key in ('TD', 'State') else value.tolist())
                    row['embedded_metadata'] = fields
                    if 'environment' in str(path).lower():
                        row['blockers'].append('environment_absence_unverified')
                        if fields.get('TD'):
                            row['blockers'].append('environment_has_target_metadata')
            if dataset == 'RFUAV':
                row['blockers'].append('rar_member_labels_and_iq_not_audited')
                if path.name == 'FRSKY X14.rar':
                    row['blockers'].append('previous_audit_frsky_flysky_label_conflict')
            if stamp(path) != before:
                raise RuntimeError('source changed during audit: ' + str(path))
            preserved.append((path, before))
            records.append(row)
    if any(stamp(p) != s for p, s in preserved):
        raise RuntimeError('source changed before audit completion')
    return dict(schema='rfmon.drone_ml.inventory.v1', records=records,
                summary=dict(files=len(records), datasets=dict(Counter(r['dataset'] for r in records)),
                             train_eligible=0, sealed_test_eligible=0,
                             source_stats_preserved=True, iq_integrity_verified=False))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    out = new_output(a.output, [a.root])
    report = inventory(a.root)
    write_json(out / 'inventory.json', report)
    write_json(out / 'review_template.json', dict(schema='rfmon.drone_ml.review.v1', records=[
        dict(recording_id=r['recording_id'], label=None, role=None, split_group=None,
             partition='excluded', label_evidence=None, independence_evidence=None)
        for r in report['records']]))
    print(json.dumps(report['summary'], indent=2))


if __name__ == '__main__':
    main()
