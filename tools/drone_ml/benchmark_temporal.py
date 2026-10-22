"""Timing-only benchmark of an untrained temporal candidate; no weights exported."""
import argparse
from pathlib import Path
import time

import numpy as np
import torch

from .common import sha256
from .live_audit import write_new_json
from .temporal_model import TemporalDroneModel,description


def benchmark(output,iterations=10):
    if not 3<=iterations<=30:raise ValueError('invalid benchmark iteration count')
    if Path(output).exists():raise FileExistsError(output)
    torch.set_num_threads(2);torch.manual_seed(17)
    model=TemporalDroneModel(21).eval()
    rows=[]
    with torch.inference_mode():
        for count in (8,16,32,64):
            x=torch.randn(1,count,1,256,625)
            for _ in range(2):model(x)
            times=[]
            for _ in range(iterations):
                start=time.perf_counter();result=model(x)
                times.append((time.perf_counter()-start)*1000)
            if not all(torch.isfinite(v).all() for v in result.values()):raise ValueError('nonfinite output')
            rows.append(dict(context_ms=count*8,tiles=count,mean_ms=float(np.mean(times)),
                median_ms=float(np.median(times)),sample_p95_ms=float(np.quantile(times,.95)),
                input_bytes=x.numel()*x.element_size()))
    report=dict(schema='rfmon.drone_ml.temporal_benchmark.v1',architecture=description(21),
        parameter_count=sum(p.numel() for p in model.parameters()),torch_version=torch.__version__,
        device='cpu',threads=2,batch_size=1,warmups=2,iterations=iterations,results=rows,
        model_source_sha256=sha256(Path(__file__).with_name('temporal_model.py')),
        pretrained_weights_used=False,weights_exported=False,
        limitations=['Synthetic normalized inputs and random weights; no accuracy measured.',
            'Forward only: excludes IQ/STFT preprocessing, queues, IPC, acquisition and display.',
            'Context acquisition delay is additional; host load uncontrolled; not a real-time guarantee.',
            'Overlapping contexts recompute CNN features here; streaming feature reuse is not implemented.'])
    write_new_json(output,report)
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--iterations',type=int,default=10);a=p.parse_args()
    r=benchmark(a.output,a.iterations)
    print(f"Untrained candidate: {r['parameter_count']} parameters; {len(r['results'])} context timings saved")


if __name__=='__main__':main()
