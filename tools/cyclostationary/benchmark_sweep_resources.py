#!/usr/bin/env python3
"""Read-only helper exercise on dense analytic sweeps; no receiver acceptance."""
import argparse
import cmath
import json
import math
from pathlib import Path
import struct
import subprocess
import benchmark_structure as structure


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--tool',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();tool=args.tool.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False);records=[]
    for kind in ('dense_linear','three_slopes'):
        source=out/(kind+'.cf32');values=[]
        for i in range(262144):
            if kind=='dense_linear':
                # A reset every 8192 samples leaves many eligible long gates.
                span=8192;fraction=.6;direction=1
            else:
                span=1024;fraction=(.4,.5,.6)[(i//span)%3];direction=-1 if (i//span)%3==1 else 1
            t=float(i%span)-(span-1)*.5;z=cmath.exp(2j*math.pi*.5*direction*fraction/(span-1)*t*t)
            values.append(struct.pack('<ff',z.real,z.imag))
        source.write_bytes(b''.join(values));sha=structure.digest(source)
        for rate in (20_000_000,60_000_000,200_000_000):
            full=json.loads(subprocess.run([str(tool),'shadow-replay','--input',str(source),'--format','cf32_le',
                '--sample-rate',str(rate),'--capture-samples','262144'],check=True,capture_output=True,text=True,timeout=15).stdout)
            worker=full['worker'];assert worker['wire_version']==10 and worker['deadline_ms']==2000
            assert worker['address_space_limit_bytes']==134217728 and worker['processing_roundtrip_ms']<2000
            assert len(full['tiles'])==4 and full['classification']['link_family']=='unknown'
            for tile in full['tiles']:
                clock=tile['clock_refinement']
                assert clock['grids_tested']<=9 and clock['cp_trials']<=18 and clock['code_trials']<=18
                assert clock['interpolated_samples']<=11*clock['partition_samples']
            candidates=[len(tile['linear_sweep_discovery']['candidates']) for tile in full['tiles']]
            assert candidates==([1]*4 if kind=='dense_linear' else [3]*4)
            assert all(any(c['pattern_consistent'] for c in tile['linear_sweep_discovery']['candidates']) for tile in full['tiles'])
            assert structure.digest(source)==sha
            records.append({'kind':kind,'sample_rate_hz':rate,'source_sha256':sha,'worker':worker,'candidate_counts':candidates})
    root=Path(__file__).resolve().parents[2]
    summary={'scope':'software_full_copy_budget_dense_linear_and_three_slope_controls','radio_opened':False,
        'model_trained':False,'hardware_acceptance':False,'records':records,'tool_sha256':structure.digest(tool),
        'worker_sha256':structure.digest(tool.with_name('wifi_cyclo_worker')),
        'source_sha256':{str(path.relative_to(root)):structure.digest(path) for path in (Path(__file__),root/'src/cyclostationary/chirp_discovery.cpp')}}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({'records':len(records),'min_ms':min(r['worker']['processing_roundtrip_ms'] for r in records),
        'max_ms':max(r['worker']['processing_roundtrip_ms'] for r in records)}))


if __name__=='__main__':main()
