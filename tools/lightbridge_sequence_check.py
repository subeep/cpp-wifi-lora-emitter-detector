"""Measure the two unused middle symbols at previously selected two-anchor hits."""
import argparse
import json
from pathlib import Path

import numpy as np
from tools.lightbridge_same_band import read_view, P, CP, N, matched
from tools.lightbridge_prefix_check import channel
from tools.lightbridge_full_scan import RATE, EDGES, FFT_SIZE


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('trains','hits','out'):
        parser.add_argument(key,type=Path)
    args=parser.parse_args()
    row=next(r for r in json.loads(args.trains.read_text())['trains']
             if r['path'].endswith('/PHA_1000_01.dat') and r['band']==2
             and r['first_symbol']==14 and r['offset']==50772032)
    start=row['phase']+row['first_symbol']*P+CP
    x=read_view(row)
    templates=[x[start+j*P:start+j*P+N] for j in range(4)]
    similarity=[]
    for a in templates:
        pair=[]
        for b in templates:
            peak=max(abs(np.fft.ifft(np.fft.fft(a)*np.fft.fft(b).conj())))
            pair.append(float(peak/(np.linalg.norm(a)*np.linalg.norm(b))))
        similarity.append(pair)
    output=[]
    hz=sum(EDGES[2:4])*RATE/(2*FFT_SIZE)
    for rec in map(json.loads,args.hits.read_text().splitlines()):
        with open(rec['path'],'rb') as f:
            for hit in rec['hits']:
                begin=max(0,hit['sample']-6000)
                f.seek(begin*8)
                iq=np.fromfile(f,'<c8',count=36000)
                y=channel(iq,hz)
                i=round((hit['sample']-begin-64)*11.2e6/RATE-64)
                scores=[]
                for j in (1,2):
                    k=i+j*P
                    scores.append(float(max(matched(y[k-3:k+N+3],templates[j]))))
                output.append(dict(path=rec['path'],sample=hit['sample'],middle=scores))
    with args.out.open('x') as f:
        json.dump(dict(template_similarity=similarity,checks=output),f,indent=2)


if __name__=='__main__':
    main()
