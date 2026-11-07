"""Replay the two shifted +4 candidates without a timing or frequency search."""
import argparse
import json
from tools.lightbridge_sequence_replay import search
from pathlib import Path

import numpy as np

from tools.lightbridge_adjacent_phase import evaluate
from tools.lightbridge_carrier_holdout import extract, score
from tools.lightbridge_full_scan import RATE, EDGES, FFT_SIZE
from tools.lightbridge_same_band import channel, read_view, P, CP, N
from tools.lightbridge_wide_grid import grid, SPACING


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--full-scan", action="store_true")
    full_scan = parser.parse_args().full_scan
    data = Path("data/validation")
    result = json.loads((data/"lightbridge_spectral_PHA_0000_02.json").read_text())
    target = Path(result["path"])
    manifest = Path.home()/"Music/lb_decode/data/validation/lightbridge-anchor-context-2026-11-06/symbol_manifest.json"
    m = json.loads(manifest.read_text())
    entries = {}
    for e in m["entries"]:
        if e["path"].endswith("PHA_0000_01.dat") and e["passes_gate"]:
            source = Path(e["path"])
            entries.setdefault(e["source_offset_sample"], {})[e["offset"]] = e
    def half(offsets):
        ids = sorted(a for a, v in entries.items() if set(offsets) <= v.keys())
        return ids[:len(ids)//2]
    train = [extract(source,a,entries[a],m)[0] for a in half((0,4,5))]
    ref = np.exp(1j*np.array(train)).mean(axis=0)
    ranks = np.argsort(-abs(ref))[:128]
    pilots, targets = ranks[::2], ranks[1::2]
    train6 = np.array([extract(source,a,entries[a],m,(4,5,6),True)
                       for a in half((0,4,6)) if 5 in entries[a]])

    trains = json.loads((data/"lightbridge-full-2026-11-06-b/trains.json").read_text())["trains"]
    row = next(r for r in trains if Path(r["path"]).stem=="PHA_1000_01" and r["band"]==2
               and r["first_symbol"]==14 and r["offset"]==50772032)
    view = read_view(row)
    start = row["phase"] + row["first_symbol"]*P + CP
    templates = np.array([view[start+j*P:start+j*P+N] for j in range(4)])
    hz = sum(EDGES[2:4])*RATE/(2*FFT_SIZE)
    def correlations(path, cp_start, shift):
        begin = cp_start-6000
        with path.open("rb") as stream:
            stream.seek(begin*8)
            y = channel(np.fromfile(stream,"<c8",count=42000), hz+shift*SPACING)
        i = round((cp_start-begin-64)*11.2e6/RATE-64)+CP
        segments = np.array([y[i+j*P:i+j*P+N] for j in range(4)])
        return (abs(segments @ templates.conj().T) /
                (np.linalg.norm(segments,axis=1)[:,None]*np.linalg.norm(templates,axis=1))).tolist()
    records = []
    acquisition = None
    positions = [75624398,119368553]
    if full_scan:
        acquisition = search(target, 2, templates[[0,3]], -914*SPACING)
        positions = [round(h["sample"]-CP*RATE/11.2e6) for h in acquisition["hits"]]
    for a in positions:
        bins = np.array(m["carrier_bins"])+4096//2-914
        r = dict(path=str(target),first_symbol=0,source_start_sample_estimate=a)
        five = grid(dict(r,stop_symbol=5))[0][:,bins]
        seven = grid(dict(r,stop_symbol=7))[0][:,bins]
        spectra = seven[[4,5,6]]*seven[0].conj()
        six, _ = evaluate(train6,spectra[None])
        def four_score(cross):
            return float(score(np.angle(cross)[None],np.angle(ref),pilots,targets)[0])
        records.append(dict(cp_start=a,sequence=correlations(target,a,-914),
                            plus4=four_score(five[4]*five[0].conj()),plus5=four_score(spectra[1]),
                            plus6=six["relative_to_zero"]))
    positive = Path(row["path"])
    hit = 3310714
    print(json.dumps(dict(records=records,acquisition=acquisition,positive_sample=hit,
                          positive_sequence=correlations(positive,round(hit-CP*RATE/11.2e6),0))))


if __name__ == "__main__":
    main()
