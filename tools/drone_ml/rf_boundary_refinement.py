"""Bounded offline energy-core proposals; parents remain available as context.

Quiet-support checks describe the observed window, never verified noise or
drone absence. No state is carried across capture gaps or retuning events.
"""
from dataclasses import asdict, dataclass
import math

import numpy as np
from scipy import fft, ndimage

from .rf_localizer import _rate, proposal_view


@dataclass(frozen=True)
class BoundaryConfig:
    frequency_peak_drop_db: float = 12.
    time_peak_drop_db: float = 10.
    minimum_contrast_db: float = 6.
    quiet_tolerance_db: float = 3.
    minimum_quiet_frames: int = 4
    context_seconds: float = .0005
    max_read_samples: int = 4_000_000
    max_total_read_samples: int = 64_000_000
    max_parents: int = 256
    max_cores: int = 1024
    max_cores_per_parent: int = 8
    max_bands_per_parent: int = 8

    def validate(self):
        for k in ('frequency_peak_drop_db','time_peak_drop_db','minimum_contrast_db','quiet_tolerance_db'):
            v=getattr(self,k)
            if not math.isfinite(v) or not 0<v<=30:raise ValueError('invalid contrast')
        if not math.isfinite(self.context_seconds) or not 0<=self.context_seconds<=.002:
            raise ValueError('invalid context')
        for k,lo,hi in [('minimum_quiet_frames',2,64),('max_read_samples',2048,4_000_000),
                        ('max_total_read_samples',2048,128_000_000),('max_parents',1,1024),('max_cores',1,4096),
                        ('max_cores_per_parent',1,64),('max_bands_per_parent',1,64)]:
            v=getattr(self,k)
            if type(v) is not int or not lo<=v<=hi:raise ValueError('invalid budget')


def _runs(mask):
    edges=np.diff(np.r_[False,mask,False].astype(np.int8))
    return list(zip(np.flatnonzero(edges==1),np.flatnonzero(edges==-1)))


def reference_quality(trace, config=BoundaryConfig()):
    config.validate(); trace=np.asarray(trace,dtype=float)
    if trace.ndim!=1 or len(trace)<4 or not np.isfinite(trace).all() or np.any(trace<0):
        raise ValueError('invalid reference trace')
    floor=max(float(np.max(trace))*1e-10,1e-30)
    low=max(float(np.quantile(trace,.1)),floor)
    high=max(float(np.quantile(trace,.98)),floor)
    quiet=trace<=low*10**(config.quiet_tolerance_db/10)
    runs=_runs(quiet);longest=max((b-a for a,b in runs),default=0)
    contrast=10*math.log10(high/low)
    supported=(contrast>=config.minimum_contrast_db and longest>=config.minimum_quiet_frames
               and np.count_nonzero(quiet)>=config.minimum_quiet_frames)
    return dict(status='local_quiet_support' if supported else 'insufficient_local_quiet_support',
        usable_for_local_edges=bool(supported),contrast_db=contrast,
        quiet_fraction=float(np.mean(quiet)),longest_quiet_run_frames=int(longest),
        low_reference_power=low,high_reference_power=high,verified_noise=False,
        reason='Contrast and contiguous quiet support are heuristic; gain changes and other emitters remain ambiguous.')


