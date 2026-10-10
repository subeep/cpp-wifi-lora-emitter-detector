#!/usr/bin/env python3
"""Replay bounded overlapping windows from derived IQ; never open dataset originals.

Continuity is an input claim, not established by a file or matching markers.
No family acceptance, tracking, label feedback, threshold tuning or ML.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
from pathlib import Path
import subprocess
from collections import Counter
from marker_recurrence import assess_recurrence


def offsets(samples,rate,support_seconds=64e-6):
    if not math.isfinite(rate) or not 0<rate<=1e9 or samples<=0 or not math.isfinite(support_seconds) or support_seconds<=0:
        raise ValueError('invalid replay interval or overlap')
    width=65536;stride=width-math.ceil(rate*support_seconds)
    if stride<=0:raise ValueError('marker cannot fit current window budget')
    if samples<=width:return [0]
    return sorted(set([*range(0,samples-width+1,stride),samples-width]))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--manifest',type=Path,required=True)
    p.add_argument('--tool',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--refine-markers',action='store_true',help='digital-filter trial only; does not qualify analog passband')
    p.add_argument('--symbol-context',action='store_true',help='overlap includes 66.67 us span plus 8 us before and 4 us after')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);tool=a.tool.resolve()
    results=[]
    overlap_seconds=1/15000+12e-6 if a.symbol_context else 64e-6
    for record in json.loads(a.manifest.read_text())['records']:
        src=Path(record['path']);rate=record['sample_rate_hz']
        if src.parent!=a.manifest.resolve().parent:raise ValueError('only derived IQ beside manifest is allowed')
        before=hashlib.sha256(src.read_bytes()).hexdigest()
        if before!=record['sha256']:raise ValueError('derived source hash mismatch')
        def run(start):
            cmd=[str(tool),'analyze','--input',str(src),'--format',record['format'],'--sample-rate',str(rate),
                '--center',str(record['capture_center_hz']),'--offset',str(start),'--samples',str(min(65536,record['samples']-start)),
                '--frames','128','--alpha-bins','1','--assess-evidence','--named-link-candidates']
            r=json.loads(subprocess.run(cmd,check=True,capture_output=True,text=True,timeout=30).stdout)
            refinements=[]
            if a.refine_markers:
                proposals=[]
                for i,c in enumerate(r['chirp_structure']['candidates']):
                    if (c['status']=='measured' and c['peak_coherence_squared']>=.8 and
                        c['first_half_coherence_squared']>=.8 and c['second_half_coherence_squared']>=.8 and
                        c['frequency_status']=='measured' and c['phase_unwraps']==0 and
                        abs(c['frequency_slope_hz_per_second']/c['slope_hz_per_second']-1)<=.02):
                        proposals.append((i,c))
                for i,c in sorted(proposals,key=lambda x:x[1]['peak_coherence_squared'],reverse=True)[:2]:
                    center=round(c['frequency_mean_offset_hz']/250000)*250000
                    width=c['nominal_sweep_hz']+2000000
                    digital_span=.9*rate
                    if abs(center)+width/2>=digital_span/2:continue
                    decimation=max(1,min(4,int(.8*rate/width)))
                    extra=['--roi-offset',str(center),'--roi-bandwidth',str(width),
                           '--source-bandwidth',str(digital_span),'--decimate',str(decimation)]
                    filtered=json.loads(subprocess.run(cmd+extra,check=True,capture_output=True,text=True,timeout=30).stdout)
                    refinements.append({'selection_measurement':i,'center_offset_hz':center,'width_hz':width,
                        'digital_span_hz':digital_span,'analog_passband_calibrated':False,'decimation':decimation,
                        'named':filtered['named_link_candidates']['candidates'],
                        'chirps':filtered['chirp_structure']['candidates'],'preprocessing':filtered['preprocessing']})
            return {'offset':start,'named':r['named_link_candidates']['candidates'],
                    'chirps':r['chirp_structure']['candidates'],'quality':r['status'],'refinements':refinements}
        with ThreadPoolExecutor(max_workers=2) as pool:windows=list(pool.map(run,offsets(record['samples'],rate,overlap_seconds)))
        assert before==hashlib.sha256(src.read_bytes()).hexdigest()
        counts=Counter(c['status'] for w in windows for c in w['named'])
        peaks=[max((w['chirps'][i]['peak_coherence_squared'] or 0) for w in windows) for i in range(3)]
        points=[]
        refined_counts=Counter()
        for w in windows:
            for trial,center in [(w,0)]+[(t,t['center_offset_hz']) for t in w['refinements']]:
                for c in trial['named']:
                    if trial is not w:refined_counts[c['status']]+=1
                    if c['status']!='unvalidated_candidate':continue
                    m=trial['chirps'][c['measurement_index']]
                    points.append({'sample':c['first_source_sample_center'],'profile':c['profile'],
                        'frequency_hz':record['capture_center_hz']+center+m['frequency_mean_offset_hz'],
                        'coherence':m['peak_coherence_squared'],'filtered':trial is not w})
        recurrence=assess_recurrence(points,rate,False)
        result={'record':record,'windows':windows,'status_counts':dict(counts),'maximum_coherences':peaks,
                'refined_status_counts':dict(refined_counts),'recurrence':recurrence,
                'whole_derived_interval_window_covered':True,'sources_unchanged':True,'accepted_family':None}
        (a.output/(src.stem+'.json')).write_text(json.dumps(result,indent=2)+'\n')
        results.append({k:v for k,v in result.items() if k!='windows'})
        print(src.name,len(windows),dict(counts),dict(refined_counts),'sequence',len(recurrence['longest_sequence']),flush=True)
    (a.output/'summary.json').write_text(json.dumps({'schema':'contiguous_marker_replay.v1','records':results,
        'tool_sha256':hashlib.sha256(tool.read_bytes()).hexdigest(),'script_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'marker_duration_seconds':64e-6,'overlap_seconds':overlap_seconds,'symbol_context_requested':a.symbol_context,'independent_accuracy_evaluation':False,'live_routing_changed':False},indent=2)+'\n')

if __name__=='__main__':main()
