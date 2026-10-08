#!/usr/bin/env python3
"""General sweep shape controls and unchanged older outputs; no drone ground truth."""
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
import benchmark_structure as structure
import benchmark_waveforms as breadth
ROOT=Path(__file__).resolve().parents[2]


def sweep(seed, span, fraction, direction, n=32768):
    rng=random.Random(seed);origin=rng.randrange(span);carrier=rng.uniform(-.08,.08)
    slope=direction*fraction/(span-1)
    return [cmath.exp(2j*math.pi*(carrier*t+.5*slope*t*t))
            for j in range(n) for t in [float((j+origin)%span)-(span-1)*.5]]


def additions():
    for seed in range(3):
        for span in (128,512,2048):
            for fraction in (.1,.4,.6):
                for direction in (-1,1):
                    x=sweep(940117+seed,span,fraction,direction)
                    name=f'sweep_{seed}_{span}_{fraction}_{direction}';group=f'analytic_sweep:{seed}'
                    yield name,'sweep_clean',group,x,breadth.RATE,breadth.RATE,False
                    noise=breadth.noise(971117+seed,len(x))
                    for snr in (30,15):
                        # Unit-power chirp, complex noise power two.
                        amplitude=math.sqrt(.5*10**(-snr/10))
                        yield name+f'_noise{snr}',f'sweep_noise{snr}',group,[z+amplitude*n for z,n in zip(x,noise)],breadth.RATE,breadth.RATE,False
                    yield name+'_echo3','sweep_echo3',group,[z+(.3*x[j-3] if j>=3 else 0) for j,z in enumerate(x)],breadth.RATE,breadth.RATE,False
        x=sweep(940117+seed,512,.4,1)
        yield f'sweep_{seed}_clock2000','sweep_clock2000',f'analytic_sweep:{seed}',review.resample(x,1.002),breadth.RATE,breadth.RATE,False
        yield f'sweep_{seed}_discovery_only','sweep_discovery_only',f'analytic_sweep:{seed}',x[:16384]+breadth.noise(951117+seed,16384),breadth.RATE,breadth.RATE,True
        reverse=sweep(940117+seed,512,.4,-1)
        yield f'sweep_{seed}_opposite_held','sweep_opposite_held',f'analytic_sweep:{seed}',x[:16384]+reverse[16384:],breadth.RATE,breadth.RATE,True
        # Smooth nonlinear FM has no single correct linear sweep hypothesis.
        nonlinear=[cmath.exp(2j*math.pi*.3*512/(2*math.pi)*math.sin(2*math.pi*j/512)) for j in range(len(x))]
        yield f'nonlinear_fm_{seed}','nonlinear_fm_confusable','analytic_nonlinear_fm',nonlinear,breadth.RATE,breadth.RATE,False


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--prior-review',type=Path,required=True)
    args=p.parse_args();tool=args.tool.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    historic=json.loads(args.prior_review.read_text());prior={r['id']:r for r in historic['records']};records=[]
    def evaluate(name,category,group,path,rate,width,null):
        sha=structure.digest(path)
        command=[str(tool),'analyze','--input',str(path),'--format','cf32_le','--sample-rate',str(rate),
                 '--samples',str(min(65536,path.stat().st_size//8)),'--frames','256','--assess-evidence']
        if width:command+=['--usable-bandwidth',str(width)]
        baseline=json.loads(subprocess.run(command,check=True,capture_output=True,text=True,timeout=20).stdout)
        full=json.loads(subprocess.run(command+['--review-waveforms'],check=True,capture_output=True,text=True,timeout=20).stdout)
        assert all(full[k]==v for k,v in baseline.items()) and structure.digest(path)==sha,name
        if name in prior:
            assert sha==prior[name]['input_sha256'] and full['waveform_review']==prior[name]['waveform_review'],name
        s=full['linear_sweep_discovery'];r=full['linear_sweep_review']
        assert s['discovery_trials']<=510 and s['discovery_continuation_trials']<=2*s['discovery_trials']
        assert len(s['candidates'])<=3 and len(r['candidates'])<=3 and not s['classification_used']
        assert all(c['status']!='experimental_pattern' for c in r['candidates'])
        records.append({'id':name,'category':category,'source_group':group,'sample_rate_hz':rate,
            'input_sha256':sha,'legacy_fields_preserved':True,'historical_review_preserved':name in prior,
            'software_null_or_discovery_only':null,'linear_sweep_discovery':s,'linear_sweep_review':r,'classification':full['classification']})
    with tempfile.TemporaryDirectory(prefix='rfmon-sweep-benchmark-') as tmp:
        path=Path(tmp)/'control.cf32'
        for name,category,group,x,rate,width,null in review.cases(historic['fresh_seed_offset']):
            path.write_bytes(b''.join(struct.pack('<ff',z.real,z.imag) for z in x));evaluate(name,category,group,path,rate,width,null)
        for name,category,group,x,rate,width,null in additions():
            path.write_bytes(b''.join(struct.pack('<ff',z.real,z.imag) for z in x));evaluate(name,category,group,path,rate,width,null)
    for path in sorted((ROOT/'tests/fixtures/wifi_ofdm').glob('*.cf32')):
        evaluate(path.stem,'received_wifi_confusable','x310:2026-09-19:received-wifi',path,breadth.RATE,breadth.RATE,False)
    counts=defaultdict(Counter);nullgroups={}
    for r in records:
        raw=any(p['pattern_consistent'] for p in r['linear_sweep_discovery']['candidates'])
        counts[r['category']]['cases']+=1;counts[r['category']]['shape_matches']+=raw
        if r['software_null_or_discovery_only']:nullgroups[r['source_group']]=nullgroups.get(r['source_group'],False) or raw
    summary={'schema':'rfmon.cyclo.sweep_benchmark.v1','method':records[0]['linear_sweep_discovery']['method'],
        'rules_frozen_before_run':True,'rules_tuned_on_run':False,'radio_opened':False,'model_trained':False,
        'named_family_acceptance_enabled':False,'receiver_or_drone_probability_calibrated':False,
        'cases':len(records),'source_groups':len({r['source_group'] for r in records}),
        'legacy_preservation_cases':len(records),'historical_review_preservation_cases':sum(r['historical_review_preserved'] for r in records),
        'software_null_groups':len(nullgroups),'software_null_shape_groups':sum(nullgroups.values()),
        'null_scope':'Gaussian white/AR coloured software seeds plus discovery-only derivatives; no field calibration',
        'category_counts':{k:dict(v) for k,v in counts.items()},'tool_sha256':structure.digest(tool),
        'source_sha256':{str(path.relative_to(ROOT)):structure.digest(path) for path in (Path(__file__),ROOT/'src/cyclostationary/chirp_discovery.cpp',ROOT/'src/cyclostationary/waveform_review.cpp')},
        'records':records}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k not in ('records','source_sha256')},indent=2))


if __name__=='__main__':main()
