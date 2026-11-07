"""Offline CP-first replay of one Phantom capture against the frozen +4 template."""
import argparse
import json
from pathlib import Path

import numpy as np
from tools.lightbridge_carrier_holdout import extract as anchor_phase, score
from tools.lightbridge_full_scan import CELL, CENTER, EDGES, FFT_SIZE, RATE
from tools.lightbridge_prefix_check import channel, contrast
from tools.lightbridge_train_probe import extract as find_trains, PERIOD
from tools.lightbridge_wide_grid import grid, N, SPACING
from scipy.signal import find_peaks
from scipy.ndimage import uniform_filter1d


def spectral_shift(reference, spectra, bins):
    """Align smoothed log-power envelopes without using +4 phase scores."""
    def envelope(power):
        return uniform_filter1d(10*np.log10(np.maximum(power,1e-20)),33)
    ref = envelope(reference)[bins+N//2]
    ref = ref-ref.mean()
    field = envelope(np.median(spectra,axis=0))
    shifts = np.arange(-N//2-bins.min(), N//2-bins.max())
    fields = field[shifts[:,None]+bins+N//2]
    fields -= fields.mean(axis=1,keepdims=True)
    values = fields@ref/np.maximum(np.linalg.norm(fields,axis=1)*np.linalg.norm(ref),1e-20)
    return int(shifts[values.argmax()])


def local_pairs(x):
    """Find local prefix timing at both symbols; allow interruptions in between."""
    lag, width = 1024, 128
    def sums(values):
        prefix = np.r_[0, np.cumsum(values, dtype=np.result_type(values, np.float64))]
        return prefix[width:] - prefix[:-width]
    a, b = x[:-lag], x[lag:]
    coh = abs(sums(a.conj()*b))/np.sqrt(np.maximum(sums(abs(a)**2)*sums(abs(b)**2), 1e-30))
    gap = 4*PERIOD
    n = len(coh)-gap-256
    quality = np.minimum(coh[:n], coh[gap:gap+n])
    control = np.maximum(coh[256:256+n],coh[gap+256:gap+256+n])
    peaks, _ = find_peaks(np.where(quality-control>.25,quality,0),height=.55,distance=PERIOD//2)
    return [(int(i),5,float(quality[i])) for i in peaks]


def main():
    directory = Path("data/validation/lightbridge-full-2026-11-06-b")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, nargs="?", default=Path.home() / "Pictures/DroneDetect_V2/CLEAN/PHA_ON/PHA_0000_02.dat")
    parser.add_argument("--local-pairs", action="store_true")
    parser.add_argument("--spectral-shift", action="store_true", help="Estimate one shift per band on early windows, then score later windows")
    args = parser.parse_args()
    path = args.path.resolve()
    m = json.loads((Path.home() / "Music/lb_decode/data/validation/lightbridge-anchor-context-2026-11-06/symbol_manifest.json").read_text())
    entries = {}
    for e in m["entries"]:
        if e["path"].endswith("PHA_0000_01.dat") and e["passes_gate"]:
            src = Path(e["path"])
            entries.setdefault(e["source_offset_sample"], {})[e["offset"]] = e
    ids = sorted(a for a,v in entries.items() if {0,4,5} <= v.keys())
    phases = [anchor_phase(src,a,entries[a],m)[0] for a in ids[:len(ids)//2]]
    resultant = np.exp(1j*np.asarray(phases)).mean(axis=0)
    ranked = np.argsort(-abs(resultant))[:128]
    pilots, targets = ranked[::2], ranked[1::2]
    axes = np.angle(resultant)
    rng = np.random.default_rng(42)
    templates = [axes]
    for _ in range(99):
        perm = axes.copy()
        perm[targets] = rng.permutation(axes[targets])
        templates.append(perm)

    power = np.load(directory/(path.stem+".npy"))
    bins = np.asarray(m["carrier_bins"])
    reference = None
    frozen = {}
    if args.spectral_shift:
        spectra = []
        for a in ids[:len(ids)//2]:
            g,_,_ = grid(dict(path=str(src),first_symbol=0,stop_symbol=5,
                              source_start_sample_estimate=a-128*RATE/11.2e6))
            spectra.append(np.mean(abs(g)**2,axis=0))
        reference = np.median(spectra,axis=0)
    maxima = np.zeros(len(templates))
    rows = []
    window = RATE//50
    total = path.stat().st_size//8
    for band in range(4):
        hz = sum(EDGES[band:band+2])*RATE/(2*FFT_SIZE)
        starts = []
        # Sample the strongest cell from each chronological quarter of this band.
        for indices in np.array_split(np.arange(len(power)),4):
            peak = indices[np.argmax(power[indices,band])]
            starts.append(int(np.clip(peak*CELL-window//2,0,total-window)))
        early_spectra = []
        for quarter,start in enumerate(starts):
            if args.spectral_shift and quarter == 2 and early_spectra:
                frozen[band] = spectral_shift(reference,early_spectra,bins)
            with path.open("rb") as f:
                f.seek(start*8)
                iq = np.fromfile(f,"<c8",count=window)
            x = channel(iq,hz)
            if args.local_pairs:
                candidates = local_pairs(x)
            else:
                phase = contrast(x,1024,128,PERIOD)[0]
                candidates = [(phase+first*PERIOD,end-first,cp) for first,end,cp,_ in find_trains(x[phase:])]
            trains = []
            for position,count,cp in candidates:
                sample = start+64+(64+position)*RATE/11.2e6
                row = dict(path=str(path),first_symbol=0,stop_symbol=count,
                           source_start_sample_estimate=sample)
                g,_,_ = grid(row)
                if args.spectral_shift and quarter < 2:
                    early_spectra.append(np.mean(abs(g)**2,axis=0))
                    continue
                best = 0.
                pair_scores = np.zeros(len(g)-4)
                # Same absolute carrier raster and translation from discovery band 2.
                shifts = [frozen[band]] if band in frozen else []
                if not args.spectral_shift:
                    shifts = sorted({0,round((band-2)*56*RATE/FFT_SIZE/SPACING)})
                for shift in shifts:
                    idx = bins+N//2+shift
                    if idx.min()<0 or idx.max()>=N:
                        continue
                    values = np.angle(g[4:,idx]*g[:-4,idx].conj())
                    trial = np.array([score(values,t,pilots,targets).max() for t in templates])
                    pair_scores = np.maximum(pair_scores,score(values,axes,pilots,targets))
                    maxima = np.maximum(maxima,trial)
                    best = max(best,float(trial[0]))
                trains.append(dict(source_sample=round(sample),symbols=count,cp=cp,best=best,pair_scores=pair_scores.tolist()))
            rows.append(dict(band=band,start=start,trains=trains))
    print(json.dumps(dict(path=str(path),edges_MHz=[(CENTER+e*RATE/FFT_SIZE)/1e6 for e in EDGES],
        map_cells=len(power),mean_band_power_fraction=(power.sum(axis=0)/power.sum()).tolist(),
        windows=rows,spectral_shifts=frozen,best=maxima[0],shuffled_maxima=maxima[1:].tolist())))


if __name__ == "__main__":
    main()
