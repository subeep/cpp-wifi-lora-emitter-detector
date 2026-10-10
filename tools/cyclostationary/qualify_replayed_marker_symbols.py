#!/usr/bin/env python3
"""Check complete chirp span on already copied IQ and retained replay evidence."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import numpy as np
from marker_symbol import assess_symbol, prepare_filtered_window
from marker_prefix import assess_marker_structure
from marker_recurrence import assess_segmented_recurrence


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest',type=Path,required=True)
    p.add_argument('--replay',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--check-prefix',action='store_true',help='check prefix/tail and span at a common start; offline only')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    rows=[]
    for record in json.loads(a.manifest.read_text())['records']:
        src=Path(record['path'])
        if src.resolve().parent != a.manifest.resolve().parent or src.is_symlink():
            raise ValueError('only workspace derivatives beside manifest are allowed')
        payload=src.read_bytes()
        if hashlib.sha256(payload).hexdigest()!=record['sha256']:
            raise ValueError('derived IQ checksum mismatch')
        if record['format']=='cf32_le':
            iq=np.frombuffer(payload,dtype='<c8')
        elif record['format']=='ci16_le':
            v=np.frombuffer(payload,dtype='<i2').reshape(-1,2)
            iq=(v[:,0].astype(np.float32)+1j*v[:,1].astype(np.float32))/32768
        else:raise ValueError('unsupported derivative format')
        if len(iq)!=record['samples']:raise ValueError('sample count mismatch')
        replay_path=a.replay/(src.stem+'.json');replay_bytes=replay_path.read_bytes()
        replay=json.loads(replay_bytes)
        if replay['record']!=record:raise ValueError('replay provenance differs from manifest')
        evidence=[]
        for w in replay['windows']:
            native=iq[w['offset']:w['offset']+65536]
            for trial in [w]+w['refinements']:
                eligible=[c for c in trial['named'] if c['status']=='unvalidated_candidate']
                if not eligible:continue
                filtered=trial is not w
                x,rate=prepare_filtered_window(native,record['sample_rate_hz'],trial) if filtered else (native,record['sample_rate_hz'])
                for c in eligible:
                    m=trial['chirps'][c['measurement_index']]
                    check=assess_symbol(x,rate,m['peak_window_offset_samples'],m['slope_hz_per_second'],m['frequency_mean_offset_hz'])
                    if a.check_prefix:
                        check['prefix']=assess_marker_structure(x,rate,m['peak_window_offset_samples'],m['slope_hz_per_second'],m['frequency_mean_offset_hz'])
                    step=c['source_sample_step']
                    first=c['first_source_sample_center']-m['peak_window_offset_samples']*step
                    if 'start_offset_samples' in check:
                        check['first_original_source_sample_center']=record['source_sample_offset']+first+check['start_offset_samples']*step
                    if a.check_prefix and check['prefix'].get('joint_span_and_prefix'):
                        cp=check['prefix'];cp['first_original_source_sample_center']=record['source_sample_offset']+first+cp['prefix_start_samples']*step
                        cp['last_original_source_sample_center']=record['source_sample_offset']+first+(cp['symbol_start_samples']+cp['symbol_lag_samples']-1)*step
                    evidence.append(dict(window_offset=w['offset'],profile=c['profile'],filtered=filtered,
                        legacy_marker_source_sample=c['first_source_sample_center'],source_sample_step=step,
                        frequency_hz=record['capture_center_hz']+(trial['center_offset_hz'] if filtered else 0)+m['frequency_mean_offset_hz'],diagnostic=check))
        counts=dict(Counter(e['diagnostic']['status'] for e in evidence))
        # Raw/refined views of one marker are not independent observations.
        groups=[]
        for e in sorted(evidence,key=lambda v:v['legacy_marker_source_sample']):
            group=next((g for g in groups if g[0]['profile']==e['profile'] and
                abs(g[0]['legacy_marker_source_sample']-e['legacy_marker_source_sample'])<=record['sample_rate_hz']*66.7e-6 and
                abs(g[0]['frequency_hz']-e['frequency_hz'])<=250000),None)
            if group is None:groups.append([e])
            else:group.append(e)
        distinct_supported=sum(any(e['diagnostic']['status']=='span_with_boundary_contrast' for e in g) for g in groups)
        row=dict(record=record,evaluations=len(evidence),status_counts=counts,
            distinct_legacy_markers=len(groups),distinct_markers_with_span_and_contrast=distinct_supported,
            accepted_family=None,independent_validation=False,
            replay_sha256=hashlib.sha256(replay_bytes).hexdigest(),evidence=evidence)
        if a.check_prefix:
            row['distinct_markers_with_joint_span_prefix']=sum(any(e['diagnostic']['prefix'].get('joint_span_and_prefix',False) for e in g) for g in groups)
            row['prefix_status_counts']=dict(Counter(e['diagnostic']['prefix']['status'] for e in evidence))
            structured=[]
            for e in evidence:
                cp=e['diagnostic']['prefix']
                if not cp.get('joint_span_and_prefix'):continue
                start=cp['first_original_source_sample_center']-record['source_sample_offset']
                end=cp['last_original_source_sample_center']-record['source_sample_offset']+1
                structured.append(dict(sample=start,profile=e['profile'],frequency_hz=e['frequency_hz'],
                    coherence=cp['coherence'],support_start=start,support_end=end))
            # Current source manifests have no independently audited continuity.
            # Keep the acquisition interval explicit; do not upgrade that claim.
            row['structured_recurrence']=assess_segmented_recurrence(structured,record['sample_rate_hz'],[
                dict(id='derived_interval',start=0,end=record['samples'],continuity_verified=False)])
            row['identity_blockers']=['protocol_uniqueness_unproven','independent_mode_truth_missing',
                'continuity_unverified','receiver_passband_unqualified','full_search_false_positives_uncalibrated']
            if record.get('label_conflicts'):row['identity_blockers'].append('source_label_requires_review')
        (a.output/(src.stem+'.json')).write_text(json.dumps(row,indent=2)+'\n')
        rows.append({k:v for k,v in row.items() if k!='evidence'})
        print(src.name,counts,flush=True)
    (a.output/'summary.json').write_text(json.dumps(dict(schema='marker_symbol_diagnostics.v1',records=rows,
        policy_sha256=hashlib.sha256(Path(__file__).with_name('marker_symbol.py').read_bytes()).hexdigest(),
        script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        live_behavior_changed=False,identity_acceptance_enabled=False,prefix_check_requested=a.check_prefix,
        prefix_policy_sha256=hashlib.sha256(Path(__file__).with_name('marker_prefix.py').read_bytes()).hexdigest() if a.check_prefix else None),indent=2)+'\n')

if __name__=='__main__':main()
