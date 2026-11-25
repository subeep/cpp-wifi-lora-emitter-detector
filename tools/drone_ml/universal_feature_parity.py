"""Compare B3 lag facts with the production C++ probe on generated recorded views."""
import argparse
import json
from pathlib import Path
import subprocess
import numpy as np
from .universal_features import extract,NAMES,EXTRA_HYPOTHESES
from .universal_inventory import read_json
from .cp_lag_probe import BLOCK,HYPOTHESES


def run(store,worker):
    cases=[];blocks=[];expected=[]
    for path in sorted(Path(store).glob('*.npy')):
        meta=read_json(path.with_suffix('.json'))
        x=np.load(path,mmap_mode='r',allow_pickle=False)
        for index in range(len(x)):
            r=extract(x[index]);blocks.append(np.asarray(x[index,:BLOCK],dtype='<c8').tobytes())
            expected.append(list(r['lag_ratios'].values()))
            cases.append(dict(recording_id=meta['source_record']['recording_id'],dataset=meta['source_record']['dataset'],window=index))
    out=subprocess.run([str(worker)],input=b''.join(blocks),capture_output=True,check=True,timeout=120)
    actual=np.asarray([json.loads(line) for line in out.stdout.splitlines()])
    expected=np.asarray(expected)
    if actual.shape!=expected.shape:raise ValueError('missing C++ results')
    errors=np.abs(actual-expected);tolerance=np.maximum(.05,.02*np.abs(expected))
    flag_mismatch=((actual>4)!=(expected>4))&(np.abs(expected-4)>.12)
    passed=bool(np.all(errors<=tolerance) and not flag_mismatch.any())
    report=dict(schema='rfmon.drone_ml.universal_feature_parity.v1',passed=passed,
                windows=len(cases),sources=sorted({c['dataset'] for c in cases}),features=NAMES,
                timing_features_compared=list(HYPOTHESES)+list(EXTRA_HYPOTHESES),tolerance='max(0.05 absolute, 2% relative); flags must agree outside ±0.12 of threshold 4',
                max_absolute_error=float(errors.max()),max_scaled_error=float((errors/tolerance).max()),
                nonboundary_flag_mismatches=int(flag_mismatch.sum()),
                note='C++ parity covers timing facts; bandwidth/envelope/flatness have separate synthetic tests, not C++ equivalents.',cases=cases)
    if not passed:raise ValueError(json.dumps(report))
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--store',type=Path,required=True)
    p.add_argument('--worker',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=run(a.store,a.worker);a.output.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps({k:v for k,v in r.items() if k not in ('cases','features')}))
if __name__=='__main__':main()
