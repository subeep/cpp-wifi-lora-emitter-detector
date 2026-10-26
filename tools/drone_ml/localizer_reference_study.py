"""Synthetic duty-cycle ablation of median versus lower-tail references.

Includes one-to-one matching so a whole-record rectangle cannot count as
multiple precisely localized bursts. No source labels or model training.
"""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import time

import numpy as np
from scipy.optimize import linear_sum_assignment

from .common import new_output, sha256, write_json
from .localizer_audit import score_regions
from .localizer_comparison_experiment import cases
from .localizer_synthetic import noise
from .rf_localizer import LocalizerConfig, localize as baseline
from .rf_localizer_multiscale import MultiscaleConfig, localize


LOWER_TAIL = MultiscaleConfig(reference_method='lower_tail',fast_high_db=10.,fast_low_db=6.)


def duty_fixture(seed,duty,rate=20_000_000):
    if not .05<=duty<=.9:
        raise ValueError('unsupported fixture duty cycle')
    rng=np.random.default_rng(seed);n=round(rate*.02);x=noise(rng,n);truth=[]
    for start_seconds in (0.,.01):
        a=round((start_seconds+.0005)*rate);b=round((start_seconds+.0005+.01*duty)*rate)
        frequency=np.fft.fftfreq(b-a,1/rate)
        x[a:b]+=np.fft.ifft(np.fft.fft(noise(rng,b-a))*((frequency>=2e6)&(frequency<6e6)))*10**.5
        truth.append(dict(start=a,count=b-a,low_hz=2e6,high_hz=6e6))
    return x.astype('<c8'),truth


def one_to_one_matches(regions,truth,threshold=.5):
    if not regions or not truth:
        return []
    scores=np.zeros((len(truth),len(regions)))
    for i,g in enumerate(truth):
        for j,r in enumerate(regions):
            overlap=max(0,min(g['start']+g['count'],r['stop_sample'])-max(g['start'],r['start_sample']))*max(
                0,min(g['high_hz'],r['high_hz'])-max(g['low_hz'],r['low_hz']))
            total=g['count']*(g['high_hz']-g['low_hz'])+(r['stop_sample']-r['start_sample'])*(r['high_hz']-r['low_hz'])
            scores[i,j]=overlap/(total-overlap)
    # Maximize eligible match count first, IoU second; one proposal per truth.
    benefit=(scores>=threshold).astype(float)*(min(scores.shape)+1)+scores
    left,right=linear_sum_assignment(-benefit)
    return [dict(reference_index=int(i),proposal_id=regions[j]['id'],iou=float(scores[i,j]))
            for i,j in zip(left,right) if scores[i,j]>=threshold]


def experiment(output):
    out=new_output(output,[])
    seeds=[701,709,719,727,733]
    configs=dict(baseline=asdict(LocalizerConfig()),median=asdict(MultiscaleConfig()),lower_tail=asdict(LOWER_TAIL))
    paths=[Path(__file__),Path(__file__).with_name('rf_localizer_multiscale.py'),
           Path(__file__).with_name('rf_localizer.py'),Path(__file__).with_name('localizer_comparison_experiment.py'),
           Path(__file__).with_name('localizer_synthetic.py')]
    hashes={str(p):sha256(p) for p in paths}
    write_json(out/'frozen_settings.json',dict(configs=configs,seeds=seeds,development_seed=7,code_sha256=hashes,
        note='New realizations of known synthetic families plus explicit duty-cycle fixtures; not field validation.'))
    rows=[]
    for seed in seeds:
        def all_cases():
            yield from cases(seed)
            for duty in (.3,.7,.9):
                x,truth=duty_fixture(seed,duty)
                yield f'duty_{round(duty*100)}pct_10db',x,20_000_000,truth
        for kind,x,rate,truth in all_cases():
            for name,cfg,runner in [('baseline',LocalizerConfig(),baseline),('median',MultiscaleConfig(),localize),
                                    ('lower_tail',LOWER_TAIL,localize)]:
                before=time.perf_counter();r=runner(lambda s,n:x[s:s+n],len(x),rate,len(x)/rate,cfg)
                elapsed=time.perf_counter()-before
                scores=[score_regions(r['regions'],g,rate) for g in truth]
                pairs=one_to_one_matches(r['regions'],truth)
                unmatched=sum(not any(p['start_sample']<g['start']+g['count'] and p['stop_sample']>g['start']
                    and p['low_hz']<g['high_hz'] and p['high_hz']>g['low_hz'] for g in truth) for p in r['regions'])
                rows.append(dict(seed=seed,case=kind,algorithm=name,reference_count=len(truth),regions=len(r['regions']),
                    reference_area_80pct=sum(s is not None and s['reference_area_coverage']>=.8 for s in scores),
                    one_to_one_iou_50pct=len(pairs),pairs=pairs,coverage_matches=scores,unmatched_regions=unmatched,
                    stable_ambiguous_regions=sum(p.get('stable_background_ambiguity',False) for p in r['regions']),
                    dropped_regions=r['dropped_regions'],wall_seconds=elapsed,observed_seconds=r['analysed_samples']/rate))
        write_json(out/'progress.json',dict(completed_seed=seed,rows=len(rows)))
    summary={}
    for kind in sorted({r['case'] for r in rows}):
        summary[kind]={}
        for name in configs:
            selected=[r for r in rows if r['case']==kind and r['algorithm']==name]
            summary[kind][name]={k:sum(r[k] for r in selected) for k in
                ['reference_count','regions','reference_area_80pct','one_to_one_iou_50pct','unmatched_regions',
                 'stable_ambiguous_regions','dropped_regions','wall_seconds','observed_seconds']}
    if any(sha256(p)!=hashes[str(p)] for p in paths):
        raise ValueError('experiment code changed while running')
    report=dict(schema='rfmon.drone_ml.reference_study.v1',configs=configs,seeds=seeds,code_sha256=hashes,
        rows=rows,summary=summary,drone_false_alarm_rate=None,training_performed=False,
        note='Activity localization only. Duty-cycle fixtures have two equal-power Gaussian band-limited bursts '
             'per recording. Coverage may match many truths to one rectangle; one-to-one IoU explicitly cannot.')
    write_json(out/'report.json',report);write_json(out/'summary.json',summary)
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=experiment(a.output)
    print(json.dumps(r['summary'],indent=2))


if __name__=='__main__':main()
