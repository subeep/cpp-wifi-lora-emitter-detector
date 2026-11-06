"""Compare pilot and train-onset hypotheses on the existing partial-band views.

Run as python -m tools.lightbridge_train_probe SCAN_DIR OUTPUT_JSON.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from tools.lightbridge_full_scan import EDGES, FFT_SIZE, RATE
from tools.lightbridge_prefix_check import channel

PERIOD = 1152
BINS = np.arange(-256, 256)
MIN_TRAIN = 8


def coherence(a, b):
    den = np.sqrt(np.sum(abs(a)**2, axis=1)*np.sum(abs(b)**2, axis=1))
    return abs(np.sum(a.conj()*b, axis=1))/np.maximum(den, 1e-30)


def extract(x):
    symbols = x[:len(x)//PERIOD*PERIOD].reshape(-1, PERIOD)
    a = symbols[:, :128]
    b = symbols[:, 1024:]
    cp = coherence(a, b)
    # Two within-symbol controls at lag 1024 require crossing into the next row.
    off = np.ones(len(cp))
    for start in (256, 768):
        shifted = x[start:start+(len(cp)-1)*PERIOD].reshape(-1, PERIOD)
        score = coherence(shifted[:, :128], shifted[:, 1024:])
        if start == 256:
            off[:-1] = score
        else:
            off[:-1] = np.maximum(off[:-1], score)
    mask = (cp > .55) & (cp-off > .25)
    edges = np.flatnonzero(np.diff(np.r_[False, mask, False]))
    for start, end in edges.reshape(-1, 2):
        if end-start < MIN_TRAIN:
            continue
        train = symbols[start:end]
        omega = np.angle(np.vdot(train[:, :128], train[:, 1024:]))/1024
        corrected = train*np.exp(-1j*omega*np.arange(train.size).reshape(train.shape))
        grid = np.fft.fft(corrected[:, 128:], axis=1)[:, BINS % 1024]
        yield int(start), int(end), float(cp[start:end].mean()), grid


def pilot_scores(grid):
    # Sign-changing pilots can have a stable squared phase. Require power
    # comparable with neighbouring occupied carriers. Scores are exploratory.
    power = np.median(abs(grid)**2, axis=0)
    valid = (power > np.median(power)*.25) & (power < np.median(power)*8)
    unit = grid/np.maximum(abs(grid), 1e-15)
    scores = abs(np.mean(unit**2, axis=0))
    return np.where(valid, scores, np.nan)


def compare(train, others):
    scores = pilot_scores(train)
    finite = np.flatnonzero(np.isfinite(scores))
    if len(finite) < 16:
        return []
    chosen = finite[np.argsort(scores[finite])[-16:]]
    # Onset waveform similarity permits circular timing offsets and common phase.
    first = train[0]
    results = []
    for record, grid in others:
        ranks = pilot_scores(grid)
        valid = np.isfinite(ranks)
        eligible = chosen[valid[chosen]]
        if eligible.size == 0:
            continue
        percentile = np.mean(ranks[eligible,None] > ranks[None,valid], axis=1)
        products = grid*first.conj()
        expanded = np.zeros((len(grid),1024),dtype=complex)
        expanded[:,BINS%1024] = products
        similarity = 1024*abs(np.fft.ifft(expanded,axis=1)).max(axis=1)
        similarity /= np.maximum(np.linalg.norm(grid,axis=1)*np.linalg.norm(first),1e-15)
        results.append(dict(target=record,eligible=int(len(eligible)),
                            pilot_mean_percentile=float(percentile.mean()),
                            onset_similarity=float(similarity[0]),
                            onset_rank_vs_interior=float(np.mean(similarity[0]>similarity[1:]))))
    return results


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    p.add_argument('output',type=Path)
    args=p.parse_args()
    if args.output.exists():
        raise ValueError('choose a new output')
    rows = []
    grids = []
    for name in ('prefix','control-prefix'):
        for line in (args.directory/(name+'.jsonl')).read_text().splitlines():
            r = json.loads(line)
            t = next(t for t in r['tests'] if t['prefix']==128)
            with open(r['path'],'rb') as f:
                f.seek(r['offset']*8)
                iq = np.fromfile(f,'<c8',count=RATE//50)
            if len(iq)!=RATE//50:
                raise ValueError('short read')
            band = r['band']
            hz = sum(EDGES[band:band+2])*RATE/(2*FFT_SIZE)
            x = channel(iq,hz)[t['phase']:]
            for start,end,cp,grid in extract(x):
                rows.append(dict(group=name,path=r['path'],band=band,offset=r['offset'],
                                 phase=t['phase'],first_symbol=start,stop_symbol=end,
                                 mean_prefix_coherence=cp,clipped_start=start==0,
                                 source_start_sample_estimate=r['offset']+64+
                                 (64+t['phase']+start*PERIOD)*RATE/11.2e6))
                grids.append(grid)
    comparisons = []
    for band in range(4):
        # One discovery train from a clean file per band. For each independent
        # filename take the longest observed train, chosen without template scores.
        pool=[i for i,r in enumerate(rows) if r['band']==band and not r['clipped_start']]
        clean=[i for i in pool if rows[i]['group']=='prefix' and '/CLEAN/' in rows[i]['path']]
        if not clean:
            continue
        ref=max(clean,key=lambda i:len(grids[i]))
        by_file={}
        for i in pool:
            path=rows[i]['path']
            if path!=rows[ref]['path'] and (path not in by_file or len(grids[i])>len(grids[by_file[path]])):
                by_file[path]=i
        comparisons.append(dict(reference=ref,band=band,top_bins=BINS[np.argsort(
            np.nan_to_num(pilot_scores(grids[ref]),nan=-1))[-16:]].tolist(),
            tests=compare(grids[ref],[(i,grids[i]) for i in by_file.values()])))
    with args.output.open('x') as f:
        json.dump(dict(trains=rows,comparisons=comparisons),f,indent=2,allow_nan=False)


if __name__=='__main__':
    main()
