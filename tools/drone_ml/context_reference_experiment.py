"""Paired context-length/quantile ablation with no recognition or live changes."""
import argparse
from dataclasses import asdict,replace
from pathlib import Path
import time

import numpy as np

from .boundary_experiment import metrics
from .common import new_output,sha256,stamp,write_json
from .localizer_audit import load
from .localizer_comparison_experiment import cases
from .localizer_reference_study import duty_fixture,LOWER_TAIL,one_to_one_matches
from .localizer_synthetic import noise
from .rf_boundary_refinement import refine
from .rf_context_reference import ContextConfig,localize as context_localize
from .rf_localizer_multiscale import localize as parent_localize
from .temporal_audit import check_record,reader


def periodic_fixture(seed,duty,phase,seconds=.04):
    rate=20_000_000;rng=np.random.default_rng(seed);n=round(seconds*rate)
    x=noise(rng,n);truth=[]
    for k in range(-1,round(seconds/.01)+1):
        a=max(0,round((phase+k*.01)*rate));b=min(n,round((phase+k*.01+.01*duty)*rate))
        if b<=a:continue
        freq=np.fft.fftfreq(b-a,1/rate)
        x[a:b]+=np.fft.ifft(np.fft.fft(noise(rng,b-a))*((freq>=2e6)&(freq<6e6)))*10**.5
        truth.append(dict(start=a,count=b-a,low_hz=2e6,high_hz=6e6))
    return x.astype('<c8'),truth


def combine(parent,core,context):
    # This measures availability in complementary paths, not a production
    # scheduler or deduplicated event stream. Each proposal gets a unique ID.
    regions=[dict(p,proposal_path=name) for name,result in [('parent',parent),('core',core),('context',context)]
             for p in result['regions']]
    for i,r in enumerate(regions):r['id']=i
    return dict(regions=regions,analysed_samples=context['analysed_samples'])


def experiment(output,reports=(),seeds=(907,919,929,937,947)):
    reports=list(map(Path,reports));before={p:stamp(p) for p in reports};parents=[]
    for path in reports:
        d=load(path)
        if d.get('schema')!='rfmon.drone_ml.localizer_audit.v1' or d.get('algorithm')!='multiscale_lower_tail':
            raise ValueError('requires frozen lower-tail audit')
        for r in d['records']:check_record(r['source']);parents.append(r)
    if len(parents)>32:raise ValueError('source budget')
    out=new_output(output,[p.parent for p in reports]+[Path(r['source']['path']).parent for r in parents])
    configs={f'{round(ms*1000)}ms_q{round(q*100)}':replace(ContextConfig(),block_seconds=ms,reference_quantile=q)
             for ms in (.01,.02) for q in (.2,.05)}
    code=[Path(__file__).with_name(p) for p in ('context_reference_experiment.py','rf_context_reference.py',
        'rf_boundary_refinement.py','rf_localizer_multiscale.py','rf_localizer.py','localizer_synthetic.py',
        'localizer_comparison_experiment.py','localizer_reference_study.py','localizer_audit.py','boundary_experiment.py')]
    hashes={str(p):sha256(p) for p in code+reports}
    write_json(out/'frozen_settings.json',dict(configurations={k:asdict(v) for k,v in configs.items()},
        seeds=list(seeds),development_seed=7,input_sha256=hashes,
        note='2x2 context/quantile ablation; integration and thresholds fixed. No tuning on these seeds or real results.'))
    rows=[]
    for seed in seeds:
        def fixtures():
            yield from cases(seed)
            for duty in (.3,.7,.9):
                x,g=duty_fixture(seed,duty);yield f'duty_{round(duty*100)}pct_10db',x,20_000_000,g
            for name,duty,phase in [('duty98_10db',.98,.0001),('duty90_shifted_10db',.9,.0037)]:
                x,g=periodic_fixture(seed,duty,phase);yield name,x,20_000_000,g
        for name,x,rate,truth in fixtures():
            get=lambda s,n:x[s:s+n];parent=parent_localize(get,len(x),rate,len(x)/rate,LOWER_TAIL)
            core=refine(get,parent);variants={}
            for tag,cfg in configs.items():
                start=time.perf_counter()
                result=context_localize(get,len(x),rate,len(x)/rate,segments=[(0,len(x))],continuity='synthetic_contiguous',config=cfg)
                combined=combine(parent,core,result)
                variants[tag]=dict(context=metrics(result,truth,rate),combined=metrics(combined,truth,rate),
                    matches=one_to_one_matches(result['regions'],truth),
                    combined_matches=one_to_one_matches(combined['regions'],truth),
                    common_mode_ambiguous=sum(p['broad_common_mode_ambiguity'] for p in result['regions']),
                    dropped_regions=result['dropped_regions'],context_wall_seconds=time.perf_counter()-start)
            rows.append(dict(seed=seed,case=name,truth=truth,parent=metrics(parent,truth,rate),core=metrics(core,truth,rate),variants=variants))
        write_json(out/'progress.json',dict(phase='synthetic',completed_seed=seed))
    summary={}
    for name in sorted({r['case'] for r in rows}):
        selected=[r for r in rows if r['case']==name]
        fields=('regions','references','one_to_one_iou50','area80')
        summary[name]={key:{f:sum(r[key][f] for r in selected) for f in fields} for key in ('parent','core')}
        for tag in configs:
            summary[name][tag]={kind:{f:sum(r['variants'][tag][kind][f] for r in selected) for f in fields}
                                for kind in ('context','combined')}
    write_json(out/'synthetic.json',dict(rows=rows,summary=summary,drone_false_alarm_rate=None))
    real=[];source_hashes={}
    for index,parent in enumerate(parents):
        source=parent['source'];path=Path(source['path'])
        if path not in source_hashes:before[path]=stamp(path);source_hashes[path]=sha256(path)
        if source_hashes[path]!=source['source_sha256']:raise ValueError('source hash mismatch')
        with reader(source) as (available,get):
            limit=parent['analysed_samples']
            if available<limit:raise ValueError('source shorter than parent audit')
            variants={}
            for tag,cfg in configs.items():
                start=time.perf_counter()
                r=context_localize(get,int(limit),source['sample_rate_hz'],.1,segments=[(0,int(limit))],
                    continuity='sample_index_only',config=cfg)
                r['context_wall_seconds']=time.perf_counter()-start;variants[tag]=r
        real.append(dict(source=source,parent_regions=parent['regions'],variants=variants,
            continuity_evidence=source.get('continuity'),wall_clock_continuity_verified=False))
        write_json(out/'progress.json',dict(phase='real',completed_records=index+1,total_records=len(parents)))
    if any(stamp(p)!=s for p,s in before.items()):raise ValueError('source changed during run')
    if any(sha256(p)!=h for p,h in hashes.items()):raise ValueError('frozen code/input changed')
    write_json(out/'real.json',dict(records=real,source_hashes_verified=True,source_stats_preserved=True,
        final_test_iq_read=False,training_performed=False,live_changes=False))
    write_json(out/'progress.json',dict(phase='complete',synthetic_cases=len(rows),real_records=len(real)))
    return summary


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--reports',type=Path,nargs='*',default=[]);a=p.parse_args();experiment(a.output,a.reports)


if __name__=='__main__':main()
