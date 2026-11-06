"""Evaluate frozen +6 phase models on the separate sequence-discovery recording."""
import json
from pathlib import Path

import numpy as np
from scipy.special import i0, logsumexp
from tools.lightbridge_retime import analyze, OFFSETS, PERIOD, CP, FS, RATE, N
from tools.lightbridge_carrier_holdout import extract

def timing_for(path, a, carriers):
    _, shifts, cp, control = analyze(path, a, carriers+N//2)[128]
    begin = int(np.floor(a + (OFFSETS[0]*PERIOD-CP)*RATE/FS))-3128
    starts = round((a-begin)*FS/RATE) + OFFSETS*PERIOD + shifts
    timing = {int(o): dict(fft_start_sample_448=int(t)) for o,t,c,b in zip(OFFSETS,starts,cp,control) if c>.7 and c-b>.25}
    return timing

def run():
    manifest = Path.home() / "Music/lb_decode/data/validation/lightbridge-anchor-context-2026-11-06/symbol_manifest.json"
    m = json.loads(manifest.read_text())
    entries = {}
    for e in m["entries"]:
        if e["path"].endswith("PHA_0000_01.dat") and e["passes_gate"]:
            source = Path(e["path"])
            entries.setdefault(e["source_offset_sample"], {})[e["offset"]] = e
    train_ids = sorted(a for a, v in entries.items() if {0, 4, 6} <= v.keys())
    train_ids = train_ids[:len(train_ids)//2]
    carriers = np.array(m["carrier_bins"])
    def read(path, anchor, timing):
        return extract(path, anchor, timing, m, (4, 6), complex_grid=True)
    train = np.array([read(source, a, entries[a]) for a in train_ids])
    replay = Path("data/validation/lightbridge-wide-grid-v1/sequence_replay.jsonl")
    rec = next(r for r in map(json.loads, replay.read_text().splitlines()) if r["path"].endswith("PHA_1000_01.dat"))
    test, ids = [], []
    for hit in rec["hits"]:
        a = hit["sample"]
        # Exclude the neighbourhood used to discover the acquisition template.
        if 50772032 <= a <= 50772032 + 1200000:
            continue
        timing = timing_for(Path(rec["path"]), a, carriers)
        if {0,4,6} <= timing.keys():
            test.append(read(Path(rec["path"]), a, timing))
            ids.append(a)
    test = np.array(test)
    ref = np.exp(1j*np.angle(train[:,0])).mean(axis=0)
    pilots = np.argsort(-abs(ref))[:64]
    def phases(rows, slope):
        residual = np.angle(rows[:,0,pilots])-np.angle(ref[pilots])
        delays = np.arange(-4,4.01,.25) if slope else np.array([0.])
        ramps = delays[:,None]*2*np.pi*carriers/N
        fits = np.exp(1j*(residual[:,None,:]-ramps[:,pilots])).mean(axis=2)
        best = abs(fits).argmax(axis=1)
        return np.angle(rows[:,1])-np.angle(fits[np.arange(len(rows)),best])[:,None]-ramps[best]
    base = phases(train,False)
    rank = abs(np.exp(4j*base).mean(axis=0))-abs(np.exp(2j*base).mean(axis=0))
    rank[pilots] = -np.inf
    selected = np.argsort(-rank)[:64]
    results = []
    for slope in (False,True):
        tr, te = [phases(rows,slope)[:,selected] for rows in (train,test)]
        for order in (1,2,4):
            axis = np.angle(np.exp(1j*order*tr).mean(axis=0))/order
            delta = te[:,:,None]-axis[None,:,None]-np.arange(order)*2*np.pi/order
            # Equal mixture weights, fixed concentration. Uniform phase scores zero.
            ll = logsumexp(4*np.cos(delta),axis=2)-np.log(order*i0(4))
            shuffled = np.random.default_rng(42).permutation(axis)
            control_delta = delta + (axis-shuffled)[None,:,None]
            control_ll = logsumexp(4*np.cos(control_delta),axis=2)-np.log(order*i0(4))
            # Rank by strength within each burst; no target labels used.
            strength = abs(test[:,1,selected])
            strong = strength >= np.median(strength,axis=1,keepdims=True)
            results.append(dict(delay_fit=slope, states=order, mean_log_gain=float(ll.mean()),
                                shuffled=float(control_ll.mean()), strong=float(ll[strong].mean()),
                                weak=float(ll[~strong].mean())))
    print(json.dumps(dict(train=train_ids, eligible_test=ids, test_path=rec["path"], results=results,
        median_cross_amplitude=float(np.median(abs(test[:,1,selected]))))),flush=True)

if __name__ == "__main__":
    run()
