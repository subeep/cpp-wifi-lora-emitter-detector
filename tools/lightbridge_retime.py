"""Paired comparison of fixed and cyclic-prefix-derived FFT boundaries."""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly

from tools.lightbridge_anchor_grid import RATE, FS, N, CP, OFFSETS, PERIOD, phases


def window_sums(values):
    integral = np.r_[0, np.cumsum(values, dtype=np.result_type(values, np.float64))]
    return integral[CP:] - integral[:-CP]


def coherence_curve(x, start, radius):
    idx = start - CP - radius
    left = x[idx:start+radius]
    right = x[idx+N:start+radius+N]
    energy = window_sums(abs(left)**2) * window_sums(abs(right)**2)
    return abs(window_sums(right*left.conj())) / np.sqrt(np.maximum(energy, 1e-30))


def choose(curve, radius):
    # Prefer a nearby point on a broad correlation plateau.
    best = curve.max()
    if best < .7 or best < curve[radius] + .02:
        return 0
    choices = np.flatnonzero(curve >= best-.005) - radius
    return int(choices[np.argmin(abs(choices))])


def analyze(path, sample, mask):
    # Extend the original resampler input by an integer multiple of 75 samples
    # to preserve the fractional sampling phase used in the baseline.
    begin = int(np.floor(sample + (OFFSETS[0]*PERIOD-CP)*RATE/FS))-128-3000
    count = int(np.ceil(sample+(OFFSETS[-1]*PERIOD+N)*RATE/FS))+4628-begin
    if begin < 0 or (begin+count)*8 > path.stat().st_size:
        raise ValueError('Incomplete neighbourhood')
    with path.open('rb') as stream:
        stream.seek(begin*8)
        raw = np.fromfile(stream,'<c8',count=count)
    if len(raw)!=count or not np.isfinite(raw).all():
        raise ValueError('Invalid source')
    x = resample_poly(raw,56,75)
    starts = round((sample-begin)*FS/RATE)+OFFSETS*PERIOD
    anchor = starts[(OFFSETS>=0)&(OFFSETS<=3),None]+np.arange(-CP,0)
    omega = np.angle(np.vdot(x[anchor],x[anchor+N]))/N
    rotation = np.exp(-1j*omega*np.arange(N))
    curves = [coherence_curve(x,s,2048) for s in starts]
    output={}
    for radius in (0,128,2048):
        shifts = np.array([choose(c[2048-radius:2049+radius],radius) if radius and o>3 else 0
                           for o,c in zip(OFFSETS,curves)])
        fft_starts = starts+shifts
        body = x[fft_starts[:,None]+np.arange(N)]
        grid = np.fft.fftshift(np.fft.fft(body*rotation,axis=1),axes=1)
        evidence = phases(grid[None])[0][:,mask]
        cp = np.array([c[2048+d] for c,d in zip(curves,shifts)])
        # A sustained tone may correlate at many lags and symbol phases.
        # Recheck at a displaced lag and a displaced phase on the same IQ.
        idx = fft_starts[:,None] + np.arange(-CP,0)
        def control(offset, lag):
            a,b=x[idx+offset],x[idx+offset+lag]
            return abs(np.sum(a.conj()*b,axis=1))/np.maximum(
                np.linalg.norm(a,axis=1)*np.linalg.norm(b,axis=1),1e-20)
        competing=np.maximum(control(0,N+64),control(768,N))
        output[radius]=(evidence,shifts,cp,competing)
    return output


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('baseline',type=Path)
    p.add_argument('output',type=Path)
    args=p.parse_args()
    r=json.loads((args.baseline/'result.json').read_text())
    indices=np.load(args.baseline/'carrier_evidence.npz')['bins']+N//2
    results={}
    for f in r['files']:
        results[f['path']]=[analyze(Path(f['path']),s,indices) for s in f['samples']]
    modes={}
    for radius in (0,128,2048):
        means=[]
        events=[]
        byfile=[]
        for path,rows in results.items():
            a,shifts,cp,competing=map(np.array,zip(*(z[radius] for z in rows)))
            means.append(a.mean(axis=0))
            byfile.append(dict(path=path,median_cp=np.median(cp,axis=0).tolist()))
            samples=next(f['samples'] for f in r['files'] if f['path']==path)
            events.extend(dict(path=path,sample=s,shift_samples=d.tolist(),cp=c.tolist(),control=h.tolist())
                          for s,d,c,h in zip(samples,shifts,cp,competing))
        agreement=(means[0].conj()*means[1]).real.mean(axis=1)
        modes[radius]=dict(agreement=agreement.tolist(),files=byfile,events=events)
    np.testing.assert_allclose(modes[0]['agreement'],
                               [s['agreement'] for s in r['offsets']],atol=.001)
    with args.output.open('x') as out:
        json.dump(dict(offsets=OFFSETS.tolist(),modes=modes),out,allow_nan=False)


if __name__=='__main__':
    main()
