#!/usr/bin/env python3
"""Full-budget isolated helper burst workload; no receiver performance acceptance."""
import argparse
import cmath
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import benchmark_structure as structure


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--tool',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();tool=args.tool.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False);records=[]
    for kind in ('long_conjugate','long_three_slopes','eight_regions'):
        source=out/(kind+'.cf32');rng=random.Random(10101631);values=[]
        for tile in range(4):
            for i in range(65536):
                z=.001*complex(rng.gauss(0,1),rng.gauss(0,1))
                if kind=='eight_regions':
                    active=2048<=i<18432 or (20480<=i<49152 and (i-20480)%4096<3072)
                    if active:z+=sum(cmath.exp(2j*math.pi*b*(i%512)/512) for b in (-64,32,96))
                elif 16384<=i<49152:
                    if kind=='long_conjugate':z+=rng.choice((-1,1))*cmath.exp(1j*math.pi*i/128)
                    else:
                        span=1024;fraction=(.4,.5,.6)[(i//span)%3];direction=-1 if (i//span)%3==1 else 1
                        t=float(i%span)-(span-1)*.5;z+=cmath.exp(2j*math.pi*.5*direction*fraction/(span-1)*t*t)
                values.append(struct.pack('<ff',z.real,z.imag))
        source.write_bytes(b''.join(values));sha=structure.digest(source)
        for rate in (20_000_000,60_000_000,200_000_000):
            full=json.loads(subprocess.run([str(tool),'shadow-replay','--input',str(source),'--format','cf32_le',
                '--sample-rate',str(rate),'--capture-samples','262144'],check=True,capture_output=True,text=True,timeout=15).stdout)
            w=full['worker'];assert w['wire_version']==10 and w['process_isolated'] and w['deadline_ms']==2000
            assert w['address_space_limit_bytes']==134217728 and w['cpu_lifetime_limit_seconds']==30 and w['processing_roundtrip_ms']<2000
            assert len(full['tiles'])==4 and full['selection']['copied_samples']==262144 and full['classification']['link_family']=='unknown'
            for t in full['tiles']:
                clock=t['clock_refinement']
                assert clock['scope']=='selected_contiguous_burst' and clock['grids_tested']<=9
                assert clock['cp_trials']<=18 and clock['code_trials']<=18
                assert clock['interpolated_samples']<=11*clock['partition_samples']
                b=t['burst_analysis'];assert b['samples_examined']==16384 and b['max_regions_per_tile']==1
                assert b['cyclic_background']['fft_calls']<=16 and b['structure_discovery']['ofdm_fft_calls']<=6
                assert b['structure_discovery']['timing_hypotheses']<=32 and b['structure_discovery']['spread_hypotheses']<=32
                assert b['linear_sweep_discovery']['discovery_trials']<=510 and len(b['linear_sweep_discovery']['candidates'])<=3
                if kind=='eight_regions':assert b['eligible_retained_regions']==8 and b['budget_skipped_retained_regions']==7
            assert sha==structure.digest(source)
            records.append({'kind':kind,'sample_rate_hz':rate,'source_sha256':sha,'worker':w,
                'burst_samples_per_tile':[t['burst_analysis']['samples_examined'] for t in full['tiles']]})
    root=Path(__file__).resolve().parents[2]
    summary={'scope':'software_full_copy_budget_four_independent_bursts_not_hardware_acceptance','radio_opened':False,
        'model_trained':False,'hardware_acceptance':False,'records':records,'tool_sha256':structure.digest(tool),
        'worker_sha256':structure.digest(tool.with_name('wifi_cyclo_worker')),
        'source_sha256':{str(p.relative_to(root)):structure.digest(p) for p in (Path(__file__),root/'src/cyclostationary/burst_analysis.cpp',root/'src/cyclostationary/worker_protocol.cpp')}}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({'records':len(records),'min_ms':min(r['worker']['processing_roundtrip_ms'] for r in records),
        'max_ms':max(r['worker']['processing_roundtrip_ms'] for r in records)}))


if __name__=='__main__':main()
