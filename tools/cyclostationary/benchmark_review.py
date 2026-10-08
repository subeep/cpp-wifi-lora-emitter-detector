#!/usr/bin/env python3
"""Fresh whole-search controls and grouped rejection/ambiguity review. No training.

Intervals describe software-generated null groups only. They do not calibrate
receiver backgrounds, event false alarms or the probability of a drone.
"""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import statistics
import struct
import subprocess
import tempfile
import time
import benchmark_structure as structure
import benchmark_waveforms as breadth
ROOT=Path(__file__).resolve().parents[2]


def interval(k,n):
    # Nominal two-sided 95% Wilson score interval on independent source groups.
    if not n:return None
    z=statistics.NormalDist().inv_cdf(.975);p=k/n;den=1+z*z/n
    center=(p+z*z/(2*n))/den;delta=z*math.sqrt(p*(1-p)/n+z*z/(4*n*n))/den
    return [max(0,center-delta),min(1,center+delta)]


def resample(x,scale):
    y=[]
    for j in range(len(x)):
        pos=j*scale;at=int(pos);fraction=pos-at
        y.append(x[at]*(1-fraction)+x[at+1]*fraction if at+1<len(x) else 0j)
    return y


def cases(seed_offset):
    for name,category,group,x,width in structure.cases():
        yield name,category,group,x,breadth.RATE,width,False
    # Same base seed across rate/duration derivatives: one conservative group.
    for kind in ('white','colored'):
        for seed in range(64):
            x=breadth.noise(915017+seed_offset+seed+(1000 if kind=='colored' else 0),65536)
            if kind=='colored':
                previous=0j;y=[]
                for z in x:previous=.975*previous+z;y.append(previous)
                x=y
            for n in (4096,65536):
                for rate in (20_000_000,200_000_000):
                    yield f'fresh_{kind}_{seed}_{n}_{rate}',f'fresh_{kind}_null',f'fresh:{kind}:{seed+seed_offset}',x[:n],rate,rate,True
    for kind in ('cp','barker'):
        x=structure.cp_geometry(91031,96,24) if kind=='cp' else breadth.states(91411,'dsss',period=8)
        for ppm in (50,500,2000):
            yield f'{kind}_clock_{ppm}',kind+'_clock_impairment',f'impairment:{kind}',resample(x,1+ppm*1e-6),breadth.RATE,breadth.RATE,False
        for delay in (3,17,33):
            yield f'{kind}_echo_{delay}',kind+'_echo_impairment',f'impairment:{kind}',[z+(.5*x[j-delay] if j>=delay else 0) for j,z in enumerate(x)],breadth.RATE,breadth.RATE,False
        yield kind+'_dc',kind+'_dc_impairment',f'impairment:{kind}',[z+2+2j for z in x],breadth.RATE,breadth.RATE,False
        yield kind+'_limited',kind+'_float_limited_impairment',f'impairment:{kind}',[complex(max(-.5,min(.5,z.real)),max(-.5,min(.5,z.imag))) for z in x],breadth.RATE,breadth.RATE,False
    for seed in range(8):
        x=breadth.noise(821317+seed)
        # Pulsed noise has a real envelope cycle: deliberately a confusable,
        # not a stationary-noise null or a positive modulation reference.
        yield f'pulsed_noise_{seed}','pulsed_noise_confusable',f'pulsed:{seed}',[z if j%512<128 else .01*z for j,z in enumerate(x)],breadth.RATE,breadth.RATE,False


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--prior-structure',type=Path,required=True)
    p.add_argument('--fresh-seed-offset',type=int,default=0)
    args=p.parse_args();tool=args.tool.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    prior={r['id']:r for r in json.loads(args.prior_structure.read_text())['records']};records=[]
    def evaluate(name,category,group,path,rate,width,null):
        checksum=structure.digest(path)
        command=[str(tool),'analyze','--input',str(path),'--format','cf32_le','--sample-rate',str(rate),
            '--samples',str(min(65536,path.stat().st_size//8)),'--frames','256','--assess-evidence']
        if width:command+=['--usable-bandwidth',str(width)]
        baseline=json.loads(subprocess.run(command,capture_output=True,text=True,check=True,timeout=20).stdout)
        start=time.monotonic()
        full=json.loads(subprocess.run(command+['--review-waveforms'],capture_output=True,text=True,check=True,timeout=20).stdout)
        elapsed=(time.monotonic()-start)*1000
        assert all(full[k]==v for k,v in baseline.items()),name
        assert checksum==structure.digest(path),name
        review=full['waveform_review'];s=full['structure_discovery']
        assert not review['classification_used'] and not review['thresholds_calibrated']
        assert len(review['candidates'])<=18 and s['timing_hypotheses']<=32 and s['spread_hypotheses']<=32
        historic=name in prior
        if historic:assert prior[name]['input_sha256']==checksum and prior[name]['structure_discovery']==s,name
        cp=any(c['kind']=='cp_timing' and c['pattern_consistent'] for c in review['candidates'])
        code=any(c['kind']=='barker_11' and c['pattern_consistent'] for c in review['candidates'])
        assert review['cp_code_ambiguous']==(cp and code)
        if cp and code:
            assert all(c['status']!='experimental_pattern' for c in review['candidates'] if c['kind'] in ('cp_timing','barker_11') and c['pattern_consistent'])
        records.append({'id':name,'category':category,'source_group':group,'software_stationary_null':null,
            'sample_rate_hz':rate,'samples':full['input']['samples_read'],'input_sha256':checksum,
            'legacy_fields_preserved':True,'historical_structure_preserved':historic,
            'raw_pattern_count':sum(c['pattern_consistent'] for c in review['candidates']),
            'experimental_review_pattern_count':sum(c['status']=='experimental_pattern' for c in review['candidates']),
            'roundtrip_ms':elapsed,'waveform_review':review,'classification':full['classification']})
        if len(records)%100==0:print(f'{len(records)} cases checked',flush=True)
    with tempfile.TemporaryDirectory(prefix='rfmon-review-benchmark-') as directory:
        path=Path(directory)/'control.cf32'
        for name,category,group,x,rate,width,null in cases(args.fresh_seed_offset):
            path.write_bytes(b''.join(struct.pack('<ff',z.real,z.imag) for z in x))
            evaluate(name,category,group,path,rate,width,null)
    for path in sorted((ROOT/'tests/fixtures/wifi_ofdm').glob('*.cf32')):
        evaluate(path.stem,'received_wifi_confusable','x310:2026-09-19:received-wifi',path,breadth.RATE,breadth.RATE,False)
    counts=defaultdict(Counter);profiles=defaultdict(dict);null_groups={};reviewed_null_groups={}
    for r in records:
        c=counts[r['category']];c['cases']+=1;c['raw_pattern_cases']+=r['raw_pattern_count']>0
        c['cp_code_ambiguous_cases']+=r['waveform_review']['cp_code_ambiguous']
        c['experimental_review_pattern_cases']+=r['experimental_review_pattern_count']>0
        for candidate in r['waveform_review']['candidates']:
            if candidate['pattern_consistent']:c[candidate['status']+'_candidate_count']+=1
        if r['software_stationary_null']:
            key=f"{r['category']}:{r['samples']}:{r['sample_rate_hz']}"
            profiles[key][r['source_group']]=r['raw_pattern_count']>0
            null_groups[r['source_group']]=null_groups.get(r['source_group'],False) or r['raw_pattern_count']>0
            reviewed_null_groups[r['source_group']]=reviewed_null_groups.get(r['source_group'],False) or r['experimental_review_pattern_count']>0
    rate=lambda groups:{'independent_generator_groups':len(groups),'whole_search_pattern_groups':sum(groups.values()),
        'nominal_binomial_wilson_95_interval':interval(sum(groups.values()),len(groups))}
    summary={'schema':'rfmon.cyclo.review_benchmark.v1','method':'expanded_measurement_review_v2',
        'software_development_evaluation':True,'rules_frozen_before_run':True,'rules_tuned_on_run':False,
        'radio_opened':False,'model_trained':False,'remote_id_decoded':False,'independent_real_unit_session_test':False,
        'receiver_or_drone_probability_calibrated':False,'named_family_acceptance_enabled':False,
        'cases':len(records),'source_groups':len({r['source_group'] for r in records}),
        'legacy_preservation_cases':len(records),'historical_structure_preservation_cases':sum(r['historical_structure_preserved'] for r in records),
        'fresh_seed_offset':args.fresh_seed_offset,'stationary_null_group_maxima':rate(null_groups),
        'reviewed_structure_null_group_maxima':rate(reviewed_null_groups),'stationary_null_profiles':{key:rate(value) for key,value in profiles.items()},
        'interval_scope':'nominal_binomial_score_on_independent_software_seed_groups_not_receiver_field_or_drone_accuracy',
        'category_counts':{key:dict(value) for key,value in counts.items()},
        'tool_sha256':structure.digest(tool),'source_sha256':{str(path.relative_to(ROOT)):structure.digest(path) for path in (Path(__file__),ROOT/'src/cyclostationary/waveform_review.cpp',ROOT/'src/cyclostationary/structure_discovery.cpp',ROOT/'src/cyclostationary/waveform_features.cpp')},
        'records':records}
    (output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({key:value for key,value in summary.items() if key not in ('records','source_sha256')},indent=2))


if __name__=='__main__':main()
