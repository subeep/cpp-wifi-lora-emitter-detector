"""Measure differential carrier recurrence around externally located useful-symbol starts."""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly
from tools.lightbridge_wide_grid import FS, N, CP, SPACING

RATE = 60_000_000
OFFSETS = np.arange(-8, 17)
PERIOD = N + CP


def extract(path, sample):
    # Keep the filter transient outside all observed symbol intervals.
    begin = int(np.floor(sample + (OFFSETS[0]*PERIOD-CP)*RATE/FS)) - 128
    end = int(np.ceil(sample + (OFFSETS[-1]*PERIOD+N)*RATE/FS)) + 128
    if begin < 0 or end*8 > path.stat().st_size:
        return None
    with path.open('rb') as f:
        f.seek(begin*8)
        iq = np.fromfile(f, '<c8', count=end-begin)
    if len(iq)!=end-begin or not np.isfinite(iq).all():
        raise ValueError('Invalid anchor interval')
    x = resample_poly(iq,56,75)
    starts = round((sample-begin)*FS/RATE) + OFFSETS*PERIOD
    body = x[starts[:,None]+np.arange(N)]
    prefix = x[starts[:,None]+np.arange(-CP,0)]
    tail = body[:,-CP:]
    cross = np.sum(tail*prefix.conj(),axis=1)
    cp = abs(cross)/np.maximum(np.linalg.norm(tail,axis=1)*np.linalg.norm(prefix,axis=1),1e-20)
    omega = np.angle(cross[(OFFSETS>=0)&(OFFSETS<4)].sum())/N
    grid = np.fft.fftshift(np.fft.fft(body*np.exp(-1j*omega*np.arange(N)),axis=1),axes=1)
    return grid, cp


def phases(grid):
    # Adjacent carrier phase differences cancel common symbol phase.
    # Relating each symbol to symbol zero cancels static channel phase slopes.
    d = grid[...,1:]*grid[...,:-1].conj()
    unit = d/np.maximum(abs(d),1e-25)
    return unit*unit[:,OFFSETS==0,:].conj()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('checks',type=Path)
    p.add_argument('output',type=Path)
    args=p.parse_args()
    if args.output.exists():
        raise ValueError('Choose a new output directory')
    checks=json.loads(args.checks.read_text())['checks']
    groups={}
    excluded=[]
    for row in checks:
        path=Path(row['path'])
        if path.stem=='PHA_1000_01' or min(row['middle'])<.35:
            continue
        result=extract(path,row['sample'])
        if result is None:
            excluded.append(row)
            continue
        groups.setdefault(str(path),[]).append((row['sample'],*result))
    if len(groups)!=2:
        raise ValueError('Expected two independent comparison filenames')
    args.output.mkdir(parents=True)
    summaries=[]
    templates=[]
    observed=[]
    cp_by_file=[]
    # First file supplies the power mask. Restrict to measured nominal passband.
    for path,entries in groups.items():
        grids=np.array([g for _,g,_ in entries])
        cp=np.array([c for _,_,c in entries])
        cp_by_file.append(cp)
        powers=abs(grids)**2
        if not templates:
            ref=np.median(powers[:,OFFSETS==0,:],axis=(0,1))
            band=(abs(np.arange(N)-N//2)*SPACING<14e6)
            band &= np.abs(np.arange(N)-N//2)>4
            good=band & (ref>ref[band].max()/100)
            mask=good[:-1]&good[1:]
        u=phases(grids)[:,:,mask]
        signature=u.mean(axis=0)
        templates.append(signature)
        observed.append(u)
        energy=powers[:,:,:-1][:,:,mask].mean(axis=2)
        relative=10*np.log10(np.maximum(energy,1e-30)/energy[:,OFFSETS==0])
        summaries.append(dict(path=path,samples=[s for s,_,_ in entries],
            cp=np.median(cp,axis=0).tolist(),relative_db=np.median(relative,axis=0).tolist(),
            within_coherence=abs(signature).mean(axis=1).tolist()))
    ref,other=templates
    agreement=(ref.conj()*other).real
    # Fix bin and sign using file A. In B measure phase agreement without refitting.
    reliable=(abs(ref)>.75)
    heldout=np.real(ref.conj()*other)/np.maximum(abs(ref),1e-20)
    indices=np.flatnonzero(mask)-N//2
    np.savez_compressed(args.output/'carrier_evidence.npz',offsets=OFFSETS,bins=indices,
                        first=ref,second=other,heldout_phase_agreement=heldout)
    rows=[]
    for i,offset in enumerate(OFFSETS):
        bins=indices[reliable[i] & (heldout[i]>.75)]
        rows.append(dict(offset=int(offset),agreement=float(agreement[i].mean()),
                         reference_selected=int(reliable[i].sum()),repeated_bins=bins.tolist()))
    selected_band=(indices*SPACING>=0) & ((indices+1)*SPACING<=6.5625e6)
    for i,row in enumerate(rows):
        row['localizing_band_agreement']=float(agreement[i,selected_band].mean())
        row['other_band_agreement']=float(agreement[i,~selected_band].mean())
    for idx,u in enumerate(observed):
        per_anchor=(u*templates[1-idx].conj()).real.mean(axis=2)
        shoulder=per_anchor[:,(OFFSETS>=4)&(OFFSETS<=7)].mean(axis=1)
        later=per_anchor[:,(OFFSETS>=9)&(OFFSETS<=12)].mean(axis=1)
        summaries[idx]['shoulder_above_later']=int(np.sum(shoulder>later))
        summaries[idx]['median_difference']=float(np.median(shoulder-later))
        # Sensitivity check for loss of OFDM timing in later positions.
        cp=cp_by_file[idx]
        timed=(np.min(cp[:,(OFFSETS>=4)&(OFFSETS<=7)],axis=1)>.7)
        timed &= (np.min(cp[:,(OFFSETS>=9)&(OFFSETS<=12)],axis=1)>.7)
        summaries[idx]['both_ranges_timed']=int(timed.sum())
        summaries[idx]['timed_shoulder_above_later']=int(np.sum(shoulder[timed]>later[timed]))
    (args.output/'result.json').write_text(json.dumps(dict(files=summaries,excluded=excluded,
        bins=int(mask.sum()),localizing_pairs=int(selected_band.sum()),offsets=rows),indent=2,allow_nan=False))


if __name__=='__main__':
    main()
