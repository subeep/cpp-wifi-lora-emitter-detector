"""Read-only label/split and packet-report audit. Does not authorize training.

Reuses frozen partitions, reads ZIP metadata (never extracts archives), and
checks one bounded first-block format probe per FPV session. These probes are
not background labels and do not establish device-time continuity.
"""
import argparse
from collections import Counter, defaultdict
from datetime import datetime
import hashlib
import io
import json
from pathlib import Path
import zipfile

import numpy as np

from .common import read_source, sha256, stamp, new_output, write_json


def csv_rows(raw):
    import csv
    return list(csv.DictReader(io.StringIO(raw.decode('utf-8-sig'))))


def load_json(path):
    with read_source(path) as f:
        return json.load(f)


def validate_frozen_review(review, plan):
    if review.get('schema') != 'rfmon.drone_ml.expanded_review.v1':
        raise ValueError('unsupported review')
    if plan.get('schema') != 'rfmon.drone_ml.expanded_split.v1':
        raise ValueError('unsupported split')
    groups = plan['groups']
    signatures = set()
    for row in review['records']:
        if groups.get(row['split_group']) != dict(partition=row['partition'], label=row['label']):
            raise ValueError('frozen partition or label mismatch')
        key = (row['source'], row.get('member'))
        if key in signatures:
            raise ValueError('duplicate reviewed member')
        signatures.add(key)
    if set(groups) != {r['split_group'] for r in review['records']}:
        raise ValueError('unaccounted split group')


def packet_entries(rows, review):
    """Resolve source context conservatively; never assign packet identity."""
    grouped = defaultdict(list)
    for r in rows:
        grouped[(r['dataset'], r['drone'], r['source'])].append(r)
    result = []
    dsmap = {'KULeuven': 'KULeuven_DroneRF', 'Tampere': 'Tampere_DroneRF'}
    for (dataset, alias, source), regions in sorted(grouped.items()):
        ds = dsmap.get(dataset, dataset)
        matches = [r for r in review['records'] if r['dataset'] == ds and
                   ((r.get('member') == source) or
                    (not r.get('member') and Path(r['source']).name == source))]
        partitions = sorted({r['partition'] for r in matches})
        protected = any('test' in p for p in partitions)
        result.append(dict(dataset=dataset, source_alias=alias, source_member=source,
            context_labels=sorted({r['label'] for r in matches}),
            existing_partitions=partitions, existing_groups=sorted({r['split_group'] for r in matches}),
            resolution='matched_existing_review' if matches else 'unresolved_do_not_infer',
            report_regions=len(regions), report_exposure=True,
            exposure_warning='reserved_source_plot_seen_raw_IQ_not_read' if protected else None,
            raw_iq_read_allowed=False, verified_presence_label=None, verified_identity_label=None,
            training_eligible=False, region_scope='automatic_energy_regions_any_emitter',
            proposed_regions=regions))
    return result


def validate_blocks(rows, iq_bytes):
    if not rows or len(rows) > 100000:
        raise ValueError('invalid sweep row count')
    previous_end = 0
    for r in rows:
        offset, count = int(r['offset_bytes']), int(r['num_samples'])
        freq = float(r['center_freq'])
        if (offset < previous_end or offset % 8 or not 0 < count <= 4_000_000
                or offset + count * 8 > iq_bytes or not np.isfinite(freq) or freq <= 0):
            raise ValueError('invalid sweep geometry under cf32 hypothesis')
        previous_end = offset + count * 8
    return previous_end


def probe_block(raw, declared_samples, rate):
    if (type(declared_samples) is not int or not 0<declared_samples<=4_000_000
            or not np.isfinite(rate) or not 1e6<=rate<=200e6):
        raise ValueError('invalid format probe geometry')
    if len(raw) != declared_samples * 8:
        raise ValueError('short format probe')
    x = np.frombuffer(raw, dtype='<c8')
    if not np.isfinite(x).all():
        raise ValueError('nonfinite format probe')
    nz = np.flatnonzero(x)
    nonzero_extent = int(nz[-1] + 1) if len(nz) else 0
    return dict(encoding_hypothesis='cf32_le', declared_samples=declared_samples,
        last_nonzero_extent_samples=nonzero_extent,
        trailing_zero_samples=declared_samples-nonzero_extent,
        declared_seconds=declared_samples/rate,
        nonzero_extent_seconds=nonzero_extent/rate,
        zero_fraction=float(np.mean(x == 0)), iq_prefix_sha256=hashlib.sha256(raw).hexdigest(),
        physical_continuity_verified=False, training_eligible=False,
        interpretation='Observed zero tail is not permission to trim or join blocks; encoding/timing still require source reconciliation.')


