"""Bounded offline RF activity/coverage study; never a packet or drone detector.

Uses native-rate complex spectra and energy envelopes in fixed frequency views.
No resampling, model prediction, label assignment or reserved-partition access.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import math
from pathlib import Path

import h5py
import numpy as np

from .common import read_source, sha256, stamp
from .live_audit import write_new_json
from .public_background import capture_layout, read_iq

CONTEXTS_MS=(8,16,32,64,128,256,512,1000)
ALLOWED_PARTITIONS=('train','development','development_only')


def check_record(row):
    # Reject protected data before even opening its path.
    if row.get('partition') not in ALLOWED_PARTITIONS:
        raise ValueError('reserved, unknown or missing partition is forbidden')
    if not row.get('collection_group') or not row.get('source_sha256'):
        raise ValueError('explicit source hash and collection group required')
    rate=row.get('sample_rate_hz',0)
    if not isinstance(rate,(int,float)) or not math.isfinite(rate) or not 20e6<=rate<=200e6 or rate!=int(rate):
        raise ValueError('unsupported actual sample rate')
    if row.get('adapter') not in ('cf32_le','ci16_le','nist_i16_hdf5'):
        raise ValueError('unsupported source adapter')


@contextmanager
def reader(row):
    path=Path(row['path']);before=stamp(path)
    with read_source(path) as f:
        if row['adapter']=='nist_i16_hdf5':
            with h5py.File(f,'r') as h:
                keys=capture_layout(h)
                if row['dataset_key'] not in keys:raise ValueError('unknown NIST capture')
                ds=h[row['dataset_key']]
                yield ds.size//2,lambda start,n:read_iq(ds,start,n)
        else:
            size=8 if row['adapter']=='cf32_le' else 4
            if before[2]%size:raise ValueError('misaligned IQ')
            def get(start,n):
                if start<0 or not 0<n<=4000000 or (start+n)*size>before[2]:
                    raise ValueError('read out of bounds')
                f.seek(start*size);raw=f.read(n*size)
                if len(raw)!=n*size:raise ValueError('short IQ read')
                if size==8:return np.frombuffer(raw,dtype='<c8')
                v=np.frombuffer(raw,dtype='<i2').astype(np.float32)/32768.
                return v[::2]+1j*v[1::2]
            yield before[2]//size,get
    if stamp(path)!=before:raise ValueError('source changed during audit')


def runs(mask):
    edges=np.diff(np.r_[False,mask,False].astype(np.int8))
    return np.flatnonzero(edges==-1)-np.flatnonzero(edges==1)


def activity_summary(power,step_seconds,threshold_db):
    if not len(power) or not np.isfinite(power).all() or np.any(power<0):
        raise ValueError('invalid energy envelope')
    # A within-recording reference, NOT an estimated physical noise floor.
    reference=float(np.quantile(power,.2))
    threshold=max(reference,1e-20)*10**(threshold_db/10)
    mask=power>threshold
    lengths=runs(mask)*step_seconds*1000
    quiet=runs(~mask)*step_seconds*1000
    prefix=np.r_[0,np.cumsum(mask,dtype=np.int64)]
    contexts=[]
    for ms in CONTEXTS_MS:
        n=math.ceil(ms/1000/step_seconds)
        counts=prefix[n:]-prefix[:-n] if n<=len(mask) else np.array([],dtype=int)
        contexts.append(dict(requested_ms=ms,actual_ms=n*step_seconds*1000,
            correlated_windows=int(counts.size),
            fraction_containing_energy_activity=float(np.mean(counts>0)) if counts.size else None))
    return dict(relative_threshold_db=threshold_db,reference_power_20th_percentile=reference,
        active_fraction=float(np.mean(mask)),energy_islands=len(lengths),
        island_duration_ms_quantiles=np.quantile(lengths,[0,.5,.9,1]).tolist() if len(lengths) else [],
        maximum_quiet_ms=float(max(quiet,default=0)),contexts=contexts,
        interpretation='Relative-energy islands, not packet lengths or drone detections; edge islands are censored. '
        'Overlapping context windows are correlated. Continuous traffic can have zero islands.')


def analyse_record(row,duration_seconds=1.):
    check_record(row)
    if not math.isfinite(duration_seconds) or not .008<=duration_seconds<=2:
        raise ValueError('duration must be between 8 ms and 2 seconds')
    rate=int(row['sample_rate_hz']);nfft=1024
    block=max(nfft,round(rate*.001/nfft)*nfft)
    frequencies=np.fft.fftshift(np.fft.fftfreq(nfft,1/rate))
    extent=max(0,.45*rate-10e6)
    offsets=np.unique(np.linspace(-extent,extent,5))
    # 16 MHz interior supports comparison with current guarded 20 MSps selector.
    masks=[np.abs(frequencies-o)<=8e6 for o in offsets]
    if any(not m.any() for m in masks):raise ValueError('frequency view contains no bins')
    views=[[] for _ in masks];broad=[];sample_digest=hashlib.sha256()
    window=np.hanning(nfft+1)[:-1];component_peak=0.;near_rail=0
    with reader(row) as (available,get):
        limit=min(available,round(rate*duration_seconds))
        used=limit//block*block
        if used<block:raise ValueError('insufficient samples')
        for start in range(0,used,block):
            iq=get(start,block)
            if not np.isfinite(iq).all():raise ValueError('nonfinite IQ')
            sample_digest.update(np.asarray(iq,dtype='<c8').tobytes())
            component_peak=max(component_peak,float(np.max(np.abs(iq.real))),float(np.max(np.abs(iq.imag))))
            near_rail+=int(np.count_nonzero((np.abs(iq.real)>=.99)|(np.abs(iq.imag)>=.99)))
            frames=iq.astype(np.complex128).reshape(-1,nfft)
            frames=frames-frames.mean(axis=1,keepdims=True)
            psd=np.mean(np.abs(np.fft.fftshift(np.fft.fft(frames*window,axis=1),axes=1))**2,axis=0)/np.sum(window**2)
            broad.append(float(np.sum(psd)))
            for values,mask in zip(views,masks):values.append(float(np.sum(psd[mask])))
    profiles=[]
    for offset,values in [(None,broad),*zip(offsets,views)]:
        profiles.append(dict(offset_hz=None if offset is None else float(offset),
            kind='full_band' if offset is None else '16MHz_interior_of_20MSps_view',
            mean_power=float(np.mean(values)),
            thresholds=[activity_summary(np.asarray(values),block/rate,db) for db in (3,6,10)]))
    best=int(np.argmax([np.mean(v) for v in views]))
    return dict(source=row,requested_seconds=duration_seconds,available_samples=available,
        analysed_samples=used,analysed_seconds=used/rate,trailing_samples_not_analysed=limit-used,
        energy_bin_seconds=block/rate,nfft=nfft,analysed_cf32_sha256=sample_digest.hexdigest(),
        component_peak=component_peak,component_ge_099_fraction=near_rail/used,
        largest_mean_power_view_hz=float(offsets[best]),profiles=profiles,
        labels_created=False,packet_boundaries_verified=False,
        limitation='Native contiguous array only; acquisition continuity inherits source metadata. '
        'No claim of physical packet boundaries, chirp visibility, drone recall or false alarms.')


def audit(manifest_path,output,duration_seconds=1.):
    manifest_path=Path(manifest_path);output=Path(output)
    manifest_before=stamp(manifest_path);manifest_digest=sha256(manifest_path)
    with read_source(manifest_path) as f:manifest=json.load(f)
    if manifest.get('schema')!='rfmon.drone_ml.temporal_sources.v1':raise ValueError('unsupported manifest')
    rows=manifest.get('records',[])
    if not 1<=len(rows)<=32:raise ValueError('select between 1 and 32 recordings')
    for row in rows:check_record(row)
    if output.exists():raise FileExistsError(output)
    for row in rows:
        if output.resolve().is_relative_to(Path(row['path']).resolve().parent):
            raise ValueError('output must be outside source directory')
    checked={};before={}
    for row in rows:
        path=Path(row['path']).resolve()
        if path not in checked:
            before[path]=stamp(path);checked[path]=sha256(path)
        if checked[path]!=row['source_sha256']:raise ValueError('source SHA-256 mismatch')
    results=[analyse_record(row,duration_seconds) for row in rows]
    if any(stamp(p)!=s for p,s in before.items()):raise ValueError('source changed during audit')
    if stamp(manifest_path)!=manifest_before:raise ValueError('source manifest changed during audit')
    report=dict(schema='rfmon.drone_ml.temporal_audit.v1',records=results,
        source_manifest_sha256=manifest_digest,
        source_hashes_verified=True,source_stats_preserved=True,final_test_iq_read=False,
        live_qualified=False,optimal_context_determined=False,
        current_inference_max_analysed_seconds_per_second=.048,
        note='Coverage diagnostics only. Fit and compare models on independently grouped labels before selecting context.')
    write_new_json(output,report)
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--seconds',type=float,default=1.)
    a=p.parse_args();r=audit(a.manifest,a.output,a.seconds)
    print(json.dumps(dict(recordings=len(r['records']),source_stats_preserved=True,optimal_context_determined=False)))


if __name__=='__main__':main()
