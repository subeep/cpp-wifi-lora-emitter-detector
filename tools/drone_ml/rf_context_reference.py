"""Offline integrated-energy proposals with explicit observation boundaries.

This is a complementary weak/high-duty branch, not a drone decision. A 20 ms
reference uses only its own declared segment, never hidden persistent history.
Unverified real captures may be analysed in sample-index space only.
"""
from dataclasses import asdict, dataclass
import math

import numpy as np
from scipy import fft, ndimage

from .rf_localizer import _rate, proposal_view
from .rf_localizer_multiscale import _components, _area


@dataclass(frozen=True)
class ContextConfig:
    block_seconds: float = .02
    reference_quantile: float = .05
    high_db: float = 1.5
    low_db: float = .8
    integration_time_cells: int = 9
    integration_frequency_bins: int = 33
    quiet_tolerance_db: float = .5
    minimum_quiet_run: int = 4
    max_regions_per_block: int = 128
    max_regions_per_record: int = 1024

    def validate(self):
        if self.block_seconds not in (.01,.02):raise ValueError('unsupported block duration')
        if not math.isfinite(self.reference_quantile) or not .02<=self.reference_quantile<=.2:
            raise ValueError('invalid reference quantile')
        if not all(math.isfinite(v) for v in (self.high_db,self.low_db,self.quiet_tolerance_db)):
            raise ValueError('nonfinite thresholds')
        if not 0<self.low_db<self.high_db<=10 or not 0<self.quiet_tolerance_db<=3:
            raise ValueError('invalid thresholds')
        if (self.integration_time_cells,self.integration_frequency_bins)!=(9,33):
            raise ValueError('unsupported integration profile')
        for k,hi in [('minimum_quiet_run',64),('max_regions_per_block',512),('max_regions_per_record',4096)]:
            if type(getattr(self,k)) is not int or not 1<=getattr(self,k)<=hi:raise ValueError('invalid budget')


def _longest(mask):
    edges=np.diff(np.r_[False,mask,False].astype(np.int8))
    return max(np.flatnonzero(edges==-1)-np.flatnonzero(edges==1),default=0)


