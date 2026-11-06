"""Compare original wideband IQ on the proposed 10.9375 kHz OFDM raster."""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy.ndimage import median_filter, uniform_filter1d
from scipy.signal import resample_poly, correlate

FS = 44_800_000
N = 4096
CP = 512
SPACING = FS / N


def grid(row):
    count = row['stop_symbol'] - row['first_symbol']
    source = round(row['source_start_sample_estimate'])
    start = max(0, source-120)
    skip = round((source-start)*56/75)
    needed = int(np.ceil((skip + count*(N+CP)+64)*75/56))
    with open(row['path'], 'rb') as stream:
        stream.seek(start*8)
        iq = np.fromfile(stream, '<c8', count=needed)
    if len(iq) != needed or not np.isfinite(iq).all():
        raise ValueError('Incomplete or invalid wideband train')
    x = resample_poly(iq,56,75)[skip:skip+count*(N+CP)].reshape(-1,N+CP)
    a,b=x[:,:CP],x[:,N:]
    product = np.vdot(a,b)
    coherence = abs(product)/max(np.linalg.norm(a)*np.linalg.norm(b),1e-20)
    omega=np.angle(product)/N
    corrected=x*np.exp(-1j*omega*np.arange(x.size).reshape(x.shape))
    return np.fft.fftshift(np.fft.fft(corrected[:,CP:],axis=1),axes=1),float(coherence),float(omega*FS/(2*np.pi))


def profile(g):
    return 10*np.log10(np.maximum(np.mean(abs(g)**2,axis=0),1e-20))


def occupied(g):
    smooth=uniform_filter1d(median_filter(profile(g),9),25)
    # Restrict analysis to the nominal 28 MHz receiver bandwidth.
    valid=np.abs(np.arange(N)-N//2)*SPACING < 14e6
    mask=valid & (smooth>np.max(smooth[valid])-10)
    edges=np.flatnonzero(np.diff(np.r_[False,mask,False])).reshape(-1,2)
    return max(edges,key=lambda e:e[1]-e[0])


def fingerprint(g):
    p=profile(g)
    return np.clip(p-median_filter(p,17),-12,12)


def alignment(reference,target,lo,hi):
    """Select integer translation on alternating symbols using fine spectral detail."""
    ref=fingerprint(reference)[lo:hi]
    ref=ref-ref.mean()
    field=fingerprint(target)
    cross=correlate(field,ref,mode='valid')
    w=len(ref)
    sums=np.r_[0,np.cumsum(field)]
    sums2=np.r_[0,np.cumsum(field**2)]
    energy=sums2[w:]-sums2[:-w]-(sums[w:]-sums[:-w])**2/w
    scores=cross/np.maximum(np.sqrt(np.maximum(energy,0))*np.linalg.norm(ref),1e-20)
    centers=np.arange(len(scores))
    valid=(centers>=N//2-1280)&(centers+w<=N//2+1280)
    scores[~valid]=-2
    return scores


def pilot_rank(g):
    # Candidate fixed-index BPSK pilots retain phase modulo pi across symbols.
    unit=g/np.maximum(abs(g),1e-20)
    return abs(np.mean(unit**2,axis=0))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('trains',type=Path)
    p.add_argument('out',type=Path)
    args=p.parse_args()
    if args.out.exists():
        raise ValueError('Choose a fresh directory')
    args.out.mkdir(parents=True)
    rows=json.loads(args.trains.read_text())['trains']
    best={}
    for row in rows:
        n=row['stop_symbol']-row['first_symbol']
        key=(n,row['mean_prefix_coherence'])
        if n>=12 and key>best.get(row['path'],((0,0),))[0]:
            best[row['path']]=(key,row)
    ordered=sorted(best.values(),key=lambda pair:pair[0],reverse=True)
    reference,_,_=grid(ordered[0][1])
    lo,hi=map(int,occupied(reference[::2]))
    if hi-lo<32:
        raise ValueError('Reference support too narrow')
    selected=np.argsort(pilot_rank(reference[::2,lo:hi]))[-16:]
    results=[]
    for _,row in ordered:
        g,coh,cfo=grid(row)
        scores=alignment(reference[::2],g[::2],lo,hi)
        k=int(np.argmax(scores))
        hold=alignment(reference[1::2],g[1::2],lo,hi)
        pilots=pilot_rank(g[1::2,k:k+hi-lo])
        rank=float(np.mean(pilots[selected,None]>pilots[None,:]))
        left,right=map(int,occupied(g))
        results.append(dict(source=row,cp_coherence=coh,fractional_cfo_hz=cfo,
            occupied_bins=[left-N//2,right-N//2],shift_bins=k-lo,
            fit=float(scores[k]),recheck=float(hold[k]),recheck_best_shift=int(np.argmax(hold))-lo,
            pilot_percentile=rank))
        if len(results)==1:
            np.savez_compressed(args.out/'reference.npz',grid=g.astype(np.complex64),
                                pilot_bin_ids=selected+lo-N//2)
    (args.out/'results.json').write_text(json.dumps(dict(spacing_hz=SPACING,
        reference_span_bins=[lo-N//2,hi-N//2],rows=results),indent=2,allow_nan=False))


if __name__=='__main__':
    main()
