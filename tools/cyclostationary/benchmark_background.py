#!/usr/bin/env python3
"""Local CAF background development controls; no calibrated drone/receiver accuracy."""
import argparse
import cmath
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import benchmark_review as review
import benchmark_sweeps as sweeps
import benchmark_structure as structure
import benchmark_waveforms as breadth
ROOT=Path(__file__).resolve().parents[2]


def additions():
    # Fresh seeds; rate/length/rho derivatives share one conservative group.
    for seed in range(32):
        source=breadth.noise(10060031+seed,65536)
        for rho in (0,.5,.9,.975,.995):
            previous=0j;x=[]
            for z in source:previous=rho*previous+z;x.append(previous)
            for n in (4096,65536):
                for rate in (20_000_000,200_000_000):
                    yield f'background_null_{seed}_{rho}_{n}_{rate}','fresh_stationary_background',f'fresh_noise_seed:{seed}',x[:n],rate,rate,True
    for seed in range(6):
        rng=random.Random(10061331+seed);noise=breadth.noise(10061731+seed,32768)
        for kind in ('periodic_envelope','conjugate_bpsk'):
            alpha=1/128
            x=[math.sqrt(1+.9*math.cos(2*math.pi*alpha*j))*z for j,z in enumerate(noise)] if kind=='periodic_envelope' else [
                rng.choice((-1,1))*cmath.exp(1j*math.pi*alpha*j) for j in range(len(noise))]
            group=f'analytic_cycle_seed:{seed}'
            yield f'caf_{kind}_{seed}',kind+'_clean',group,x,breadth.RATE,breadth.RATE,False
            added_noise=breadth.noise(10063731+seed,len(x))
            noisy=[z+.2*n for z,n in zip(x,added_noise)]
            yield f'caf_{kind}_{seed}_noise',kind+'_noise',group,noisy,breadth.RATE,breadth.RATE,False
            yield f'caf_{kind}_{seed}_discovery_only',kind+'_discovery_only',group,x[:16384]+breadth.noise(10062531+seed,16384),breadth.RATE,breadth.RATE,True
            yield f'caf_{kind}_{seed}_clock2000',kind+'_clock2000',group,review.resample(x,1.002),breadth.RATE,breadth.RATE,False
        # Narrowband real-noise translations can have genuine conjugate cycles.
        # They are confusables, not proper-complex stationary-noise nulls.
        previous=0.;x=[]
        for j,z in enumerate(noise):previous=.975*previous+z.real;x.append(previous*cmath.exp(2j*math.pi*j/128))
        yield f'translated_real_noise_{seed}','translated_real_noise_confusable',f'analytic_cycle_seed:{seed}',x,breadth.RATE,breadth.RATE,False


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--tool',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--prior-sweeps',type=Path,required=True)
    p.add_argument('--prior-review',type=Path,required=True);p.add_argument('--reuse-measurements',type=Path);args=p.parse_args()
    tool=args.tool.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    prior={r['id']:r for r in json.loads(args.prior_sweeps.read_text())['records']}
    historical_review={r['id']:r for r in json.loads(args.prior_review.read_text())['records']};records=[]
    def evaluate(name,category,group,path,rate,width,null):
        sha=structure.digest(path);cmd=[str(tool),'analyze','--input',str(path),'--format','cf32_le','--sample-rate',str(rate),
            '--samples',str(min(65536,path.stat().st_size//8)),'--frames','256','--assess-evidence']
        if width:cmd+=['--usable-bandwidth',str(width)]
        baseline=json.loads(subprocess.run(cmd,check=True,capture_output=True,text=True,timeout=20).stdout)
        full=json.loads(subprocess.run(cmd+['--review-waveforms'],check=True,capture_output=True,text=True,timeout=20).stdout)
        assert sha==structure.digest(path) and all(full[k]==v for k,v in baseline.items()),name
        if name in prior:
            old=prior[name];assert old['input_sha256']==sha and full['linear_sweep_discovery']==old['linear_sweep_discovery'] and full['linear_sweep_review']==old['linear_sweep_review'],name
        if name in historical_review:assert full['waveform_review']==historical_review[name]['waveform_review'],name
        b=full['cyclic_background'];r=full['cyclic_background_review']
        assert len(b['peaks'])<=8 and b['fft_calls']<=16 and not b['selected_rates_or_lags_refitted']
        assert all(p['status']!='experimental_pattern' for p in r['candidates'])
        records.append({'id':name,'category':category,'source_group':group,'sample_rate_hz':rate,'samples':path.stat().st_size//8,
            'input_sha256':sha,'legacy_fields_preserved':True,'historical_sweep_preserved':name in prior,
            'historical_review_preserved':name in historical_review,'software_null_or_discovery_only':null,
            'raw_persistent_count':sum(p['raw_persistent_pattern'] for p in b['peaks']),
            'background_supported_count':sum(p['background_supported'] for p in b['peaks']),
            'cyclic_background':b,'cyclic_background_review':r,'classification':full['classification']})
        with (out/'records.jsonl').open('a') as checkpoint:checkpoint.write(json.dumps(records[-1])+'\n')
    if args.reuse_measurements:
        previous=json.loads(args.reuse_measurements.read_text())
        assert previous['tool_sha256']==structure.digest(tool)
        for name in ('src/cyclostationary/cyclic_background.cpp','src/cyclostationary/waveform_review.cpp'):
            assert previous['source_sha256'][name]==structure.digest(ROOT/name)
        records=previous['records']
    else:
        with tempfile.TemporaryDirectory(prefix='rfmon-background-benchmark-') as tmp:
            path=Path(tmp)/'control.cf32'
            for generator in (review.cases(500000),sweeps.additions(),additions()):
                for name,category,group,x,rate,width,null in generator:
                    path.write_bytes(b''.join(struct.pack('<ff',z.real,z.imag) for z in x));evaluate(name,category,group,path,rate,width,null)
        for path in sorted((ROOT/'tests/fixtures/wifi_ofdm').glob('*.cf32')):
            evaluate(path.stem,'received_wifi_confusable','x310:2026-09-19:received-wifi',path,breadth.RATE,breadth.RATE,False)
    counts=defaultdict(Counter);nullgroups={}
    for r in records:
        c=counts[r['category']];c['cases']+=1;c['raw_match_cases']+=r['raw_persistent_count']>0;c['background_supported_cases']+=r['background_supported_count']>0
        r['software_stationary_null']=r['category'] in ('fresh_white_null','fresh_colored_null','fresh_stationary_background')
        r['discovery_only_cyclic_control']=r['category'] in ('periodic_envelope_discovery_only','conjugate_bpsk_discovery_only')
        # Other-branch negatives (for example opposite chirp slopes) can share
        # genuine cyclic structure and must not inflate stationary-null counts.
        if r['software_stationary_null']:
            pair=nullgroups.setdefault(r['source_group'],[False,False]);pair[0]|=r['raw_persistent_count']>0;pair[1]|=r['background_supported_count']>0
    summary={'schema':'rfmon.cyclo.background_benchmark.v2','method':'local_caf_background_v1','rules_frozen_before_run':True,
        'thresholds_tuned_on_run':False,'radio_opened':False,'model_trained':False,'receiver_or_drone_probability_calibrated':False,
        'named_family_acceptance_enabled':False,'cases':len(records),'source_groups':len({r['source_group'] for r in records}),
        'legacy_preservation_cases':len(records),'historical_sweep_preservation_cases':sum(r['historical_sweep_preserved'] for r in records),
        'historical_review_preservation_cases':sum(r['historical_review_preserved'] for r in records),
        'stationary_null_groups':len(nullgroups),'raw_stationary_null_groups':sum(p[0] for p in nullgroups.values()),'supported_stationary_null_groups':sum(p[1] for p in nullgroups.values()),
        'null_scope':'proper-complex stationary Gaussian AR source groups; rate/length/rho derivatives shared, not receiver/drone accuracy',
        'measurements_reused_without_DSP_rerun':bool(args.reuse_measurements),
        'measurement_source_report_sha256':structure.digest(args.reuse_measurements) if args.reuse_measurements else None,
        'category_counts':{k:dict(v) for k,v in counts.items()},'tool_sha256':structure.digest(tool),
        'source_sha256':{str(path.relative_to(ROOT)):structure.digest(path) for path in (Path(__file__),ROOT/'src/cyclostationary/cyclic_background.cpp',ROOT/'src/cyclostationary/waveform_review.cpp')},
        'records':records}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k not in ('records','source_sha256')},indent=2))


if __name__=='__main__':main()
