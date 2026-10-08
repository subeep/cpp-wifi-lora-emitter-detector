#!/usr/bin/env python3
"""Read-only four-tile helper checks; no radio/hardware performance acceptance."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool',type=Path,required=True)
    parser.add_argument('--input',type=Path,required=True,help='contiguous 262144-sample cf32 software noise control')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();tool=args.tool.resolve();source=args.input.resolve();output=args.output.resolve()
    if source.stat().st_size!=262144*8:parser.error('expected full-budget cf32 software noise control')
    output.mkdir(parents=True,exist_ok=False);before=digest(source);records=[]
    for rate in (20_000_000,60_000_000,200_000_000):
        for repeat in range(3):
            report=json.loads(subprocess.run([str(tool),'shadow-replay','--input',str(source),
                '--format','cf32_le','--sample-rate',str(rate),'--capture-samples','262144'],
                check=True,capture_output=True,text=True,timeout=15).stdout)
            w=report['worker']
            assert report['selection']['copied_samples']==262144 and len(report['tiles'])==4
            assert w['wire_version']==10 and w['process_isolated']
            assert w['address_space_limit_bytes']==134217728 and w['cpu_lifetime_limit_seconds']==30
            assert w['deadline_ms']==2000 and w['processing_roundtrip_ms']<2000
            assert report['classification']['link_family']=='unknown'
            for tile in report['tiles']:
                clock=tile['clock_refinement']
                assert clock['grids_tested']<=9 and clock['cp_trials']<=18 and clock['code_trials']<=18
                assert clock['interpolated_samples']<=11*clock['partition_samples']
                assert not any(c['pattern_consistent'] for c in clock['cp_candidates']+clock['code_candidates'])
                s=tile['structure_discovery']
                assert s['ofdm_fft_calls']<=6 and s['timing_hypotheses']<=32 and s['spread_hypotheses']<=32
                assert not any(p['pattern_consistent'] for p in s['ofdm_candidates']+s['short_code_candidates'])
                background=tile['cyclic_background']
                assert background['fft_calls']<=16 and len(background['peaks'])<=8
                sweep=tile['linear_sweep_discovery']
                assert sweep['discovery_trials']<=510 and len(sweep['candidates'])<=3
                assert not any(p['pattern_consistent'] for p in sweep['candidates'])
            records.append({'rate_hz':rate,'repeat':repeat,'worker':w,'tile_count':4,
                'copied_samples':262144,'classification':report['classification']})
    assert before==digest(source)
    root=Path(__file__).resolve().parents[2]
    summary={'scope':'software_full_copy_budget_noise_native_rates_not_hardware_acceptance','radio_opened':False,
        'source_sha256':before,'worker_sha256':digest(tool.with_name('wifi_cyclo_worker')),
        'tool_sha256':digest(tool),'script_sha256':digest(Path(__file__)),
        'background_source_sha256':digest(root/'src/cyclostationary/cyclic_background.cpp'),
        'sweep_source_sha256':digest(root/'src/cyclostationary/chirp_discovery.cpp'),
        'structure_source_sha256':digest(root/'src/cyclostationary/structure_discovery.cpp'),'timing_header_sha256':digest(root/'src/cyclostationary/clock_refinement.hpp'),'records':records}
    (output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({'records':len(records),'min_ms':min(r['worker']['processing_roundtrip_ms'] for r in records),
        'max_ms':max(r['worker']['processing_roundtrip_ms'] for r in records)}))


if __name__=='__main__':main()
