"""Explore repeated-sample lags in frequency-localized Phantom excerpts."""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.fft import fft, ifft


def lag_peaks(iq, offset_hz):
    # Isolate +/- 3.28 MHz around the selected subband centre and decimate by 4.
    phase = -2j*np.pi*offset_hz/60e6 * np.arange(len(iq))
    taps = signal.firwin(129, 3.28e6,fs=60e6)
    filtered = signal.upfirdn(taps,iq*np.exp(phase),down=4)[32:-32]
    # Compare halves separately so a single short peak cannot pass both halves.
    scores = []
    for half in np.array_split(filtered,2):
        frames = half[:len(half)//8192*8192].reshape(-1,8192)
        frames = frames - frames.mean(axis=1,keepdims=True)
        corr = abs(ifft(abs(fft(frames,n=16384,axis=1))**2,axis=1)[:,:4097])
        scores.append(corr.sum(axis=0)/max(corr[:,0].sum(),1e-30))
    common = np.minimum(*scores)
    peaks = signal.find_peaks(common)[0]
    peaks = peaks[peaks>=32]
    order = sorted(peaks,key=lambda k:common[k],reverse=True)[:5]
    return [[int(k),round(float(common[k]),5)] for k in order]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    args=parser.parse_args()
    with (args.directory/'lags.jsonl').open('x') as out:
        for line in (args.directory/'records.jsonl').read_text().splitlines():
            row=json.loads(line)
            with open(row['path'],'rb') as f:
                for v in row['views']:
                    f.seek(v['offset']*8)
                    iq=np.fromfile(f,'<c8',count=v['samples'])
                    lo,hi=row['frequency_edges_hz'][v['band']:v['band']+2]
                    peaks=lag_peaks(iq,(lo+hi)/2-2437.5e6)
                    print(json.dumps(dict(path=row['path'],band=v['band'],offset=v['offset'],
                        interference=row['interference'],lag_rate_hz=15e6,peaks=peaks)),file=out)


if __name__=='__main__':
    main()
