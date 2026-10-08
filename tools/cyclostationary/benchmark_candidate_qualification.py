#!/usr/bin/env python3
"""Check candidate accounting/provenance and exact legacy output preservation.

Synthetic timing coverage and representative development windows only; not
receiver qualification, signal-detection recall or link-identification accuracy.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import time


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(tool, options):
    start = time.monotonic()
    r = subprocess.run([str(tool), 'analyze', *map(str, options)], capture_output=True, text=True, timeout=30, check=True)
    return json.loads(r.stdout), (time.monotonic()-start)*1000


def synthetic(kind, n, seed):
    rng = random.Random(seed)
    data, phase = bytearray(), 0.0
    spans = [(n//8,n//8+n//10), (n//2,n//2+n//10), (3*n//4,3*n//4+n//10)]
    bit = False
    for i in range(n):
        if kind == 'tone':
            z = complex(math.cos(.31*i), math.sin(.31*i))
        elif kind == 'fsk':
            if i % 64 == 0:
                bit = bool(rng.getrandbits(1))
            phase += 2*math.pi*(.14+(.025 if bit else -.025))
            z = complex(math.cos(phase), math.sin(phase))
        elif kind == 'three_bursts':
            gain = next((float(3-k) for k,(a,b) in enumerate(spans) if a <= i < b), .005)
            z = gain*complex(rng.gauss(0,1), rng.gauss(0,1))
        elif kind == 'two_tones':
            z = complex(math.cos(.21*i), math.sin(.21*i)) + .4*complex(math.cos(1.5*i), math.sin(1.5*i))
        else:
            raise ValueError(kind)
        data.extend(struct.pack('<ff', z.real, z.imag))
    return data, spans if kind == 'three_bursts' else []


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool', type=Path, required=True)
    p.add_argument('--baseline-tool', type=Path, required=True)
    p.add_argument('--audit', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    args.tool, args.baseline_tool = args.tool.resolve(), args.baseline_tool.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = []
    for kind in ('tone', 'fsk', 'three_bursts', 'two_tones'):
        for n in (8192, 65536):
            raw, spans = synthetic(kind, n, 6201)
            path = args.output/(kind+'_'+str(n)+'.cf32')
            path.write_bytes(raw)
            for rate in (20e6, 56e6):
                for width in (0, .8*rate):
                    cases.append({'path':str(path), 'format':'cf32_le','sample_rate_hz':rate,
                        'capture_center_hz':2444.5e6,'offset':0,'samples':n,'width':width,
                        'source_group':'synthetic:'+kind+':6201','known_burst_spans':spans,
                        'kind':kind, 'declaration':'software_profile_not_receiver_calibration'})
    # One representative record per existing conservative group, plus the
    # same-session target-off comparison. Do not imply this samples the corpus.
    groups = set()
    for r in json.loads(args.audit.read_text())['records']:
        if r['audit']['status'] != 'probe_finite':
            continue
        if r['split_group'] in groups and r['recording_id'] != 'mini2:target-off25':
            continue
        groups.add(r['split_group'])
        n = r['audit']['file_samples']
        for width in (0, .8*r['sample_rate_hz']):
            cases.append({'path':r['path'],'format':r['format'],'sample_rate_hz':r['sample_rate_hz'],
                'capture_center_hz':r['capture_center_hz'],'offset':max(0,(n-65536)//2),
                'samples':min(n,65536),'width':width,'source_group':r['split_group'],
                'recording_id':r['recording_id'],'known_burst_spans':[], 'kind':'received_development',
                'declaration':'hypothetical_width_for_software_test_not_calibrated_RF_coverage'})
    records, statuses = [], Counter()
    for index, case in enumerate(cases):
        path = Path(case['path'])
        before = path.stat()
        width = 8 if case['format']=='cf32_le' else 4
        def read_window():
            with path.open('rb') as f:
                f.seek(case['offset']*width)
                data = f.read(case['samples']*width)
            assert len(data)==case['samples']*width
            return data
        original_sha = sha(read_window())
        options = ['--input',path,'--format',case['format'],'--sample-rate',case['sample_rate_hz'],
                   '--center',case['capture_center_hz'],'--offset',case['offset'],'--samples',case['samples'],
                   '--frames',256,'--alpha-bins',1,'--prepare-candidates']
        if case['width']:
            options += ['--usable-bandwidth',case['width']]
        baseline,_ = run(args.baseline_tool, options)
        measured,elapsed = run(args.tool, options+['--qualify-candidates'])
        q = measured.pop('candidate_qualification')
        assert measured == baseline, 'legacy output drift: '+str(index)
        assert q['proposal_accounting_exact'] and q['returned_proposals']<=2
        assert q['proposals_seen']==q['returned_proposals']+q['duplicates_of_returned']+q['budget_omitted_proposals']
        assert q['output_center_extent_union_samples']<=q['filter_input_time_union_samples']<=q['prepared_region_time_union_samples']<=q['analysis_samples']
        assert measured['candidate_preparation']['fir_operations']<=64000000
        assert measured['classification']['link_family']=='unknown'
        assert sha(read_window()) == original_sha
        after=path.stat()
        assert (before.st_size,before.st_ino,before.st_mtime_ns,before.st_ctime_ns)==(after.st_size,after.st_ino,after.st_mtime_ns,after.st_ctime_ns)
        time_overlaps=[]
        candidates=measured['candidate_preparation']['candidates']
        for a,b in case['known_burst_spans']:
            time_overlaps.append(any(c['source_region_first_sample']<b and c['source_region_last_original_sample']>=a
                for c in candidates))
        statuses.update(c['preparation_status'] for c in q['candidates'])
        records.append({'case':case,'window_sha256':original_sha,'legacy_fields_exact':True,
            'source_window_and_stat_unchanged':True,'process_wall_ms':elapsed,'qualification':q,
            'known_burst_returned_region_time_overlap':time_overlaps,
            'overlap_meaning':'time overlap only; not successful filtering, detection or identity',
            'candidate_preparation':measured['candidate_preparation']})
        if (index+1)%16==0:
            print(index+1,'/',len(cases),flush=True)
    summary={'scope':'offline_candidate_accounting_and_legacy_preservation_not_receiver_or_identity_validation',
        'cases':len(records),'conservative_source_groups':len({r['case']['source_group'] for r in records}),
        'all_legacy_fields_exact':True,'all_source_windows_and_stats_unchanged':True,
        'status_counts':dict(statuses),'radio_opened':False,'classification_enabled':False,
        'tool_sha256':sha(args.tool.read_bytes()),'baseline_tool_sha256':sha(args.baseline_tool.read_bytes()),
        'script_sha256':sha(Path(__file__).read_bytes()),'audit_sha256':sha(args.audit.read_bytes()),
        'elapsed_process_ms_range':[min(r['process_wall_ms'] for r in records),max(r['process_wall_ms'] for r in records)],
        'records':records}
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2,allow_nan=False)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k!='records'},indent=2))


if __name__ == '__main__':
    main()
