"""Compare live 20-MSps view selection with proposal-centered diagnostic views.

Only explicit train/development sources with verified derived-source lineage
are permitted. Report boxes are unverified morphology references, not labels.
No checkpoint is run on a changed bandwidth or STFT shape.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np

from .common import new_output, read_source, sha256, stamp, write_json
from .preprocess import STFTConfig, select_offset, spectrogram
from .temporal_audit import check_record, reader


def load_json(path):
    with read_source(path) as f: return json.load(f)


def overlap_fraction(center, bandwidth, low, high):
    if not all(math.isfinite(x) for x in (center, bandwidth, low, high)) or bandwidth<=0 or high<=low:
        raise ValueError('invalid frequency interval')
    return max(0.,min(high,center+bandwidth/2)-max(low,center-bandwidth/2))/(high-low)


def centered_view(center, target_rate, source_rate):
    if target_rate>source_rate: raise ValueError('upsampling forbidden')
    if target_rate==source_rate: return 0.
    extent=.45*source_rate-target_rate/2
    if extent<0: raise ValueError('no guarded view fits')
    return float(np.clip(center,-extent,extent))


def reference_geometry(proposal, rate, capture_center, available):
    values=[float(proposal[k]) for k in ('start_ms','duration_us','center_mhz','bandwidth_mhz')]
    if not all(math.isfinite(x) for x in values): raise ValueError('nonfinite reference')
    start_ms,dur_us,center_mhz,bw_mhz=values
    start=round(start_ms*.001*rate);count=round(dur_us*1e-6*rate)
    if start<0 or not 1<=count<=4_000_000 or start+count>available:
        raise ValueError('reference outside source')
    center=center_mhz*1e6-capture_center;bw=bw_mhz*1e6
    if not 0<bw<=rate or abs(center)+bw/2>rate/2:
        raise ValueError('reference outside observed spectrum')
    n=round(rate*.008)
    if available<n: raise ValueError('no full 8 ms context')
    if count>n: raise ValueError('reference exceeds 8 ms diagnostic context')
    # Center within an admissible real 8 ms interval, not across a capture gap.
    context_start=max(0,min((start+count//2)//n*n,available-n))
    if start<context_start or start+count>context_start+n:
        context_start=max(0,min(start,available-n))
    return dict(start=start,count=count,center_hz=center,bandwidth_hz=bw,
                low_hz=center-bw/2,high_hz=center+bw/2,context_start=context_start,context_count=n)


def roi_power_fraction(iq, rate, low, high, view_center, view_rate):
    """Native-window PSD fraction inside both reference box and nominal view.

    This is spectral containment, not the resampler's attenuation or SNR.
    """
    nfft=min(4096,2**int(math.floor(math.log2(len(iq)))))
    if nfft<16: raise ValueError('reference too short for spectral diagnostic')
    x=np.asarray(iq[:len(iq)//nfft*nfft],dtype=np.complex128).reshape(-1,nfft)
    x=x-np.mean(x)
    power=np.mean(abs(np.fft.fftshift(np.fft.fft(x*np.hanning(nfft),axis=1),axes=1))**2,axis=0)
    f=np.fft.fftshift(np.fft.fftfreq(nfft,1/rate))
    mask=(f>=low)&(f<=high)
    denom=float(power[mask].sum())
    inside=(abs(f-view_center)<=view_rate/2)
    return None if denom<=0 else float(power[mask&inside].sum()/denom)


def evaluate(get, available, rate, center_hz, proposal):
    g=reference_geometry(proposal,rate,center_hz,available)
    iq=get(g['context_start'],g['context_count'])
    if not np.isfinite(iq).all(): raise ValueError('nonfinite context')
    baseline=STFTConfig(duration_seconds=.008,hop=256,auto_frequency_view=True)
    chosen=select_offset(iq,rate,baseline)
    # All views use the exact same native 8 ms input. Wider arrays are diagnostic only.
    configs=[('production_20',baseline)]
    for target,nfft in [(20000000,256),(25000000,256),(50000000,512)]:
        if target<=rate:
            off=centered_view(g['center_hz'],target,rate)
            configs.append((f'proposal_centered_{target//1000000}',STFTConfig(
                rate_hz=target,duration_seconds=.008,nfft=nfft,hop=nfft,offset_hz=off)))
    event=get(g['start'],g['count'])
    views=[];arrays={}
    for name,cfg in configs:
        a,m=spectrogram(iq,rate,cfg,return_metadata=True)
        off=m['selected_offset_hz']
        if name=='production_20' and off!=chosen: raise ValueError('selector parity failure')
        arrays[name]=a
        views.append(dict(name=name,rate_hz=cfg.rate_hz,offset_hz=off,shape=list(a.shape),
            preprocessing=cfg.metadata(),
            nominal_box_fraction=overlap_fraction(off,cfg.rate_hz,g['low_hz'],g['high_hz']),
            interior_80pct_box_fraction=overlap_fraction(off,.8*cfg.rate_hz,g['low_hz'],g['high_hz']),
            reference_region_native_power_fraction=roi_power_fraction(event,rate,g['low_hz'],g['high_hz'],off,cfg.rate_hz),
            resampled_source_unit_rms_mean_power=float(np.mean(10.**(a.astype(np.float64)/10.))),
            model_inference_performed=False))
    return dict(reference=proposal,geometry=g,views=views,
        context_iq_sha256=hashlib.sha256(np.asarray(iq,dtype='<c8').tobytes()).hexdigest(),
        reference_iq_sha256=hashlib.sha256(np.asarray(event,dtype='<c8').tobytes()).hexdigest()),arrays


def audit(sources_path,lineage_path,proposal_paths,output):
    sources_path,lineage_path=map(Path,(sources_path,lineage_path));proposal_paths=list(map(Path,proposal_paths))
    if not 1<=len(proposal_paths)<=8:raise ValueError('proposal file budget')
    inputs=[sources_path,lineage_path,*proposal_paths]
    stamps={str(p):stamp(p) for p in inputs};digests={str(p):sha256(p) for p in inputs}
    manifest=load_json(sources_path)
    if manifest.get('schema')!='rfmon.drone_ml.temporal_sources.v1':raise ValueError('invalid sources')
    origins=load_json(lineage_path)['records']
    jobs=[]
    for path in proposal_paths:
        report=load_json(path)
        match=[o for o in origins if o.get('archive_member')==report['source']]
        if len(match)!=1:raise ValueError('ambiguous or absent proposal lineage')
        origin=match[0]
        rows=[r for r in manifest['records'] if r['path']==origin['path']]
        if len(rows)!=1:raise ValueError('source not explicitly assigned')
        row=rows[0];check_record(row)  # Before opening IQ or hashing IQ.
        if (origin.get('partition') not in ('train','development','development_only')
                or row['source_sha256']!=origin['sha256'] or origin.get('source_sample_offset')!=0
                or origin['sample_rate_hz']!=row['sample_rate_hz']
                or report['sample_rate_msps']*1e6!=row['sample_rate_hz']
                or report['center_freq_mhz']*1e6!=origin['capture_center_hz']):
            raise ValueError('proposal origin metadata mismatch')
        if not 1<=len(report['packets'])<=8:raise ValueError('proposal count budget')
        jobs.append((path,report,row,origin))
    out=new_output(output,[p.parent for p in inputs]+[Path(j[2]['path']).parent for j in jobs])
    records=[]
    for j,(path,report,row,origin) in enumerate(jobs):
        source=Path(row['path']);before=stamp(source)
        if sha256(source)!=row['source_sha256']:raise ValueError('source hash mismatch')
        regions=[]
        with reader(row) as (available,get):
            for k,proposal in enumerate(report['packets']):
                result,arrays=evaluate(get,available,int(row['sample_rate_hz']),origin['capture_center_hz'],proposal)
                name=f'source_{j}_region_{k}.npz'
                with (out/name).open('xb') as f:np.savez_compressed(f,**arrays)
                result.update(tensor_file=name,tensor_sha256=sha256(out/name))
                regions.append(result)
        if stamp(source)!=before:raise ValueError('source changed')
        records.append(dict(source=row,origin=origin,proposal_path=str(path),regions=regions,
            source_stats_preserved=True,reference_truth='unverified_energy_regions_not_drone_labels',
            independent_evaluation=False))
    if any(stamp(p)!=stamps[str(p)] for p in inputs):raise ValueError('metadata changed')
    all_regions=[r for s in records for r in s['regions']]
    summary={}
    for kind in {v['name'] for r in all_regions for v in r['views']}:
        v=[v for r in all_regions for v in r['views'] if v['name']==kind]
        summary[kind]=dict(regions=len(v),fully_contained_boxes=sum(x['nominal_box_fraction']>=.999999 for x in v),
            mean_box_fraction=float(np.mean([x['nominal_box_fraction'] for x in v])),
            mean_native_reference_power_fraction=float(np.mean([x['reference_region_native_power_fraction'] for x in v
                                                               if x['reference_region_native_power_fraction'] is not None])))
    result=dict(schema='rfmon.drone_ml.frequency_view_audit.v1',input_sha256=digests,records=records,summary=summary,
        final_test_iq_read=False,training_eligible=False,model_inference_performed=False,live_changes=False,
        limitations=['Source-attributed reference boxes are unverified; fractions are not detection accuracy.',
                    'Proposal-centered views use offline report locations; not a validated live localizer.',
                    'Spectral containment excludes resampling transition loss and is not a noise-corrected signal fraction.',
                    'Original/derivative overlap prevents independent model validation.',
                    'Wider views require separately trained preprocessing-compatible models.'])
    write_json(out/'report.json',result)
    write_json(out/'summary.json',summary)
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sources',type=Path,required=True);p.add_argument('--lineage',type=Path,required=True)
    p.add_argument('--proposals',type=Path,nargs='+',required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=audit(a.sources,a.lineage,a.proposals,a.output)
    print(json.dumps(r['summary'],indent=2))


if __name__=='__main__':main()