def review_fpv(root, contexts):
    root = Path(root)
    mapped = {}
    for r in contexts:
        if r['iq_folder'] in mapped:
            raise ValueError('duplicate FPV session context')
        mapped[r['iq_folder']] = r
    sessions, evidence = [], []
    seen = set()
    for path in sorted(root.glob('*.zip')):
        before = stamp(path)
        with read_source(path) as f, zipfile.ZipFile(f) as z:
            names = [x.filename for x in z.infolist()]
            if len(names) != len(set(names)):
                raise ValueError('duplicate archive member names')
            for member in sorted((x for x in z.infolist() if x.filename.endswith('/metadata.csv')),
                                 key=lambda x:x.filename):
                if member.file_size > 4_000_000:
                    raise ValueError('oversized metadata')
                folder = Path(member.filename).parent.name
                if folder not in mapped or folder in seen:
                    raise ValueError('unmapped or duplicate FPV folder')
                seen.add(folder)
                context = mapped[folder]
                raw = z.read(member)  # Full small CSV; ZIP CRC checked.
                rows = csv_rows(raw)
                iq_name = str(Path(member.filename).parent / 'iq.bin')
                iq_info = z.getinfo(iq_name)
                used_bytes = validate_blocks(rows, iq_info.file_size)
                rate = float(context['sample_rate']) * 1e6
                if not np.isfinite(rate) or not 1e6 <= rate <= 200e6:
                    raise ValueError('unsupported declared FPV rate')
                first = rows[0]
                if int(first['offset_bytes']) != 0:
                    raise ValueError('prefix probe requires offset zero')
                n = int(first['num_samples'])
                with z.open(iq_info) as q:
                    prefix = q.read(n*8)
                probe = probe_block(prefix, n, rate)
                delta = []
                for a,b in zip(rows, rows[1:]):
                    if a['center_freq'] == b['center_freq']:
                        delta.append((datetime.fromisoformat(b['timestamp'])-
                                      datetime.fromisoformat(a['timestamp'])).total_seconds())
                absent = context['drone_freq'] == 'none'
                sessions.append(dict(archive=str(path.resolve()), member=iq_name, session=folder,
                    metadata_member=member.filename, metadata_sha256=hashlib.sha256(raw).hexdigest(),
                    metadata_crc_verified=True, iq_crc_verified=False, iq_member_bytes=iq_info.file_size,
                    described_iq_bytes=used_bytes, metadata_rows=len(rows), context=context,
                    declared_rate_hz=rate, block_sample_counts=sorted({int(r['num_samples']) for r in rows}),
                    frequencies_hz=sorted({float(r['center_freq']) for r in rows}),
                    median_same_frequency_timestamp_delta_seconds=float(np.median(delta)) if delta else None,
                    first_block_frequency_hz=float(first['center_freq']), first_block_probe=probe,
                    evidence_label='source_declared_target_absent' if absent else 'source_declared_target_context',
                    verified_presence_label=None, verified_identity_label=None, training_eligible=False,
                    partition='review_only_unassigned',
                    conservative_collection_group='FPV:'+context['environment'],
                    grouping_basis='environment groups all related sweeps conservatively; no independence claimed',
                    issue='zero_tail_observed' if probe['trailing_zero_samples'] else 'encoding_and_timestamps_unverified',
                    label_scope='target_transmitter_context_not_verified_global_drone_absence'))
        if stamp(path) != before:
            raise ValueError('archive changed during review')
        evidence.append(dict(path=str(path.resolve()), stat=before, stat_preserved=True,
                             full_archive_hash_verified=False))
    if seen != set(mapped):
        raise ValueError('missing FPV session archives')
    return sessions, evidence


