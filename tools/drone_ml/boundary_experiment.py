"""Reproducible offline boundary study, preserving original context proposals."""
import argparse
from collections import Counter
from dataclasses import asdict
from pathlib import Path
import time

from .common import new_output, sha256, stamp, write_json
from .localizer_audit import load, score_regions
from .localizer_comparison_experiment import cases
from .localizer_reference_study import LOWER_TAIL, duty_fixture, one_to_one_matches
from .rf_localizer_multiscale import localize
from .rf_boundary_refinement import BoundaryConfig, refine
from .temporal_audit import check_record, reader


def box_union(regions,samples,rate):
    times=sorted({p[k] for p in regions for k in ('start_sample','stop_sample')});area=0.
    for a,b in zip(times,times[1:]):
        spans=sorted((p['low_hz'],p['high_hz']) for p in regions if p['start_sample']<b and p['stop_sample']>a)
        total=0.;end=float('-inf')
        for lo,hi in spans:total+=max(0,hi-max(lo,end));end=max(end,hi)
        area+=(b-a)*total
    return area/(samples*rate)


def metrics(result,truth,rate):
    regions=result['regions'];matches=[score_regions(regions,g,rate) for g in truth]
    return dict(regions=len(regions),references=len(truth),
        one_to_one_iou50=len(one_to_one_matches(regions,truth)),
        area80=sum(s is not None and s['reference_area_coverage']>=.8 for s in matches),
        box_union_fraction=box_union(regions,result['analysed_samples'],rate))


def experiment(output,reports=(),seeds=(809,811,821,823,827)):
    reports=list(map(Path,reports));before={p:stamp(p) for p in reports}
    parents=[]
    for path in reports:
        d=load(path)
        if d.get('schema')!='rfmon.drone_ml.localizer_audit.v1' or d.get('algorithm')!='multiscale_lower_tail':
            raise ValueError('requires frozen lower-tail parent audit')
        for r in d['records']:
            check_record(r['source']);parents.append(r)
    if len(parents)>32:raise ValueError('source budget')
    out=new_output(output,[p.parent for p in reports]+[Path(r['source']['path']).parent for r in parents])
    cfg=BoundaryConfig()
    paths=[Path(__file__),Path(__file__).with_name('rf_boundary_refinement.py'),
        Path(__file__).with_name('rf_localizer_multiscale.py'),Path(__file__).with_name('rf_localizer.py'),
        Path(__file__).with_name('localizer_reference_study.py'),Path(__file__).with_name('localizer_synthetic.py'),
        Path(__file__).with_name('localizer_comparison_experiment.py'),Path(__file__).with_name('localizer_audit.py')]
    hashes={str(p):sha256(p) for p in paths+reports}
    write_json(out/'frozen_settings.json',dict(config=asdict(cfg),parent_config=asdict(LOWER_TAIL),seeds=list(seeds),
        development_seed=7,input_sha256=hashes,notes='Fresh draws of known synthetic families, not independent field validation.'))
    rows=[]
    for seed in seeds:
        def fixtures():
            yield from cases(seed)
            for duty in (.3,.7,.9):
                x,g=duty_fixture(seed,duty);yield f'duty_{round(duty*100)}pct_10db',x,20_000_000,g
        for name,x,rate,truth in fixtures():
            get=lambda s,n:x[s:s+n]
            parent=localize(get,len(x),rate,len(x)/rate,LOWER_TAIL)
            start=time.perf_counter();r=refine(get,parent,cfg);elapsed=time.perf_counter()-start
            rows.append(dict(seed=seed,case=name,parent=metrics(parent,truth,rate),core=metrics(r,truth,rate),
                review_statuses=dict(Counter(p['status'] for p in r['parent_reviews'])),
                refinement_seconds=elapsed,read_samples=r['read_samples'],core_budget_drops=r['core_budget_drops']))
        write_json(out/'progress.json',dict(phase='synthetic',completed_seed=seed))
    summary={}
    for name in sorted({r['case'] for r in rows}):
        group=[r for r in rows if r['case']==name]
        summary[name]={kind:{key:sum(r[kind][key] for r in group) for key in ('regions','references','one_to_one_iou50','area80')}
                       for kind in ('parent','core')}
    write_json(out/'synthetic.json',dict(rows=rows,summary=summary,drone_false_alarm_rate=None))
    real=[];source_hashes={}
    for index,parent in enumerate(parents):
        source=parent['source'];path=Path(source['path'])
        if path not in source_hashes:before[path]=stamp(path);source_hashes[path]=sha256(path)
        if source_hashes[path]!=source['source_sha256']:raise ValueError('source hash mismatch')
        start=time.perf_counter()
        with reader(source) as (available,get):
            if available<parent['analysed_samples']:raise ValueError('short parent source')
            r=refine(get,parent,cfg)
        r.update(source=source,refinement_wall_seconds=time.perf_counter()-start,
            parent_box_union=box_union(parent['regions'],parent['analysed_samples'],source['sample_rate_hz']),
            core_box_union=box_union(r['regions'],r['analysed_samples'],source['sample_rate_hz']))
        real.append(r);write_json(out/'progress.json',dict(phase='real',completed_records=index+1,total_records=len(parents)))
    if any(stamp(p)!=s for p,s in before.items()):raise ValueError('source/input changed')
    if any(sha256(p)!=h for p,h in hashes.items()):raise ValueError('frozen code/input changed')
    write_json(out/'real.json',dict(schema='rfmon.drone_ml.boundary_audit.v1',records=real,
        source_stats_preserved=True,source_hashes_verified=True,final_test_iq_read=False,
        training_performed=False,live_changes=False))
    write_json(out/'progress.json',dict(phase='complete',synthetic_rows=len(rows),real_records=len(real)))
    return summary


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--reports',type=Path,nargs='*',default=[]);a=p.parse_args();experiment(a.output,a.reports)


if __name__=='__main__':main()
