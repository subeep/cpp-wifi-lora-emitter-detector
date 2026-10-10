import sys
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/cyclostationary'))
from marker_symbol import assess_symbol


def signal(rate,duration=1/15000,slope=270.2e9,start=20e-6,carrier=0):
    n=round(rate*140e-6);begin=round(rate*start);end=begin+round(rate*duration)
    x=np.zeros(n,dtype=complex)
    t=(np.arange(end-begin)/rate-duration/2)
    x[begin:end]=np.exp(2j*np.pi*(carrier*t+slope*t*t/2))
    peak=begin+round(rate*1e-6)
    legacy=2*round(rate*32e-6)
    mean=carrier+slope*((peak-begin+(legacy-1)/2)/rate-duration/2)
    return x,peak,mean

for rate in [20e6,25e6,100e6,120e6,200e6]:
    for slope in [135.2e9,270.2e9,-135.3e9]:
        x,p,f=signal(rate,slope=slope)
        r=assess_symbol(x,rate,p,slope,f)
        assert r['status']=='span_with_boundary_contrast',(rate,slope,r)
        assert not r['family_accepted'] and not r['full_symbol_duration_established']
        assert r['eligible_trials']<=17
    x,p,f=signal(rate,duration=64e-6)
    assert not assess_symbol(x,rate,p,270.2e9,f)['span_supported']
    x,p,f=signal(rate,duration=100e-6)
    # Use a marker in the interior of a longer chirp: full span alone must not pass boundaries.
    p+=round(rate*10e-6);f+=270.2e9*10e-6
    r=assess_symbol(x,rate,p,270.2e9,f)
    assert r['status']=='span_without_boundary_contrast',r
    x,p,f=signal(rate)
    assert not assess_symbol(x,rate,p,135.2e9,f)['span_supported']
    assert not assess_symbol(x,rate,p,270.2e9,f+2e6)['span_supported']
    rng=np.random.default_rng(129)
    noise=rng.normal(size=len(x))+1j*rng.normal(size=len(x))
    assert not assess_symbol(noise,rate,p,270.2e9,0)['span_supported']
    tone=np.exp(2j*np.pi*1e6*np.arange(len(x))/rate)
    assert not assess_symbol(tone,rate,p,270.2e9,0)['span_supported']
    assert assess_symbol(np.zeros_like(x),rate,p,270.2e9,0)['status']=='no_energy'
    edge=x[round(rate*20e-6):]
    assert assess_symbol(edge,rate,round(rate*1e-6),270.2e9,f)['status']=='insufficient_boundary_context'
for x,rate,peak,slope,f in [(np.ones(65537),100e6,0,270.2e9,0),
    (np.array([complex(float('nan'),0)]),100e6,0,270.2e9,0),
    (np.ones(100),float('nan'),0,270.2e9,0),(np.ones(100),100e6,-1,270.2e9,0),
    (np.ones(100),100e6,0,1,0),(np.ones(100),100e6,0,270.2e9,float('inf'))]:
    try:assess_symbol(x,rate,peak,slope,f)
    except ValueError:pass
    else:raise AssertionError('invalid input accepted')
print('full-span, boundary, negative and budget checks passed at five sample rates')
x,p,f=signal(16e6)
assert assess_symbol(x,16e6,p,270.2e9,f)['status']=='unsupported_sample_rate'
x,p,f=signal(25e6,carrier=500000)
assert assess_symbol(x,25e6,p,270.2e9,f)['span_supported']
for amplitude in [1e-200,1e200]:
    assert assess_symbol(x*amplitude,25e6,p,270.2e9,f)['span_supported']
print('unsupported bandwidth, carrier offset and extreme scaling checks passed')