def audit(review_path, packet_csv, fpv_root, nist_path, derived_path, output):
    review_path, packet_csv, fpv_root, nist_path, derived_path = map(Path,
        (review_path, packet_csv, fpv_root, nist_path, derived_path))
    split_path = review_path.parent/'split_plan.json'
    inputs = [review_path, split_path, packet_csv, fpv_root/'iq_recording_meta.csv', nist_path, derived_path]
    before = {str(p): stamp(p) for p in inputs}
    digests = {str(p): sha256(p) for p in inputs}
    review, split = load_json(review_path), load_json(split_path)
    if digests[str(split_path)] != review['split_sha256']:
        raise ValueError('split checksum mismatch')
    validate_frozen_review(review, split)
    with read_source(packet_csv) as f: entries = packet_entries(csv_rows(f.read()), review)
    with read_source(fpv_root/'iq_recording_meta.csv') as f: contexts = csv_rows(f.read())
    # Output location is validated before any IQ format probes.
    out = new_output(output, [review_path.parent, packet_csv.parent, fpv_root,
                              Path(review['source_root']), nist_path.parent, derived_path.parent])
    sessions, archives = review_fpv(fpv_root, contexts)
    nist = load_json(nist_path)
    if nist.get('schema') != 'rfmon.drone_ml.public_background_inventory.v1' or not nist.get('publisher_sha256_verified'):
        raise ValueError('unverified NIST inventory')
    if any(r['partition'] != 'development_only' or r.get('training_eligible') for r in nist['records']):
        raise ValueError('NIST development assignment changed')
    derivatives = []
    for d in load_json(derived_path)['records']:
        matches = [r for r in review['records'] if r['source'] == d['source'] and r.get('member') == d.get('archive_member')]
        derivatives.append(dict(path=d['path'], source=d['source'], member=d.get('archive_member'),
            original_partitions=sorted({r['partition'] for r in matches}),
            derivative_partition=d.get('partition'),
            independent_evaluation_eligible=False,
            reason='same_source_diagnostic_derivative_not_independent_holdout'))
    class_groups = defaultdict(lambda: defaultdict(set))
    for r in review['records']: class_groups[r['label']][r['partition']].add(r['split_group'])
    group_counts = {label:{p:len(g) for p,g in parts.items()} for label,parts in sorted(class_groups.items())}
    ledger = [dict(kind='report_resolution', dataset=e['dataset'], alias=e['source_alias'],
                   reason=e['exposure_warning'] or e['resolution']) for e in entries
              if e['exposure_warning'] or e['resolution'] != 'matched_existing_review']
    ledger += [dict(kind='existing_source_exclusion',source=d['source'],reason=d['reason'])
               for d in review['source_decisions'] if d['use']=='excluded']
    ledger += [dict(kind='FPV_format',session=s['session'],reason=s['issue']) for s in sessions]
    result = dict(schema='rfmon.drone_ml.recognition_review.v1', input_sha256=digests,
        frozen_records=review['records'], frozen_groups=split['groups'], class_group_counts=group_counts,
        packet_report_entries=entries, fpv_sessions=sessions, archive_evidence=archives,
        nist_records=nist['records'], derivative_lineage=derivatives, exclusion_ledger=ledger,
        summary=dict(frozen_records=len(review['records']), groups=len(split['groups']),
            report_entries=len(entries), report_regions=sum(e['report_regions'] for e in entries),
            report_entries_with_reserved_exposure=sum(bool(e['exposure_warning']) for e in entries),
            fpv_sessions=len(sessions), fpv_target_absent_contexts=sum(s['context']['drone_freq']=='none' for s in sessions),
            fpv_probes_with_zero_tail=sum(s['first_block_probe']['trailing_zero_samples']>0 for s in sessions),
            nist_captures=len(nist['records']), newly_verified_presence_labels=0),
        new_training_authorized=False, existing_partitions_changed=False, final_test_iq_read=False,
        note='Frozen labels remain publisher context. This audit is not a training manifest or a field evaluation.')
    if any(stamp(p) != before[str(p)] for p in inputs):
        raise ValueError('metadata input changed')
    result['input_stats_preserved'] = True
    write_json(out/'review.json', result)
    write_json(out/'summary.json', result['summary'])
    write_json(out/'exclusion_ledger.json', ledger)
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('review','packet-csv','fpv-root','nist-inventory','derived-manifest','output'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    r=audit(a.review,a.packet_csv,a.fpv_root,a.nist_inventory,a.derived_manifest,a.output)
    print(json.dumps(r['summary'],indent=2))


if __name__=='__main__': main()