def chunk(iq,rate,start,config=ContextConfig()):
    config.validate();_rate(rate);raw=np.asarray(iq)
    if raw.ndim!=1 or not 16896<=len(raw)<=4_000_000 or not np.isfinite(raw).all():
        raise ValueError('invalid context IQ block')
    if type(start) is not int or start<0:raise ValueError('invalid offset')
    scale=float(np.max(np.abs(raw)))
    if not math.isfinite(scale):raise ValueError('invalid IQ magnitude')
    x=np.asarray(raw/(scale or 1),dtype=np.complex64);x-=x.mean()
    n=((len(x)-1024)//512+1)//2
    frames=np.lib.stride_tricks.sliding_window_view(x,1024)[::512][:n*2]
    z=fft.fftshift(fft.fft(frames*np.hanning(1025)[:-1].astype(np.float32),axis=1,workers=1),axes=1)
    power=np.mean((z.real**2+z.imag**2).reshape(n,2,1024),axis=1)
    smooth=ndimage.uniform_filter(power,(config.integration_time_cells,config.integration_frequency_bins),mode='reflect')
    floor=max(float(smooth.max())*1e-10,1e-30)
    reference=np.maximum(np.quantile(smooth,config.reference_quantile,axis=0),floor)
    # A low quantile underestimates stationary noise even after integration.
    # Estimate that common bias from the lower quintile across frequencies.
    # This assumes some frequencies are less temporally occupied; full-band
    # changes remain ambiguous and are explicitly flagged on proposals.
    bias=max(float(np.quantile(np.median(smooth,axis=0)/reference,.2)),1.)
    contrast=10*np.log10(np.maximum(smooth/(reference*bias),1e-10))
    regions=_components(contrast,config.high_db,config.low_db,'context_integrated',rate,start,1536,1024,
                        min_cells=18,min_high=6,seed_time_margin=4)
    accepted=[];unsupported=0
    for r in regions:
        f0=max(0,round(r['low_hz']/(rate/1024)+512+.5))
        f1=min(1024,round(r['high_hz']/(rate/1024)+512+.5))
        trace=np.mean(smooth[:,f0:f1],axis=1)
        low=max(float(np.quantile(trace,config.reference_quantile)),floor)
        quiet=trace<=low*10**(config.quiet_tolerance_db/10)
        run=int(_longest(quiet));supported=run>=config.minimum_quiet_run
        if not supported:unsupported+=1;continue
        r.update(reference_quality=dict(status='local_lower_tail_support',longest_quiet_run_cells=run,
            quiet_fraction=float(np.mean(quiet)),verified_noise=False,
            note='Lower-tail support may be a gain/state change; not calibrated noise or absence.'),
            broad_common_mode_ambiguity=(r['high_hz']-r['low_hz'])>=.85*rate,
            temporal_edges_resolved=False,training_eligible=False,accepted_identity=None)
        accepted.append(r)
    accepted.sort(key=_area,reverse=True)
    dropped=max(0,len(accepted)-config.max_regions_per_block)
    return accepted[:config.max_regions_per_block],dict(pooled_frames=n,
        rejected_insufficient_quiet_support=unsupported,dropped_regions=dropped,
        all_zero=bool(np.all(raw==0)),reference_verified_noise=False,
        lower_tail_bias_factor=bias,
        actual_reference_samples=(n-1)*1024+1536)


def localize(get,available,rate,seconds=.1,*,segments=None,continuity='unverified',config=ContextConfig()):
    config.validate();_rate(rate)
    if type(available) is not int or available<0:raise ValueError('invalid sample count')
    if not math.isfinite(seconds) or not .001<=seconds<=.2:raise ValueError('duration budget')
    if continuity not in ('synthetic_contiguous','declared_contiguous','sample_index_only','unverified'):
        raise ValueError('invalid continuity scope')
    limit=min(available,round(seconds*rate))
    # Require the caller to state what its index range actually means. A missing
    # continuity declaration cannot silently become a long noise history.
    if continuity=='unverified' or segments is None:
        return dict(algorithm='context_reference_v1',config=asdict(config),regions=[],chunks=[],
            status='continuity_deferred',analysed_samples=0,sample_rate_hz=rate,
            drone_presence=None,accepted_identity=None,training_eligible=False,live_qualified=False)
    if not 1<=len(segments)<=128:raise ValueError('segment budget')
    end=0
    for a,b in segments:
        if type(a) is not int or type(b) is not int or not end<=a<b<=limit:
            raise ValueError('segments overlap or exceed observation')
        end=b
    regions=[];chunks=[];read_samples=0;drops=0;skipped=0
    stride=min(round(rate*config.block_seconds)//1024*1024,(4_000_000-512)//1024*1024)
    for segment_id,(a,b) in enumerate(segments):
        start=a
        while start+16896<=b:
            length=min(stride,(b-start-512)//1024*1024)
            remaining=(b-start-length-512)//1024
            if 0<remaining<16 and length+remaining*1024+512<=4_000_000:
                length+=remaining*1024
            iq=get(start,length+512)
            if len(iq)!=length+512:raise ValueError('short context read')
            incoming,q=chunk(iq,rate,start,config);read_samples+=len(iq)
            for r in incoming:
                r.update(segment_id=segment_id,block_start_sample=start,
                    block_stop_sample=start+q['actual_reference_samples'])
            remaining_budget=config.max_regions_per_record-len(regions)
            drops+=max(0,len(incoming)-remaining_budget);regions.extend(incoming[:remaining_budget])
            chunks.append(dict(start_sample=start,samples=len(iq),segment_id=segment_id,**q))
            start+=length
        skipped+=max(0,b-start-(512 if start>a else 0))
    regions.sort(key=lambda r:(r['start_sample'],r['low_hz']))
    for i,r in enumerate(regions):
        r.update(id=i,proposed_view=proposal_view(r['low_hz'],r['high_hz'],rate),
            touches_block_edge=r['start_sample']==r['block_start_sample'] or r['stop_sample']>=r['block_stop_sample'])
    return dict(algorithm='context_reference_v1',config=asdict(config),regions=regions,chunks=chunks,
        status='complete',sample_rate_hz=rate,analysed_samples=limit,read_samples=read_samples,
        declared_segment_samples=sum(b-a for a,b in segments),unprocessed_segment_tail_samples=skipped,
        undeclared_gap_samples=limit-sum(b-a for a,b in segments),
        dropped_regions=drops+sum(c['dropped_regions'] for c in chunks),
        continuity_scope=continuity,cross_block_stitching=False,history_persisted=False,
        wall_time_interpretation_allowed=continuity in ('synthetic_contiguous','declared_contiguous'),
        fallback=dict(required=True,kind='retain_parent_and_native_full_band_context',model_input_created=False),
        drone_presence=None,accepted_identity=None,training_eligible=False,live_qualified=False,
        reference_coordinates_used=False)
