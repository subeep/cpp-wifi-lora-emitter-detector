"""Bounded cyclic-prefix/tail comparison for an already selected chirp marker.

This is a physical repetition diagnostic, not a protocol decoder. Constant
carrier offset cancels in coherence magnitude. Searches reuse selection IQ;
thresholds are development policy and cannot establish family identity.
"""
import math
import numpy as np

POLICY='marker_prefix_tail_v1'


def assess_prefix(iq,rate,peak,*,span_check=None):
    x=np.asarray(iq,dtype=np.complex128)
    if (x.ndim!=1 or len(x)>65536 or not np.isfinite(x).all() or
        not math.isfinite(rate) or not 1e6<=rate<=1e9 or
        not isinstance(peak,int) or not 0<=peak<len(x)):
        raise ValueError('invalid bounded prefix input')
    nearest=lambda t:math.floor(rate*t+.5)
    lag=nearest(1/15000);gate=nearest(4e-6);guard=nearest(1e-6)
    legacy=2*nearest(32e-6)
    # Half a microsecond of timing uncertainty around the selected 64 us span.
    starts=sorted(set(peak+round(v) for v in np.linspace(-(lag-legacy)-nearest(.5e-6),nearest(.5e-6),33)))
    lags=sorted(set([lag-1,lag,lag+1]))
    r=dict(policy=POLICY,status='insufficient_prefix_context',prefix_compatible=False,
        family_accepted=False,thresholds_calibrated=False,independent_evidence=False,
        prefix_gate_samples=gate,prefix_gate_seconds=gate/rate,
        expected_symbol_samples=lag,start_trials=len(starts),lag_trials=len(lags),eligible_trials=0,
        coherence_floor=.8,half_coherence_floor=.8,energy_balance_floor=.5,
        guard_coherence_ceiling=.5,guard_shift_samples=guard,
        exact_prefix_length_established=False)
    scale=float(max(np.max(np.abs(x.real)),np.max(np.abs(x.imag))))
    if not scale:r['status']='no_energy';return r
    x=x/scale;x-=x.mean()
    def comparison(a,b):
        pa=float(np.vdot(a,a).real);pb=float(np.vdot(b,b).real)
        if min(pa,pb)<=1e-18:return 0.,0.
        return min(1.,float(abs(np.vdot(a,b))**2/(pa*pb))),min(pa,pb)/max(pa,pb)
    best=None;span_cache={}
    for start in starts:
        for length in lags:
            begin=start-gate;tail=begin+length
            if begin<0 or tail-guard<0 or tail+gate+guard>len(x):continue
            r['eligible_trials']+=1
            a=x[begin:start];b=x[tail:tail+gate];mid=gate//2
            whole,balance=comparison(a,b)
            first,_=comparison(a[:mid],b[:mid]);second,_=comparison(a[mid:],b[mid:])
            guards=[comparison(a,x[tail+d:tail+d+gate])[0] for d in [-guard,guard]]
            passed=whole>=.8 and min(first,second)>=.8 and balance>=.5 and max(guards)<=.5
            row=dict(prefix_compatible=bool(passed),prefix_start_samples=begin,symbol_start_samples=start,
                tail_start_samples=tail,symbol_lag_samples=length,coherence=whole,
                first_half_coherence=first,second_half_coherence=second,energy_balance=balance,
                guard_coherences=guards)
            joint=False
            if span_check is not None and passed:
                if start not in span_cache:span_cache[start]=span_check(start)
                row['same_start_span']=span_cache[start]
                joint=span_cache[start]['status']=='span_with_boundary_contrast'
            row['joint_span_and_prefix']=bool(joint)
            rank=(joint,passed,min(whole,first,second),balance,-max(guards))
            if best is None or rank>best[0]:best=rank,row
    if best:
        r.update(best[1]);r['status']='prefix_tail_compatible' if r['prefix_compatible'] else 'prefix_tail_not_supported'
    return r


def assess_marker_structure(iq,rate,peak,slope,frequency_mean_hz):
    from marker_symbol import assess_symbol
    # Existing span validator checks the numeric profile and all input metadata.
    span=assess_symbol(iq,rate,peak,slope,frequency_mean_hz)
    if span['status']=='unsupported_sample_rate':
        return dict(policy=POLICY,status='unsupported_sample_rate',prefix_compatible=False,
                    joint_span_and_prefix=False,family_accepted=False)
    return assess_prefix(iq,rate,peak,span_check=lambda start:assess_symbol(
        iq,rate,peak,slope,frequency_mean_hz,fixed_start=start))
