#!/usr/bin/env python3
"""Compare opt-in routing using existing qualification cases; no identity claims."""
import argparse
import hashlib
import json
from pathlib import Path
from benchmark_candidate_qualification import run

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--tool', type=Path, required=True)
p.add_argument('--baseline-tool', type=Path, required=True)
p.add_argument('--cases', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
records = []
for index, old in enumerate(json.loads(a.cases.read_text())['records']):
    c = old['case']
    path = Path(c['path'])
    size = 8 if c['format'] == 'cf32_le' else 4
    def fingerprint():
        st = path.stat()
        with path.open('rb') as f:
            f.seek(c['offset'] * size)
            raw = f.read(c['samples'] * size)
        assert len(raw) == c['samples'] * size
        return [st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns, hashlib.sha256(raw).hexdigest()]
    before = fingerprint()
    opts = ['--input', path, '--format', c['format'], '--sample-rate', c['sample_rate_hz'],
            '--center', c['capture_center_hz'], '--offset', c['offset'], '--samples', c['samples'],
            '--frames', 256, '--alpha-bins', 1, '--prepare-candidates', '--qualify-candidates']
    if c['width']:
        opts += ['--usable-bandwidth', c['width']]
    baseline, _ = run(a.baseline_tool.resolve(), opts)
    default, _ = run(a.tool.resolve(), opts)
    assert baseline == default, f'default drift {index}'
    rounds = []
    for round_ in range(2):
        routed, ms = run(a.tool.resolve(), opts + ['--route-candidates', '--candidate-round', round_])
        q = routed['candidate_qualification']
        assert q['proposal_accounting_exact'] and q['returned_proposals'] <= 2
        assert routed['candidate_preparation']['fir_operations'] <= 64000000
        for k in default:
            if k not in ('candidate_preparation', 'candidate_qualification'):
                assert default[k] == routed[k], (index, k)
        overlaps = [any(x['source_region_first_sample'] < end and x['source_region_last_original_sample'] >= start
                        for x in routed['candidate_preparation']['candidates']) for start, end in c['known_burst_spans']]
        rounds.append({'round': round_, 'wall_ms': ms, 'qualification': q,
                       'known_burst_region_time_overlap': overlaps})
    assert fingerprint() == before
    records.append({'case': c, 'default_exact': True, 'source_unchanged': True,
                    'before_cyclic_supported': default['candidate_qualification']['cyclic_supported_candidates'],
                    'rounds': rounds})
    if (index+1) % 16 == 0:
        print(index+1, flush=True)
summary = {'scope': 'development_software_only_not_detection_recall_or_identity_validation',
           'cases': len(records), 'all_defaults_exact': True, 'all_sources_unchanged': True,
           'before_cyclic_supported': sum(r['before_cyclic_supported'] for r in records),
           'round0_cyclic_supported': sum(r['rounds'][0]['qualification']['cyclic_supported_candidates'] for r in records),
           'hashes': {k: hashlib.sha256(v.read_bytes()).hexdigest() for k,v in
                      [('tool', a.tool), ('baseline', a.baseline_tool), ('cases', a.cases), ('script', Path(__file__))]},
           'records': records}
(a.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
print(json.dumps({k:v for k,v in summary.items() if k != 'records'}, indent=2))
