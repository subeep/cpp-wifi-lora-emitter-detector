#!/usr/bin/env python3
"""Audit must not turn readable IQ or development labels into validation truth."""
import copy
from pathlib import Path
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools/cyclostationary'))
from audit_link_references import audit, mini2_records, probe_file


def rejects(fn):
    try:
        fn()
    except (ValueError, OSError):
        return
    raise AssertionError('invalid audit input accepted')


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    path = root/'large.cf32'
    with path.open('wb') as f:
        f.truncate(8*1000000)
    p = probe_file(path, 'cf32_le', path.stat().st_size)
    assert p['bytes_read'] == 3*4096*8 and p['unique_samples_probed'] == 3*4096
    assert p['status'] == 'probe_finite' and not p['whole_file_probed']
    assert not p['format_verified'] and p['full_file_sha256'] is None
    assert not p['continuity_verified']
    # Head-only audits miss this fault; our middle probe must report it.
    with path.open('r+b') as f:
        f.seek(500000*8)
        f.write(struct.pack('<ff', float('nan'), 0))
    assert probe_file(path, 'cf32_le', path.stat().st_size)['status'] == 'probe_nonfinite'
    small = root/'small.cf32'
    small.write_bytes(struct.pack('<ff', 1, 0)*100)
    q = probe_file(small, 'cf32_le', 800)
    assert q['unique_samples_probed'] == 100 and q['bytes_read'] == 800 and q['whole_file_probed']
    rejects(lambda: probe_file(small, 'cf32_le', 808))
    rejects(lambda: probe_file(small, 'native_guess', 800))
    rejects(lambda: probe_file(root, 'cf32_le', 800))
    row = {'recording_id':'a', 'path':str(small), 'bytes':800, 'format':'cf32_le',
           'sample_rate_hz':60e6, 'rate_status':'provisional_publisher_unit_ambiguity',
           'source':'test', 'split_group':'unit-unresolved', 'physical_unit_id':None,
           'session_id':None, 'partition':'development_only', 'link_family_truth':None}
    original = copy.deepcopy(row)
    report = audit([row])
    assert row == original and report['summary']['release_validation_eligible'] == 0
    assert 'sample_rate_provisional' in report['records'][0]['release_blockers']
    assert not report['family_acceptance_enabled']
    second = dict(row, recording_id='b')
    alias = root/'alias.cf32'
    alias.symlink_to(small)
    second['path'] = str(alias)
    assert audit([row, second])['summary']['same_physical_file_aliases'] == [['a', 'b']]
    # Even supplied labels and unit/session metadata cannot rescue exposed data.
    complete = dict(row, physical_unit_id='u', session_id='s', link_family_truth='claimed')
    assert not audit([complete])['records'][0]['release_validation_eligible']
    rejects(lambda: audit([dict(complete, partition='test')]))
    rejects(lambda: audit([row, row]))
    rejects(lambda: audit([dict(row, sample_rate_hz=float('nan'))]))
    assert audit([dict(row, path=str(root/'missing'))])['records'][0]['audit']['status'] == 'unavailable_or_invalid'
    rejects(lambda: mini2_records({'captures':[{'partition':'test','independent_test':True}]}))
print('bounded probes, nonfinite middle, unchanged inputs, alias and false-validation guards passed')
