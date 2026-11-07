"""Search complete captures for two fixed symbols from the discovery excerpt."""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy.signal import find_peaks

from tools.lightbridge_same_band import read_view, matched, P, CP, N
from tools.lightbridge_prefix_check import channel
from tools.lightbridge_full_scan import RATE, FFT_SIZE, EDGES


def search(path, band, templates, frequency_shift_hz=0.):
    hz=sum(EDGES[band:band+2])*RATE/(2*FFT_SIZE)+frequency_shift_hz
    samples=path.stat().st_size//8
    step=RATE//20
    hits=[]
    best=0.
    with path.open('rb') as stream:
        for start in range(0,samples,step):
            # Read overlap for channel filter transients and both symbol windows.
            read_start=max(0,start-12000)
            stream.seek(read_start*8)
            iq=np.fromfile(stream,'<c8',count=min(samples-read_start,start+step+36000-read_start))
            x=channel(iq,hz)
            a=matched(x,templates[0])
            b=matched(x,templates[1])
            n=len(a)-3*P
            if n<=0:
                continue
            joint=np.minimum(a[:n],b[3*P:])
            source=read_start+64+(64+np.arange(n))*RATE/11.2e6
            ok=(source>=start)&(source<start+step)
            best=max(best,float(np.max(joint[ok])))
            peaks=find_peaks(joint,height=.35,distance=N)[0]
            for j in peaks[ok[peaks]]:
                hits.append(dict(sample=round(float(source[j])),score=float(joint[j])))
    return dict(path=str(path),samples=samples,best_joint=best,hits=hits)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('trains',type=Path)
    p.add_argument('output',type=Path)
    p.add_argument('comparisons',nargs='*',type=Path)
    args=p.parse_args()
    rows=json.loads(args.trains.read_text())['trains']
    row,=[r for r in rows if Path(r['path']).stem=='PHA_1000_01' and r['band']==2
          and r['first_symbol']==14 and r['offset']==50772032]
    x=read_view(row)
    i=row['phase']+row['first_symbol']*P+CP
    templates=[x[i+k*P:i+k*P+N] for k in (0,3)]
    with args.output.open('x') as out:
        for path in [Path(row['path']),*args.comparisons]:
            result=search(path,row['band'],templates)
            print(json.dumps(result),file=out,flush=True)
            print(path.name,len(result['hits']),result['best_joint'],flush=True)


if __name__=='__main__':
    main()
