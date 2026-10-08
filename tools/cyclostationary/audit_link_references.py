#!/usr/bin/env python3
"""Bounded read-only audit of development references for link identification.

No discovery of labels from IQ, automatic held-out split, radio use or training.
Head/middle/tail probes are explicitly not whole-file integrity or continuity.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import struct

from validate_reference_corpus import audit_partitions, corpus

ROOT = Path(__file__).resolve().parents[2]
PROBE_SAMPLES = 4096


def load(path):
    return json.loads(path.read_text())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def signature(s):
    return s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns


def probe_file(path, fmt, expected_bytes):
    """Only a regular file; bounded bytes even for hundreds of GB of references."""
    if fmt not in ('cf32_le', 'ci16_le'):
        raise ValueError('unsupported IQ format')
    width, unpack = (8, '<ff') if fmt == 'cf32_le' else (4, '<hh')
    # NONBLOCK prevents a mistaken FIFO path from hanging before fstat.
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise ValueError('reference must be a regular file')
        if before.st_size != expected_bytes or before.st_size % width or not before.st_size:
            raise ValueError('empty, misaligned or changed reference size')
        samples = before.st_size // width
        count = min(PROBE_SAMPLES, samples)
        starts = sorted({0, (samples-count)//2, samples-count})
        probes, intervals = [], []
        for offset in starts:
            stream.seek(offset*width)
            data = stream.read(count*width)
            if len(data) != count*width:
                raise ValueError('short probe read')
            finite = all(math.isfinite(i) and math.isfinite(q) for i, q in struct.iter_unpack(unpack, data))
            probes.append({'offset_samples': offset, 'samples': count,
                           'sha256': hashlib.sha256(data).hexdigest(), 'finite': finite})
            intervals.append((offset, offset+count))
        after = os.fstat(stream.fileno())
    if signature(before) != signature(after) or signature(before) != signature(Path(path).stat()):
        raise ValueError('reference changed during audit')
    covered, end = 0, 0
    for a, b in intervals:
        covered += max(0, b-max(a, end))
        end = max(end, b)
    return {'status': 'probe_finite' if all(p['finite'] for p in probes) else 'probe_nonfinite',
            'resolved_path': str(Path(path).resolve()), 'file_bytes': before.st_size,
            'file_samples': samples, 'bytes_read': len(starts)*count*width,
            'unique_samples_probed': covered, 'sample_fraction_probed': covered/samples,
            'whole_file_probed': covered == samples, 'full_file_sha256': None,
            'continuity_verified': False, 'format_verified': False,
            'format_scope': 'interpretation_declared_by_manifest_not_inferred_from_finiteness',
            'size_mtime_ctime_inode_unchanged': True,
            'physical_file_key': [before.st_dev, before.st_ino], 'probes': probes}


def mini2_records(manifest):
    records = []
    for r in manifest['captures']:
        # This importer is intentionally development-only. A future sealed
        # corpus needs a separate explicit acquisition/split workflow.
        if r.get('partition') != 'development_only' or r.get('independent_test') is not False:
            raise ValueError('Mini 2 importer accepts development references only')
        path = Path(r['path'])
        records.append({'recording_id': 'mini2:'+r['id'], 'path': str(path),
            'bytes': path.stat().st_size, 'format': r['format'],
            'sample_rate_hz': r['sample_rate_hz'], 'capture_center_hz': r['center_frequency_hz'],
            'source': 'mini2-user-session', 'context_labels': {'operating_state': r['operating_state']},
            'labels_status': 'user_state_label_not_link_protocol_truth', 'rate_status': 'recorder_declared',
            'split_group': r['source_group'], 'physical_unit_id': r['physical_unit_id'],
            'session_id': r['session'], 'partition': 'development_only',
            'link_family_truth': None, 'drone_free_control': False,
            'target_off_is_environment_drone_free': False,
            'independent_unit_or_session_test_eligible': False,
            'previous_full_file_sha256': r['sha256'],
            'checksum_scope': 'prior_manifest_claim_not_reverified_by_bounded_audit',
            'continuity': r['continuity_source']})
    return records


def audit(records):
    audit_partitions(records)
    if len({r['recording_id'] for r in records}) != len(records):
        raise ValueError('duplicate recording identifier')
    if any(r['partition'] != 'development_only' for r in records):
        raise ValueError('audit cannot promote exposed references to sealed evaluation')
    output, physical_files = [], defaultdict(list)
    for record in sorted(records, key=lambda r: r['recording_id']):
        r = dict(record)
        rate = r.get('sample_rate_hz')
        if not isinstance(rate, (int, float)) or isinstance(rate, bool) or not math.isfinite(rate) or not 0 < rate <= 1e9:
            raise ValueError('invalid declared sample rate')
        blockers = ['development_exposed', 'receiver_passband_unqualified', 'continuity_not_independently_verified']
        for field in ('physical_unit_id', 'session_id', 'link_family_truth'):
            if not r.get(field):
                blockers.append(field+'_unknown')
        if 'provisional' in r.get('rate_status', ''):
            blockers.append('sample_rate_provisional')
        try:
            observation = probe_file(r['path'], r['format'], r['bytes'])
            physical_files[tuple(observation['physical_file_key'])].append(r['recording_id'])
            if observation['status'] != 'probe_finite':
                blockers.append('nonfinite_probe')
        except (OSError, ValueError) as e:
            observation = {'status': 'unavailable_or_invalid', 'error': str(e)}
            blockers.append('reference_unavailable_or_invalid')
        r.update({'audit': observation, 'release_validation_eligible': False,
                  'release_blockers': blockers, 'analog_usable_bandwidth_hz': None})
        output.append(r)
    aliases = [ids for ids in physical_files.values() if len(ids) > 1]
    summary = {'recordings': len(output), 'source_counts': dict(Counter(r['source'] for r in output)),
        'conservative_source_groups': len({r['split_group'] for r in output}),
        'audit_status_counts': dict(Counter(r['audit']['status'] for r in output)),
        'bytes_read': sum(r['audit'].get('bytes_read', 0) for r in output),
        'release_validation_eligible': 0, 'sealed_test_records': 0,
        'blocker_counts': dict(Counter(b for r in output for b in r['release_blockers'])),
        'same_physical_file_aliases': aliases,
        'independence_note': 'source groups are conservative leakage guards, not independent encounters',
        'probe_note': 'bounded samples only; unread samples, file format and continuity are not verified',
        'ordinary_wifi_label_note': 'decoded waveform reference; not proof of a drone-free environment'}
    return {'schema': 'rfmon.link_reference_audit.v1', 'radio_opened': False, 'model_trained': False,
            'family_acceptance_enabled': False, 'summary': summary, 'records': output}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inventory', type=Path, default=ROOT/'data/wifi_drone_analysis/offline_2026-10-02/dronedetect_inventory.json')
    p.add_argument('--zenodo', type=Path, default=ROOT/'data/wifi_drone_analysis/offline_2026-10-03/zenodo_verified_local_manifest.json')
    p.add_argument('--mini2', type=Path, default=ROOT/'data/wifi_drone_analysis/mini2_baseline_full_2026-10-05/manifest.json')
    p.add_argument('--fixtures', type=Path, default=ROOT/'tests/fixtures/wifi_ofdm')
    p.add_argument('--scope', type=Path, default=ROOT/'tools/cyclostationary/link_identification_scope.json')
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    inputs = [args.inventory, args.zenodo, args.mini2, args.scope]
    inputs += [args.fixtures/(name+'.json') for name in ('airtel-2g4','avgarde-5g','avgarde-5g-offset')]
    manifests = [{'path': str(path.resolve()), 'sha256': digest(path)} for path in inputs]
    # Validate the reviewed source profile has not silently drifted.
    scope = load(args.scope)
    for source in scope['reviewed_source_files']:
        if digest(ROOT/source['path']) != source['sha256']:
            raise ValueError('source profile changed; review scope: '+source['path'])
    records = corpus(load(args.inventory), load(args.zenodo), args.fixtures)
    records += mini2_records(load(args.mini2))
    report = audit(records)
    report['scope_contract'] = scope
    if any(digest(path) != entry['sha256'] for path,entry in zip(inputs,manifests)):
        raise ValueError('input metadata changed during audit')
    report['input_manifests'] = manifests
    report['audit_tool_sha256'] = digest(Path(__file__))
    report['corpus_importer_sha256'] = digest(Path(__file__).with_name('validate_reference_corpus.py'))
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output/'audit.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
    (args.output/'scope.json').write_text(json.dumps(scope, indent=2, allow_nan=False)+'\n')
    print(json.dumps(report['summary'], indent=2))
    return int(any(r['audit']['status'] != 'probe_finite' for r in report['records']))


if __name__ == '__main__':
    raise SystemExit(main())
