"""Test periodically moving, boosted pilot combs across aligned bursts."""
import argparse
import json
from pathlib import Path
import numpy as np
from scipy.ndimage import median_filter
from tools.lightbridge_wide_grid import grid, N


def tracks(g, spacing):
    power=10*np.log10(np.maximum(abs(g)**2,1e-20))
    residual=power-median_filter(power,size=(1,17))
    return np.column_stack([np.mean(residual[:,i::spacing],axis=1)
                            for i in range(spacing)])


def fit(track):
    n,k=track.shape
    time=np.arange(n)
    means=np.array([[np.mean(track[time,(p+v*time)%k]) for p in range(k)]
                    for v in range(k)])
    v,p=np.unravel_index(means.argmax(),means.shape)
    return int(v),int(p),float(means[v,p]-track.mean())


def confirm(track,slope,phase,start):
    t=np.arange(len(track))
    return float(np.mean(track[t,(phase+slope*(start+t))%track.shape[1]])-track.mean())


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    directory=p.parse_args().directory
    data=json.loads((directory/'results.json').read_text())
    rows=data['rows']
    lo,hi=np.array(data['reference_span_bins'])+N//2
    ref=grid(rows[0]['source'])[0][:,lo:hi]
    models=[]
    for k in [3,4,6,8,12,24]:
        track=tracks(ref,k)
        cut=len(track)//2
        v,phase,score=fit(track[:cut])
        models.append(dict(k=k,v=v,phase=phase,fit_db=score,
                           reference_check_db=confirm(track[cut:],v,phase,cut)))
    model=max(models,key=lambda m:m['fit_db'])
    tests=[]
    for row in rows[1:]:
        g=grid(row['source'])[0]
        off=row['shift_bins']
        track=tracks(g[:,lo+off:hi+off],model['k'])
        cut=len(track)//2
        # Frame index is unknown. Fit starting phase only, keeping spacing
        # and advance frozen, then predict the remaining symbols.
        phase=max(range(model['k']),key=lambda p:confirm(track[:cut],model['v'],p,0))
        tests.append(dict(path=row['source']['path'],phase=phase,
                          confirmation_db=confirm(track[cut:],model['v'],phase,cut)))
    with (directory/'moving_comb.json').open('x') as f:
        json.dump(dict(hypotheses=models,selected=model,targets=tests),f,indent=2)


if __name__=='__main__':
    main()
