"""Compare prefix train neighbourhoods in the same recording and RF region."""
import argparse
from collections import defaultdict
import json
from pathlib import Path

import numpy as np
from scipy.signal import correlate
from tools.lightbridge_prefix_check import channel
from tools.lightbridge_full_scan import EDGES, RATE, FFT_SIZE

P, CP, N = 1152, 128, 1024
OFFSETS = range(-3, 4)


def read_view(row):
    with open(row['path'], 'rb') as stream:
        stream.seek(row['offset']*8)
        iq=np.fromfile(stream,'<c8',count=RATE//50)
    if len(iq)!=RATE//50 or not np.isfinite(iq).all():
        raise ValueError('Invalid input window')
    b=row['band']
    return channel(iq,sum(EDGES[b:b+2])*RATE/(2*FFT_SIZE))


def matched(x, template):
    energy=np.r_[0.,np.cumsum(abs(x)**2)]
    den=np.sqrt(np.maximum(energy[N:]-energy[:-N],0))*np.linalg.norm(template)
    return abs(correlate(x,template,mode='valid',method='fft'))/np.maximum(den,1e-20)


def compare(a,b):
    x=read_view(a)
    first=a['phase']+a['first_symbol']*P
    second=b['phase']+b['first_symbol']*P
    end=b['phase']+b['stop_symbol']*P
    # Limit the search to the second train and three symbols of leading context.
    start=max(0,second-3*P)
    target=x[start:end]
    rows=[]
    for offset in OFFSETS:
        i=first+offset*P+CP
        if i<0 or i+N>start:
            continue
        t=x[i:i+N]
        power=abs(np.fft.fft(t))**2
        bandwidth=float(power.sum()**2/max(np.sum(power**2),1e-30))
        scores=matched(target,t)
        pred=second+offset*P+CP-start
        if pred-16<0 or pred+16>=len(scores):
            continue
        peak=int(np.argmax(scores))
        nearby=int(np.argmax(scores[pred-16:pred+17]))+pred-16
        outside=np.abs(np.arange(len(scores))-pred)>N//2
        rows.append(dict(offset=offset,effective_bins=bandwidth,
                         expected_score=float(scores[nearby]),other_max=float(scores[outside].max()),
                         best_score=float(scores[peak]),best_error_samples=peak-pred))
    return rows


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('trains',type=Path)
    p.add_argument('output',type=Path)
    args=p.parse_args()
    groups=defaultdict(list)
    for row in json.loads(args.trains.read_text())['trains']:
        groups[row['path'],row['offset'],row['band']].append(row)
    results=[]
    for key in sorted(groups):
        trains=sorted(groups[key],key=lambda r:r['first_symbol'])
        for a,b in zip(trains,trains[1:]):
            if a['clipped_start'] or b['first_symbol']-a['stop_symbol']<4:
                continue
            results.append(dict(path=key[0],offset=key[1],band=key[2],first=a['first_symbol'],
                second=b['first_symbol'],gap_symbols=b['first_symbol']-a['stop_symbol'],
                tests=compare(a,b)))
    with args.output.open('x') as f:
        json.dump(results,f,indent=2,allow_nan=False)


if __name__=='__main__':
    main()
