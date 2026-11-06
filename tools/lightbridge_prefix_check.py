"""Test the exploratory 91.43 us repetition for a phase-localized cyclic prefix."""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy.signal import firwin, resample_poly, upfirdn


def channel(iq, frequency):
    shifted = iq*np.exp(-2j*np.pi*frequency/60e6*np.arange(len(iq)))
    filtered = upfirdn(firwin(129,3.28e6,fs=60e6),shifted,down=4)[32:-32]
    return resample_poly(filtered,56,75)[64:-64]  # 11.2 MSps


def contrast(x, lag, prefix, period):
    """Select prefix phase on first half; measure same phase on second half."""
    n = ((len(x)-lag)//period)*period
    a = x[:n].reshape(-1,period)
    b = x[lag:lag+n].reshape(-1,period)
    profiles = []
    for left,right in zip(np.array_split(a,2),np.array_split(b,2)):
        energy = np.sqrt(np.sum(abs(left)**2,axis=0)*np.sum(abs(right)**2,axis=0))
        corr = abs(np.sum(left.conj()*right,axis=0))/np.maximum(energy,1e-20)
        folded = np.convolve(np.r_[corr,corr[:prefix-1]],np.ones(prefix)/prefix,'valid')
        profiles.append(folded - corr.mean())
    phase = int(np.argmax(profiles[0]))
    return [phase,float(profiles[0][phase]),float(profiles[1][phase])]


def evaluate(iq, frequency):
    x=channel(iq,frequency)
    rows=[]
    for prefix in (32,64,128,256):
        period=1024+prefix
        match=contrast(x,1024,prefix,period)
        wrong=contrast(x,1024,prefix,period+7)
        rows.append(dict(prefix=prefix,phase=match[0],discovery=match[1],
                         confirmation=match[2],wrong_period_confirmation=wrong[2]))
    return rows


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    args=p.parse_args()
    with (args.directory/'prefix.jsonl').open('x') as out:
        for line in (args.directory/'records.jsonl').read_text().splitlines():
            row=json.loads(line)
            with open(row['path'],'rb') as f:
                for v in row['views']:
                    f.seek(v['offset']*8)
                    iq=np.fromfile(f,'<c8',count=v['samples'])
                    lo,hi=row['frequency_edges_hz'][v['band']:v['band']+2]
                    tests=evaluate(iq,(lo+hi)/2-2437.5e6)
                    print(json.dumps(dict(path=row['path'],band=v['band'],offset=v['offset'],
                        interference=row['interference'],tests=tests)),file=out)


if __name__=='__main__':
    main()
