"""Map later symbols on fixed frame starts with held-out phase and power controls."""
import json
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly
from scipy.stats import spearmanr
from tools.lightbridge_wide_grid import CP, N, FS
from tools.lightbridge_full_scan import RATE


def coherence(a,b):
    return abs(np.vdot(a,b))/max(np.linalg.norm(a)*np.linalg.norm(b),1e-30)


def main():
    report = json.loads(Path("data/validation/lightbridge_frozen_full_scan.json").read_text())
    path = Path(report["acquisition"]["path"])
    starts = [r["cp_start"] for r in report["records"]]
    m = json.loads((Path.home()/"Music/lb_decode/data/validation/lightbridge-anchor-context-2026-11-06/symbol_manifest.json").read_text())
    bins = np.array(m["carrier_bins"])+N//2-914
    grids, timing = [], []
    for a in starts:
        begin = a-150
        first = round((a-begin)*FS/RATE)
        length = first+17*(N+CP)+512
        with path.open("rb") as f:
            f.seek(begin*8)
            raw = np.fromfile(f,"<c8",count=int(np.ceil(length*RATE/FS))+128)
        x = resample_poly(raw,56,75)
        if len(x)<length:
            raise ValueError("Incomplete frame neighbourhood")
        rows, gates = [], []
        for k in range(17):
            i = first+k*(N+CP)
            p = x[i:i+CP]
            c = coherence(p,x[i+N:i+N+CP])
            control = max(coherence(p,x[i+N+64:i+N+64+CP]),
                          coherence(x[i+256:i+256+CP],x[i+256+N:i+256+N+CP]))
            gates.append(c>.7 and c-control>.25)
            omega = np.angle(np.vdot(p,x[i+N:i+N+CP]))/N
            body = x[i+CP:i+CP+N]*np.exp(-1j*omega*np.arange(N))
            rows.append(np.fft.fftshift(np.fft.fft(body))[bins])
        grids.append(rows)
        timing.append(gates)
    g = np.array(grids)
    timing = np.array(timing)
    split = len(starts)//2
    out = []
    for k, matched in [(k, False) for k in range(4,17)] + [(k, True) for k in (10,11,12)]:
        good = timing[:,0]&timing[:,k]
        if matched:
            good &= timing[:,10:13].all(axis=1)
        tr = np.flatnonzero(good & (np.arange(len(g))<split))
        te = np.flatnonzero(good & (np.arange(len(g))>=split))
        row = dict(offset=k, matched_10_to_12=matched, early=len(tr), late=len(te))
        if len(tr)>=3 and len(te)>=3:
            unit = np.exp(1j*np.angle(g[:,k]*g[:,0].conj()))
            # Remove common phase using disjoint even-index carriers only.
            ref = unit[tr[0],::2]
            rotation = np.angle((unit[:,::2]*ref.conj()).mean(axis=1))
            aligned = unit[:,1::2]*np.exp(-1j*rotation[:,None])
            template = np.angle(aligned[tr].mean(axis=0))
            shuffled = np.random.default_rng(42).permutation(template)
            def agree(t):
                return float((np.cos(np.angle(aligned[te])-t)>np.cos(np.pi/4)).mean())
            # Relative power profile suppresses a stationary receive-channel envelope.
            db = 20*np.log10((abs(g[:,k,1::2])+1e-12)/(abs(g[:,0,1::2])+1e-12))
            profile = np.median(db[tr],axis=0)
            perm = np.random.default_rng(42).permutation(profile)
            def power_rank(p):
                return float(np.median([spearmanr(p,db[i]).statistic for i in te]))
            row.update(phase=agree(template),shuffled_phase=agree(shuffled),
                       relative_power_rank=power_rank(profile),shuffled_power_rank=power_rank(perm))
        out.append(row)
    print(json.dumps(dict(path=str(path),starts=starts,offsets=out),indent=2))


if __name__ == "__main__":
    main()
