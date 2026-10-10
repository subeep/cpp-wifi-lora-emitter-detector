#!/usr/bin/env python3
"""Compare native diagnostics with Python on retained development candidates.

Reads only workspace derivative IQ. This is port regression, not identity validation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import numpy as np
from marker_prefix import assess_marker_structure
from marker_symbol import prepare_filtered_window


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool',type=Path,required=True)
    p.add_argument('--replay',type=Path,action='append',required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args(); rows=[]; checked=set()
    for folder in a.replay:
        for path in sorted(folder.glob('*.json')):
            replay=json.loads(path.read_text())
            if 'windows' not in replay:continue
            record=replay['record']; src=Path(record['path'])
            if not src.resolve().is_relative_to(Path('data/wifi_drone_analysis').resolve()):
                raise ValueError('only workspace derivatives allowed')
            if str(src) not in checked:
                assert hashlib.sha256(src.read_bytes()).hexdigest()==record['sha256']
                checked.add(str(src))
            for w in replay['windows']:
                for trial in [w]+w['refinements']:
                    eligible=[c for c in trial['named'] if c['status']=='unvalidated_candidate']
                    if not eligible:continue
                    filtered=trial is not w; rate=record['sample_rate_hz']
                    args=[str(a.tool.resolve()),'analyze','--input',str(src),'--format',record['format'],
                          '--sample-rate',str(rate),'--center',str(record['capture_center_hz']),
                          '--offset',str(w['offset']),'--samples','65536','--frames','128','--alpha-bins','1',
                          '--assess-evidence','--check-marker-structure']
                    if filtered:
                        for opt,key in [('roi-offset','center_offset_hz'),('roi-bandwidth','width_hz'),
                                        ('source-bandwidth','digital_span_hz'),('decimate','decimation')]:
                            args += ['--'+opt,str(trial[key])]
                    native=json.loads(subprocess.check_output(args,timeout=20))
                    with src.open('rb') as f:
                        f.seek(w['offset']*(8 if record['format']=='cf32_le' else 4))
                        data=f.read(65536*(8 if record['format']=='cf32_le' else 4))
                    if record['format']=='cf32_le': x=np.frombuffer(data,dtype='<c8')
                    else:
                        v=np.frombuffer(data,dtype='<i2').reshape(-1,2)
                        x=(v[:,0].astype(np.float32)+1j*v[:,1].astype(np.float32))/32768
                    if filtered:x,rate=prepare_filtered_window(x,rate,trial)
                    for c in eligible:
                        i=c['measurement_index'];m=native['chirp_structure']['candidates'][i]
                        py=assess_marker_structure(x,rate,m['peak_window_offset_samples'],m['slope_hz_per_second'],m['frequency_mean_offset_hz'])
                        cpp=native['marker_structure']['candidates'][i]
                        agrees=(cpp['evaluated'] and cpp['prefix_compatible']==py['prefix_compatible'] and
                                cpp['joint_compatible']==py.get('joint_span_and_prefix',False))
                        rows.append(dict(record=src.name,offset=w['offset'],filtered=filtered,profile=c['profile'],
                                         agrees=bool(agrees),native=cpp,python=py))
    report=dict(schema='native_marker_port_regression.v1',tool_sha256=hashlib.sha256(a.tool.read_bytes()).hexdigest(),
                evaluations=len(rows),agreements=sum(r['agrees'] for r in rows),
                joint_compatible=sum(r['native']['joint_compatible'] for r in rows),
                independent_identity_validation=False,accepted_family=None,rows=rows)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(report,indent=2)+'\n')
    print({k:v for k,v in report.items() if k!='rows'})
    if not rows or not all(r['agrees'] for r in rows):raise SystemExit(1)

if __name__=='__main__':main()