def refine_parent(iq, rate, read_start, parent, config=BoundaryConfig()):
    config.validate();_rate(rate);raw=np.asarray(iq)
    if (raw.ndim!=1 or not 2048<=len(raw)<=config.max_read_samples or not np.isfinite(raw).all()
            or type(read_start) is not int or read_start<0):raise ValueError('invalid refinement input')
    if not (read_start<=parent['start_sample']<parent['stop_sample']<=read_start+len(raw)
            and -rate/2<=parent['low_hz']<parent['high_hz']<=rate/2):
        raise ValueError('parent outside read or native band')
    if parent.get('stable_background_ambiguity'):
        return [],dict(status='stable_context_only',verified_noise=False)
    scale=float(np.max(np.abs(raw)))
    if not math.isfinite(scale):raise ValueError('invalid magnitude')
    x=np.asarray(raw/(scale or 1),dtype=np.complex64);x-=x.mean()
    frames=np.lib.stride_tricks.sliding_window_view(x,1024)[::512]
    z=fft.fftshift(fft.fft(frames*np.hanning(1025)[:-1].astype(np.float32),axis=1,workers=1),axes=1)
    power=ndimage.uniform_filter1d(z.real**2+z.imag**2,3,axis=1,mode='nearest')
    centers=(np.arange(len(power))*512+512+read_start)
    in_parent=(centers>=parent['start_sample'])&(centers<parent['stop_sample'])
    freq=(np.arange(1024)-512)*rate/1024
    band=(freq>=parent['low_hz'])&(freq<=parent['high_hz'])
    if not np.any(in_parent) or not np.any(band):return [],dict(status='below_grid_support',verified_noise=False)
    profile=np.quantile(power[in_parent],.9,axis=0)
    peak=float(profile[band].max());mask=band&(profile>=peak*10**(-config.frequency_peak_drop_db/10))
    if peak<=0:return [],dict(status='zero_input',verified_noise=False)
    cores=[];quality=[]
    bands=[(a,b) for a,b in _runs(mask) if b-a>=3]
    if len(bands)>config.max_bands_per_parent:
        return [],dict(status='fragmentation_deferred',observed_bands=len(bands),verified_noise=False)
    for f0,f1 in bands:
        trace=np.mean(power[:,f0:f1],axis=1)
        q=reference_quality(trace,config);quality.append(dict(low_hz=float(freq[f0]),high_hz=float(freq[f1-1]),**q))
        if not q['usable_for_local_edges']:continue
        gate=max(q['low_reference_power']*10**(config.minimum_contrast_db/10),
                 q['high_reference_power']*10**(-config.time_peak_drop_db/10))
        for a,b in _runs(trace>=gate):
            if b-a<2:continue
            start=max(parent['start_sample'],read_start+a*512)
            stop=min(parent['stop_sample'],read_start+(b-1)*512+1024)
            if stop<=start:continue
            lo=max(parent['low_hz'],float(freq[f0]-rate/2048))
            hi=min(parent['high_hz'],float(freq[f1-1]+rate/2048))
            cores.append(dict(parent_id=parent['id'],start_sample=int(start),stop_sample=int(stop),
                low_hz=lo,high_hz=hi,branches=['energy_core'],stable_background_ambiguity=False,
                reference_quality=q,proposed_view=proposal_view(lo,hi,rate),
                training_eligible=False,accepted_identity=None))
            if len(cores)>config.max_cores_per_parent:
                # Do not cherry-pick the first or loudest fragments as valid
                # boundaries. Keep the original parent and defer this case.
                return [],dict(status='fragmentation_deferred',core_count_lower_bound=len(cores),
                               bands=quality,verified_noise=False)
    return cores,dict(status='cores_proposed' if cores else 'no_supported_core',bands=quality,verified_noise=False)


def refine(get, parent_result, config=BoundaryConfig()):
    config.validate();rate=parent_result['sample_rate_hz'];_rate(rate)
    limit=parent_result['analysed_samples'];cores=[];reviews=[];read_samples=0;attempted=0;drops=0
    for parent in parent_result['regions']:
        if parent.get('stable_background_ambiguity'):
            reviews.append(dict(parent_id=parent['id'],status='stable_context_only'));continue
        pad=round(config.context_seconds*rate)
        start=max(0,parent['start_sample']-pad);stop=min(limit,parent['stop_sample']+pad)
        length=stop-start
        if (attempted>=config.max_parents or length>config.max_read_samples
                or read_samples+length>config.max_total_read_samples):
            reviews.append(dict(parent_id=parent['id'],status='budget_deferred'));continue
        if length<2048:
            reviews.append(dict(parent_id=parent['id'],status='below_grid_support'));continue
        attempted+=1;raw=get(start,length);read_samples+=length
        if len(raw)!=length:raise ValueError('short refinement read')
        incoming,q=refine_parent(raw,rate,start,parent,config)
        reviews.append(dict(parent_id=parent['id'],**q))
        remaining=config.max_cores-len(cores);drops+=max(0,len(incoming)-remaining)
        cores.extend(incoming[:remaining])
    # Nested parent proposals can yield the exact same child; retain lineage.
    unique={}
    for r in cores:
        key=tuple(r[k] for k in ('start_sample','stop_sample','low_hz','high_hz'))
        if key in unique:unique[key]['parent_ids'].append(r['parent_id'])
        else:unique[key]=dict(r,parent_ids=[r['parent_id']])
    cores=list(unique.values())
    for i,r in enumerate(cores):r['id']=i
    return dict(schema='rfmon.drone_ml.boundary_refinement.v1',config=asdict(config),regions=cores,
        parent_reviews=reviews,read_samples=read_samples,core_budget_drops=drops,
        parent_context_retained=True,reference_verified_as_noise=False,
        parent_regions=parent_result['regions'],sample_rate_hz=rate,analysed_samples=limit,
        fallback=parent_result['fallback'],drone_presence=None,accepted_identity=None,
        training_eligible=False,live_qualified=False,reference_coordinates_used=False)
