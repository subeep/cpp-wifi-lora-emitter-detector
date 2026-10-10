import sys
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/cyclostationary'))
from marker_prefix import assess_prefix


def example(rate,slope=270.2e9,cfo=0,cp=True):
    n=round(rate/15000);g=round(rate*4e-6);start=round(rate*20e-6)
    x=np.zeros(round(rate*120e-6),complex)
    t=(np.arange(n)-(n-1)/2)/rate
    body=np.exp(1j*np.pi*slope*t*t)
    x[start:start+n]=body
    if cp:x[start-g:start]=body[-g:]
    x*=np.exp(2j*np.pi*cfo*np.arange(len(x))/rate)
    return x,start+round(rate*1e-6)

for rate in [20e6,25e6,100e6,120e6,200e6]:
    for slope in [135.2e9,270.2e9,-135.3e9]:
        for cfo in [0,500e3,-500e3]:
            x,p=example(rate,slope,cfo)
            r=assess_prefix(x,rate,p)
            assert r['prefix_compatible'],(rate,slope,cfo,r)
            assert r['eligible_trials']<=99 and not r['family_accepted']
        x,p=example(rate,slope,cp=False)
        assert not assess_prefix(x,rate,p)['prefix_compatible']
    x,p=example(rate)
    rng=np.random.default_rng(129)
    for bad in [np.ones_like(x),np.exp(2j*np.pi*1e6*np.arange(len(x))/rate),
        rng.normal(size=len(x))+1j*rng.normal(size=len(x)),np.zeros_like(x)]:
        assert not assess_prefix(bad,rate,p)['prefix_compatible']
    assert assess_prefix(x[:round(rate*40e-6)],rate,p)['status']=='insufficient_prefix_context'
    # Destroy just one prefix half: a strong half must not hide the failure.
    start=round(rate*20e-6);g=round(rate*4e-6)
    x[start-g:start-g//2]=rng.normal(size=g-g//2)+1j*rng.normal(size=g-g//2)
    assert not assess_prefix(x,rate,p)['prefix_compatible']
for rate,peak in [(0,0),(float('nan'),0),(1e8,-1),(1e8,100)]:
    try:assess_prefix(np.ones(100),rate,peak)
    except ValueError:pass
    else:raise AssertionError('invalid prefix bounds accepted')
print('prefix: three profiles, five rates, CFO, missing/corrupt prefix, tone/noise and bounds passed')
from marker_prefix import assess_marker_structure
for rate in [20e6,25e6,100e6,120e6,200e6]:
    x,p=example(rate)
    begin=round(rate*20e-6);n=round(rate/15000);legacy=2*round(rate*32e-6)
    mean=270.2e9*((p-begin+(legacy-1)/2)-(n-1)/2)/rate
    r=assess_marker_structure(x,rate,p,270.2e9,mean)
    assert r['joint_span_and_prefix'],(rate,r)
    assert r['symbol_start_samples']==r['same_start_span']['start_offset_samples']
    # A passing prefix callback at a different start must not be borrowed.
    r=assess_prefix(x,rate,p,span_check=lambda start:dict(status='full_span_not_supported'))
    assert r['prefix_compatible'] and not r['joint_span_and_prefix']
print('joint structure requires passing prefix and span at the same start')
for rate in [20e6,100e6,200e6]:
    n=round(rate*120e-6);t=np.arange(n)/rate
    long=np.exp(1j*np.pi*270.2e9*(t-60e-6)**2)
    assert not assess_prefix(long,rate,round(rate*20e-6))['prefix_compatible']
for bad in [np.full(100,complex(float('nan'),0)),np.ones(65537)]:
    try:assess_prefix(bad,100e6,1)
    except ValueError:pass
    else:raise AssertionError('nonfinite/oversized IQ accepted')
print('continuous sweep, nonfinite and oversize rejection passed')
