#!/usr/bin/env python3
"""Contiguous-burst development controls and additive-output checks; no drone truth."""
import argparse
import cmath
from collections import Counter, defaultdict
import itertools
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import benchmark_waveforms as breadth
import benchmark_structure as structure
import benchmark_review as review
import benchmark_sweeps as sweeps
ROOT=Path(__file__).resolve().parents[2]


def gate(x, offset=16384, n=65536):
    assert offset+len(x)<=n
    return [0j]*offset+x+[0j]*(n-offset-len(x))


def cases():
    for seed in range(3):
        noise=breadth.noise(10100831+seed)
        rng=random.Random(10100871+seed)
        sources={
            'cp96_24':structure.cp_geometry(10100931+seed,96,24),
            'barker8':breadth.states(10100971+seed,'dsss',period=8),
            'linear_sweep':sweeps.sweep(10101031+seed,512,.4,1),
            'conjugate_cycle':[rng.choice((-1,1))*cmath.exp(1j*math.pi*j/128) for j in range(32768)],
            'envelope_cycle':[math.sqrt(1+.9*math.cos(2*math.pi*j/128))*z for j,z in enumerate(noise)],
            'two_frequency':breadth.states(10101071+seed,'fsk')}
        for kind,base in sources.items():
            group=f'analytic:{kind}:{seed}'
            for length in (4096,8192,32768):
                for variant in ('clean','noise20','clock2000'):
                    local=base[:length]
                    if variant=='noise20':local=[z+.070710678*n for z,n in zip(local,noise)]
                    if variant=='clock2000':local=review.resample(local,1.002)
                    yield f'{kind}_{seed}_{length}_{variant}',kind+'_'+variant,group,gate(local),20_000_000,False
    # Derivatives share one source group per Gaussian seed and noise kind.
    # High-energy gated noise is nonstationary activity; only its interior is
    # a stationary Gaussian null. Energy selection itself remains uncalibrated.
    for seed in range(32):
        base=breadth.noise(10101231+seed,32768)
        for rho in (0,.975):
            previous=0j;x=[]
            for z in base:previous=rho*previous+z;x.append(previous)
            for length in (4096,8192,32768):
                for rate in (20_000_000,200_000_000):
                    yield f'gated_noise_{seed}_{rho}_{length}_{rate}','gated_white_noise' if rho==0 else 'gated_colored_noise',f'gated_gaussian_seed:{seed}',gate(x[:length]),rate,True
    for length in (512,1024,2048):
        yield f'short_{length}','short_support_control',f'short:{length}',gate([cmath.exp(2j*math.pi*j/128) for j in range(length)]),20_000_000,False
    for carrier in (.05,.15,.35):
        yield f'tone_{carrier}','tone_confusable','analytic_tones',gate([cmath.exp(2j*math.pi*carrier*j) for j in range(8192)]),20_000_000,False
    x=sources['conjugate_cycle'][:8192]
    yield 'disjoint_short_cycles','disjoint_short_control','analytic:conjugate_cycle:2',sum(([0j]*1024+x[:1024] for _ in range(16)),[])+[0j]*32768,20_000_000,False
    yield 'cycle_discovery_only','discovery_only_control','analytic:conjugate_cycle:2',gate(x[:4096]+breadth.noise(10101431,4096)),20_000_000,False
    yield 'two_equal_bursts','two_burst_control','analytic:conjugate_cycle:2',[0j]*4096+x+[0j]*8192+x+[0j]*36864,20_000_000,False


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--tool',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--prior-background',type=Path,required=True)
    args=p.parse_args();tool=args.tool.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    prior={r['id']:r for r in json.loads(args.prior_background.read_text())['records']};records=[]
    def evaluate(name,category,group,x,rate,null,path=None,historic=False):
        if path is None:
            path=control;path.write_bytes(b''.join(struct.pack('<ff',z.real,z.imag) for z in x))
        sha=structure.digest(path);cmd=[str(tool),'analyze','--input',str(path),'--format','cf32_le','--sample-rate',str(rate),
            '--samples',str(min(65536,path.stat().st_size//8)),'--frames','256','--assess-evidence','--review-waveforms','--usable-bandwidth',str(rate)]
        # Prior historical calls used category-specific widths.
        if historic and x is not None:cmd[-1]=str(x)
        baseline=json.loads(subprocess.run(cmd,check=True,capture_output=True,text=True,timeout=20).stdout)
        full=json.loads(subprocess.run(cmd+['--analyze-bursts'],check=True,capture_output=True,text=True,timeout=20).stdout)
        assert all(full[k]==v for k,v in baseline.items()) and sha==structure.digest(path),name
        if historic:
            old=prior[name];assert old['input_sha256']==sha and full['cyclic_background']==old['cyclic_background'] and full['cyclic_background_review']==old['cyclic_background_review'],name
        b=full['burst_analysis'];assert b['samples_examined']<=16384 and b['max_regions_per_tile']==1 and not b['phase_concatenation'] and not b['identity_accepted']
        assert full['classification']['link_family']=='unknown' and full['classification']['model_status']=='not_trained'
        c=Counter()
        if b['samples_examined']:
            assert b['source_last_original_sample']-b['source_first_original_sample']+1==b['samples_examined']
            for v in b['waveform_review']['candidates']:c[v['kind']]+=v['pattern_consistent']
            c['background_supported']=sum(v['background_supported'] for v in b['cyclic_background']['peaks'])
            c['linear_sweep']=sum(v['pattern_consistent'] for v in b['linear_sweep_discovery']['candidates'])
        records.append({'id':name,'category':category,'source_group':group,'gated_gaussian_control':null,
            'sample_rate_hz':rate,'input_sha256':sha,'legacy_fields_preserved':True,'historical_background_preserved':historic,
            'burst_analyzed':bool(b['samples_examined']),'local_match_counts':dict(c),
            'whole_tile_background_supported':sum(v['background_supported'] for v in full['cyclic_background']['peaks']),
            'burst_analysis':b,'classification':full['classification']})
        with (out/'records.jsonl').open('a') as f:f.write(json.dumps(records[-1])+'\n')
        if len(records)%50==0:print(f'{len(records)} burst/control cases checked',flush=True)
    with tempfile.TemporaryDirectory(prefix='rfmon-burst-benchmark-') as tmp:
        control=Path(tmp)/'control.cf32'
        for name,category,group,x,rate,null in cases():evaluate(name,category,group,x,rate,null)
        for name,category,group,x,rate,width,null in itertools.islice(review.cases(500000),128):
            control.write_bytes(b''.join(struct.pack('<ff',z.real,z.imag) for z in x))
            evaluate(name,'historical_'+category,group,width,rate,False,path=control,historic=True)
        for path in sorted((ROOT/'tests/fixtures/wifi_ofdm').glob('*.cf32')):
            evaluate(path.stem,'received_wifi_confusable','x310:2026-09-19:received-wifi',None,20_000_000,False,path=path,historic=True)
    counts=defaultdict(Counter);groups={}
    for r in records:
        c=counts[r['category']];c['cases']+=1;c['analyzed_cases']+=r['burst_analyzed']
        for kind in ('cp_timing','barker_11','two_frequency','two_level_envelope','background_supported','linear_sweep'):
            c[kind+'_cases']+=r['local_match_counts'].get(kind,0)>0
        if r['gated_gaussian_control']:
            g=groups.setdefault(r['source_group'],Counter());g['analyzed']|=r['burst_analyzed']
            for kind in ('cp_timing','barker_11','background_supported','linear_sweep'):g[kind]|=r['local_match_counts'].get(kind,0)>0
    summary={'schema':'rfmon.cyclo.burst_benchmark.v1','method':'bounded_contiguous_burst_v1',
        'rules_frozen_before_run':True,'thresholds_tuned_on_run':False,'radio_opened':False,'model_trained':False,
        'named_family_acceptance_enabled':False,'receiver_or_drone_accuracy_calibrated':False,
        'cases':len(records),'source_groups':len({r['source_group'] for r in records}),
        'legacy_preservation_cases':len(records),'historical_background_preservation_cases':sum(r['historical_background_preserved'] for r in records),
        'gated_gaussian_source_groups':len(groups),'gated_gaussian_group_counts':dict(sum((Counter(g) for g in groups.values()),Counter())),
        'control_scope':'gated proper-complex Gaussian interiors; derivatives grouped by seed, not independent events or receiver ground truth',
        'category_counts':{k:dict(v) for k,v in counts.items()},'tool_sha256':structure.digest(tool),
        'prior_background_sha256':structure.digest(args.prior_background),
        'source_sha256':{str(p.relative_to(ROOT)):structure.digest(p) for p in (Path(__file__),ROOT/'src/cyclostationary/burst_analysis.cpp',ROOT/'src/cyclostationary/burst_analysis.hpp')},
        'records':records}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k not in ('records','source_sha256')},indent=2))


if __name__=='__main__':main()
