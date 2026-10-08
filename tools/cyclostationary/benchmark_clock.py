#!/usr/bin/env python3
"""Bounded timing-grid development controls; no clock calibration or drone truth."""
import argparse
from collections import Counter,defaultdict
import itertools
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import benchmark_bursts as bursts
import benchmark_structure as structure
import benchmark_waveforms as breadth
import benchmark_review as review
ROOT=Path(__file__).resolve().parents[2]


def additions():
    for seed in range(3):
        for kind in ('cp96_24','barker8','barker16'):
            base=structure.cp_geometry(10102231+seed,96,24) if kind=='cp96_24' else breadth.states(10102271+seed,'dsss',period=8 if kind=='barker8' else 16,carrier=.35e6)
            for ppm in (-2500,-2000,-500,0,500,2000,2500):
                for n in (8192,32768):
                    local=review.resample(base[:n],1+ppm*1e-6)
                    for scope in ('whole','burst'):
                        yield f'timing_{kind}_{seed}_{ppm}_{n}_{scope}',kind+('_off_grid' if abs(ppm)==2500 else '_grid_control'),f'fresh_timing:{kind}:{seed}',bursts.gate(local) if scope=='burst' else local,20_000_000,False,ppm
    for seed in range(16):
        base=breadth.noise(10102431+seed,65536)
        for rho in (0,.975):
            prev=0j;x=[]
            for z in base:prev=rho*prev+z;x.append(prev)
            for n in (4096,32768):
                for rate in (20_000_000,200_000_000):
                    for scope in ('whole','burst'):
                        yield f'timing_null_{seed}_{rho}_{n}_{rate}_{scope}','fresh_white_null' if rho==0 else 'fresh_colored_null',f'fresh_timing_gaussian:{seed}',bursts.gate(x[:n]) if scope=='burst' else x[:n],rate,True,None
    for kind in ('cp96_24','barker8'):
        base=structure.cp_geometry(10102631,96,24) if kind=='cp96_24' else breadth.states(10102671,'dsss',period=8,carrier=.35e6)
        local=review.resample(base,1.002);held=breadth.noise(10102731,16384)
        yield kind+'_discovery_only','discovery_only_control',f'fresh_timing:{kind}:discovery_only',bursts.gate(local[:16384]+held),20_000_000,False,2000


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--tool',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--prior-bursts',type=Path,required=True);args=p.parse_args();tool=args.tool.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    prior={r['id']:r for r in json.loads(args.prior_bursts.read_text())['records']};records=[]
    def evaluate(name,category,group,x,rate,null,ppm=None,width=None,path=None,historic=False):
        if path is None:path=control;path.write_bytes(b''.join(struct.pack('<ff',z.real,z.imag) for z in x))
        sha=structure.digest(path);cmd=[str(tool),'analyze','--input',str(path),'--format','cf32_le','--sample-rate',str(rate),
            '--samples',str(min(65536,path.stat().st_size//8)),'--frames','256','--assess-evidence','--review-waveforms','--analyze-bursts','--usable-bandwidth',str(width or rate)]
        old=json.loads(subprocess.run(cmd,check=True,capture_output=True,text=True,timeout=20).stdout)
        full=json.loads(subprocess.run(cmd+['--refine-clock'],check=True,capture_output=True,text=True,timeout=20).stdout)
        assert all(full[k]==v for k,v in old.items()) and structure.digest(path)==sha,name
        if historic:
            previous=prior[name];assert previous['input_sha256']==sha and previous['burst_analysis']==full['burst_analysis'],name
        r=full['clock_refinement'];assert r['grids_tested']<=9 and r['cp_trials']<=18 and r['code_trials']<=18
        assert r['interpolated_samples']<=11*r['partition_samples'] and len(r['cp_candidates'])<=1 and len(r['code_candidates'])<=1
        assert not r['holdout_selects_grid_or_phase'] and not r['clock_grid_is_calibrated_clock_estimate'] and not r['identity_accepted']
        for c in r['cp_candidates']+r['code_candidates']:
            assert c['discovery_last_input_coordinate']<c['holdout_first_input_coordinate'] and c['holdout_last_input_coordinate']+1<r['samples_examined']
        quality=r['scope']=='selected_contiguous_burst'
        raw=full['burst_analysis']['structure_discovery'] if quality else full['structure_discovery']
        raw_cp=any(c['pattern_consistent'] for c in raw['ofdm_candidates']);raw_code=any(c['pattern_consistent'] for c in raw['short_code_candidates'])
        cp=any(c['pattern_consistent'] for c in r['cp_candidates']);code=any(c['pattern_consistent'] for c in r['code_candidates'])
        assert full['classification']['link_family']=='unknown' and full['classification']['model_status']=='not_trained'
        records.append({'id':name,'category':category,'source_group':group,'gaussian_control':null,'input_warp_ppm':ppm,'sample_rate_hz':rate,
            'input_sha256':sha,'legacy_fields_preserved':True,'historical_burst_preserved':historic,'raw_cp_match':raw_cp,'raw_code_match':raw_code,
            'refined_cp_match':cp,'refined_code_match':code,'clock_refinement':r,'classification':full['classification']})
        with (out/'records.jsonl').open('a') as f:f.write(json.dumps(records[-1])+'\n')
        if len(records)%50==0:print(f'{len(records)} timing/control cases checked',flush=True)
    with tempfile.TemporaryDirectory(prefix='rfmon-clock-benchmark-') as tmp:
        control=Path(tmp)/'control.cf32'
        for name,category,group,x,rate,null,ppm in additions():evaluate(name,category,group,x,rate,null,ppm)
        for name,category,group,x,rate,null in bursts.cases():evaluate(name,'historical_'+category,group,x,rate,null,historic=True)
        for name,category,group,x,rate,width,null in itertools.islice(review.cases(500000),128):evaluate(name,'historical_'+category,group,x,rate,False,width=width,historic=True)
        for path in sorted((ROOT/'tests/fixtures/wifi_ofdm').glob('*.cf32')):evaluate(path.stem,'received_wifi_confusable','x310:2026-09-19:received-wifi',None,20_000_000,False,path=path,historic=True)
    counts=defaultdict(Counter);nullgroups={}
    for r in records:
        c=counts[r['category']];c['cases']+=1
        for key in ('raw_cp_match','raw_code_match','refined_cp_match','refined_code_match'):c[key+'_cases']+=r[key]
        c['new_cp_cases']+=r['refined_cp_match'] and not r['raw_cp_match'];c['new_code_cases']+=r['refined_code_match'] and not r['raw_code_match']
        if r['gaussian_control']:
            g=nullgroups.setdefault(r['source_group'],[False,False]);g[0]|=r['refined_cp_match'];g[1]|=r['refined_code_match']
    summary={'schema':'rfmon.cyclo.clock_benchmark.v1','method':'discovery_selected_timing_grid_v1','radio_opened':False,'model_trained':False,
        'rules_frozen_before_run':True,'rules_tuned_on_run':False,'clock_or_receiver_accuracy_calibrated':False,'named_family_acceptance_enabled':False,
        'cases':len(records),'source_groups':len({r['source_group'] for r in records}),'legacy_preservation_cases':len(records),
        'historical_burst_preservation_cases':sum(r['historical_burst_preserved'] for r in records),'gaussian_source_groups':len(nullgroups),
        'gaussian_refined_cp_groups':sum(g[0] for g in nullgroups.values()),'gaussian_refined_code_groups':sum(g[1] for g in nullgroups.values()),
        'control_scope':'software Gaussian seeds; gating/colour/rate/length derivatives grouped, not receiver or drone event truth',
        'category_counts':{k:dict(v) for k,v in counts.items()},'tool_sha256':structure.digest(tool),'prior_burst_report_sha256':structure.digest(args.prior_bursts),
        'source_sha256':{str(p.relative_to(ROOT)):structure.digest(p) for p in (Path(__file__),ROOT/'src/cyclostationary/structure_discovery.cpp',ROOT/'src/cyclostationary/clock_refinement.hpp')},'records':records}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({k:v for k,v in summary.items() if k not in ('records','source_sha256')},indent=2))


if __name__=='__main__':main()
