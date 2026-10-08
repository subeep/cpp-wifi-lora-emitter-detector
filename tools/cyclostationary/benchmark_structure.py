#!/usr/bin/env python3
"""Bounded timing/code development benchmark; no drone or protocol ground truth."""
import argparse
import cmath
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import time
import benchmark_waveforms as breadth
ROOT = Path(__file__).resolve().parents[2]
RATE, N = breadth.RATE, breadth.N


def cp_geometry(seed, useful, prefix):
    # Independent Gaussian complex symbols plus a copied suffix. This tests
    # cyclic-prefix geometry, not a transmitter/protocol/family implementation.
    rng = random.Random(seed)
    x = []
    while len(x) < N:
        symbol = [complex(rng.gauss(0,1), rng.gauss(0,1)) for _ in range(useful)]
        x.extend(symbol[-prefix:] + symbol)
    return x[:N]


def cases():
    yield from breadth.cases()
    for useful, prefix in ((96,24), (96,20), (128,16), (160,32), (256,64), (512,64)):
        for seed in range(3):
            group = f'cp_gaussian_source:{seed}'
            x = cp_geometry(305171+seed, useful, prefix)
            yield f'cp_{useful}_{prefix}_{seed}', 'cp_geometry_clean', group, x, 19e6
            noise = breadth.noise(394171+seed)
            yield f'cp_{useful}_{prefix}_{seed}_noise10', 'cp_geometry_noise10', group, [z+.3162278*n for z,n in zip(x,noise)], 19e6
        x = cp_geometry(305171, useful, prefix)
        yield f'cp_{useful}_{prefix}_discovery_only', 'cp_discovery_only', 'cp_gaussian_source:0', x[:N//2]+breadth.noise(499171,N//2), 19e6
    for chip in (1,4,16):
        for carrier in (-.35e6, 7e6):
            x = breadth.states(794771, 'dsss', period=chip, carrier=carrier)
            yield f'barker_{chip}_{carrier}', 'barker_offset_clean', 'analytic_barker:794771', x, 19e6
    x = breadth.states(794771,'dsss',period=8)
    yield 'barker_discovery_only','barker_discovery_only','analytic_barker:794771',x[:N//2]+breadth.noise(614771,N//2),19e6
    # Same payload, a deliberately different eleven-chip sequence.
    wrong = (1,1,1,1,1,-1,-1,-1,-1,-1,-1)
    correct = (1,1,1,-1,-1,-1,1,-1,-1,1,-1)
    yield 'different_code', 'different_short_code', 'analytic_barker:794771', [z*wrong[(j//8)%11]*correct[(j//8)%11] for j,z in enumerate(x)], 19e6
    for slope in (135.2e9,-135.3e9):
        x = [cmath.exp(2j*math.pi*(-4e6*(j/RATE)+.5*slope*(j/RATE)**2)) for j in range(N)]
        yield f'chirp_{slope}', 'chirp_confusable', f'chirp:{slope}', x, 19e6


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--prior-breadth',type=Path,required=True)
    args=p.parse_args();tool=args.tool.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    prior={r['id']:r for r in json.loads(args.prior_breadth.read_text())['records']}
    records=[]
    def evaluate(name,category,group,path,width):
        before=digest(path)
        command=[str(tool),'analyze','--input',str(path),'--format','cf32_le','--sample-rate',str(RATE),'--samples',str(min(65536,path.stat().st_size//8)),'--frames','256']
        base=json.loads(subprocess.run(command,capture_output=True,text=True,check=True,timeout=20).stdout)
        start=time.monotonic()
        full=json.loads(subprocess.run(command+['--discover-structure'],capture_output=True,text=True,check=True,timeout=20).stdout)
        elapsed=(time.monotonic()-start)*1000
        assert all(full[k]==v for k,v in base.items()) and digest(path)==before,name
        s=full['structure_discovery']
        assert s['ofdm_fft_calls']<=6 and s['timing_hypotheses']<=32 and s['spread_hypotheses']<=32
        assert len(s['ofdm_candidates'])<=4 and len(s['short_code_candidates'])<=4
        historic=False
        if name in prior:
            expanded=command+['--expand-waveforms','--prepare-candidates']
            if width:expanded+=['--usable-bandwidth',str(width)]
            current=json.loads(subprocess.run(expanded,capture_output=True,text=True,check=True,timeout=20).stdout)
            old=prior[name]
            assert old['input_sha256']==before and old['waveform_features']==current['waveform_features'] and old['candidate_preparation']==current['candidate_preparation'],name
            historic=True
        target_timing = None
        if category in ('cp_geometry_clean', 'cp_geometry_noise10'):
            fields=name.split('_'); target_timing=(int(fields[1]),int(fields[2]))
        exact_timing = None if target_timing is None else any(p['pattern_consistent'] and (p['useful_samples'],p['prefix_samples'])==target_timing for p in s['ofdm_candidates'])
        expected_chip = 8 if category in ('dsss_control','dsss_noise10','dsss_echo') else int(name.split('_')[1]) if category=='barker_offset_clean' else None
        exact_chip = None if expected_chip is None else any(p['pattern_consistent'] and p['chip_samples']==expected_chip for p in s['short_code_candidates'])
        records.append({'expected_timing_samples':target_timing,'expected_timing_supported':exact_timing,
            'expected_chip_samples':expected_chip,'expected_chip_supported':exact_chip,'id':name,'category':category,'source_group':group,'input_sha256':before,'roundtrip_ms':elapsed,
            'source_and_legacy_fields_preserved':True,'historical_waveform_and_preparation_equal':historic,
            'structure_discovery':s,'classification':full['classification']})
    with tempfile.TemporaryDirectory(prefix='rfmon-structure-benchmark-') as d:
        path=Path(d)/'control.cf32'
        for name,category,group,x,width in cases():
            path.write_bytes(b''.join(struct.pack('<ff',z.real,z.imag) for z in x))
            evaluate(name,category,group,path,width)
    for path in sorted((ROOT/'tests/fixtures/wifi_ofdm').glob('*.cf32')):
        evaluate(path.stem,'received_wifi_confusable','x310:2026-09-19:received-wifi',path,None)
    counts=defaultdict(Counter)
    for r in records:
        s=r['structure_discovery'];c=counts[r['category']];c['cases']+=1
        c['cp_pattern_cases']+=any(p['pattern_consistent'] for p in s['ofdm_candidates'])
        if r['expected_timing_supported'] is not None:c['exact_timing_cases']+=r['expected_timing_supported']
        if r['expected_chip_supported'] is not None:c['exact_chip_cases']+=r['expected_chip_supported']
        c['barker_compatible_cases']+=any(p['pattern_consistent'] for p in s['short_code_candidates'])
    timings=sorted(r['roundtrip_ms'] for r in records)
    summary={'schema':'rfmon.cyclo.structure_benchmark.v1','method':'bounded_ofdm_barker_discovery_v1',
        'partition':'software_development_controls','radio_opened':False,'model_trained':False,
        'thresholds_frozen_before_run':True,'independent_real_unit_session_test':False,
        'named_family_acceptance_enabled':False,'family_precision_recall':None,
        'cases':len(records),'source_groups':len({r['source_group'] for r in records}),
        'legacy_preservation_cases':len(records),'historical_waveform_preservation_cases':sum(r['historical_waveform_and_preparation_equal'] for r in records),
        'tool_sha256':digest(tool),'source_code_sha256':{str(path.relative_to(ROOT)):digest(path) for path in (Path(__file__),ROOT/'src/cyclostationary/structure_discovery.cpp')},
        'roundtrip_scope':'offline_process_start_existing_dsp_and_structure_JSON_not_live_adapter',
        'roundtrip_ms':{'median':timings[len(timings)//2],'p95':timings[math.ceil(.95*len(timings))-1],'max':max(timings)},
        'category_counts':{k:dict(v) for k,v in counts.items()},'records':records}
    (output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k!='records'},indent=2))


if __name__=='__main__':main()
