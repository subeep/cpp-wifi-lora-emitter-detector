"""B3 measured facts for one 8 ms deployment view; no learned or model labels."""
from dataclasses import dataclass
import numpy as np
from .cp_lag_probe import BLOCK, HYPOTHESES, lag_profile

RATE = 20_000_000
# Facts v2 (24 Nov 2026): Lightbridge's own useful symbol, 4096 points at 44.8 MSps = 91.43 us
# (10.9375 kHz spacing), as the app's C++ probe has used since 20 Nov (symbol_timing.hpp). The
# 32.8 kHz lag in HYPOTHESES is a third of it; on the user's X310 Phantom it is weak while this
# one is strong. Appended after the v1 facts, so the first 29 facts are unchanged and a package
# trained on v1 reads only those (`universal_c2_train.Runner` cuts to the package's count).
EXTRA_HYPOTHESES = {'lightbridge_10.9375kHz': 1 / 10937.5}
NAMES = ([f'lag_ratio:{name}' for name in HYPOTHESES] +
         [f'lag_value:{name}' for name in HYPOTHESES] +
         [name for i in range(6) for name in (f'blind_{i}_useful_us',f'blind_{i}_ratio')] +
         ['occupied_bandwidth_hz','spectral_flatness','burst_length_s','burst_period_s','duty_cycle'] +
         [f'lag_{kind}:{name}' for name in EXTRA_HYPOTHESES for kind in ('ratio','value')])
NAMES_V1 = NAMES[:29]
VERSION='universal_measured_facts_v2'


def lag_fact(profile, t):
    """(value, ratio, valid) of the lag profile at useful-symbol duration t against nearby lags."""
    lag=RATE*t
    near=[int(np.floor(lag)),int(np.ceil(lag))] if abs(lag-round(lag))>1e-6 else [int(round(lag))]
    lo,hi=near[0]-50,near[-1]+51
    mask=np.ones(hi-lo,bool);mask[near[0]-lo-3:near[-1]-lo+4]=False
    base=float(np.median(profile[lo:hi][mask])) if profile is not None else 0.
    value=float(profile[near].max()) if profile is not None else 0.
    return value,(value/base if base>0 else 0.),base>0


def extract(iq):
    x=np.asarray(iq,dtype=np.complex64)
    if x.ndim!=1 or x.size!=160000 or not np.isfinite(x).all():
        raise ValueError('finite 8 ms view at 20 MSps required')
    profile=lag_profile(x[:BLOCK],round(RATE*150e-6))
    values=[];ratios={};valid=[]
    # Keep the first 131072 samples exactly aligned with the C++ production probe.
    for name,t in HYPOTHESES.items():
        value,ratio,ok=lag_fact(profile,t)
        ratios[name]=ratio;values.append(value);valid.append(ok)
    features=list(ratios.values())+values
    available=valid+valid
    peaks=[]
    if profile is not None:
        windows=np.lib.stride_tricks.sliding_window_view(profile[8:],101)
        for k in np.flatnonzero(windows[:,50]==windows.max(axis=1)):
            base=float(np.median(windows[k]));value=float(windows[k,50]);lag=int(k)+58
            if base>0 and value>3*base and lag<len(profile)-50:
                peaks.append(dict(useful_us=lag/RATE*1e6,ratio=value/base))
        peaks=sorted(peaks,key=lambda p:-p['ratio'])[:6]
    for i in range(6):
        features.extend([peaks[i]['useful_us'],peaks[i]['ratio']] if i<len(peaks) else [0.,0.])
        available.extend([i<len(peaks)]*2)
    frames=x[:len(x)//4096*4096].reshape(-1,4096).astype(np.complex128)
    frames-=frames.mean(axis=1,keepdims=True)
    spectrum=np.mean(np.abs(np.fft.fftshift(np.fft.fft(frames*np.hanning(4096),axis=1),axes=1))**2,axis=0)
    total=float(spectrum.sum())
    if total>0:
        cumulative=np.cumsum(spectrum)/total
        left,right=np.searchsorted(cumulative,[.005,.995])
        bandwidth=(right-left+1)*RATE/4096
        flatness=float(np.exp(np.mean(np.log(np.maximum(spectrum,1e-30))))/spectrum.mean())
    else:bandwidth,flatness=0.,0.
    envelope=np.mean(np.abs(x.reshape(-1,256).astype(np.complex128))**2,axis=1)
    floor=float(np.quantile(envelope,.1))
    active=envelope>max(floor*10**.6,1e-20)
    edges=np.diff(np.r_[False,active,False].astype(np.int8))
    starts,ends=np.flatnonzero(edges==1),np.flatnonzero(edges==-1)
    complete=(starts>0)&(ends<len(active))
    lengths=(ends-starts)[complete]*256/RATE
    intervals=np.diff(starts)*256/RATE
    features.extend([bandwidth,flatness,float(np.median(lengths)) if len(lengths) else 0.,
                     float(np.median(intervals)) if len(intervals) else 0.,float(active.mean())])
    available.extend([total>0,total>0,bool(len(lengths)),bool(len(intervals)),total>0])
    for name,t in EXTRA_HYPOTHESES.items():
        value,ratio,ok=lag_fact(profile,t)
        ratios[name]=ratio;features.extend([ratio,value]);available.extend([ok,ok])
    result=np.asarray(features,dtype=np.float32)
    if len(result)!=len(NAMES) or not np.isfinite(result).all():raise ValueError('invalid measured facts')
    return dict(version=VERSION,values=result,available=np.asarray(available,bool),lag_ratios=ratios,
                blind_peaks=peaks,burst_count=len(starts),burst_complete_count=int(complete.sum()),
                envelope_threshold='6 dB over within-window quiet decile; activity proxy only',
                timing_block_samples=BLOCK,rate_hz=RATE)
