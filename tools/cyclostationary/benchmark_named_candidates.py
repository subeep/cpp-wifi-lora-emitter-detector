#!/usr/bin/env python3
"""Named marker development/replay checks; no link accuracy or receiver qualification."""
import argparse
import hashlib
import json
from pathlib import Path
from benchmark_candidate_qualification import run

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--tool',type=Path,required=True)
p.add_argument('--baseline-tool',type=Path,required=True)
p.add_argument('--cases',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=False)
records=[]
for old in json.loads(a.cases.read_text())['records']:
    c=old['case'];path=Path(c['path']);before=path.stat()
    size=8 if c['format']=='cf32_le' else 4
    def digest():
        with path.open('rb') as f:
            f.seek(c['offset']*size);raw=f.read(c['samples']*size)
        assert len(raw)==c['samples']*size
        return hashlib.sha256(raw).hexdigest()
    sha=digest()
    opts=['--input',path,'--format',c['format'],'--sample-rate',c['sample_rate_hz'],
          '--center',c['capture_center_hz'],'--offset',c['offset'],'--samples',c['samples'],
          '--frames',256,'--alpha-bins',1,'--assess-evidence']
    if c['width']:opts+=['--usable-bandwidth',c['width']]
    baseline,_=run(a.baseline_tool.resolve(),opts)
    current,_=run(a.tool.resolve(),opts)
    assert current==baseline
    added,ms=run(a.tool.resolve(),opts+['--named-link-candidates'])
    d=added.pop('named_link_candidates');assert added==current
    assert d['accepted_family'] is None and not d['identity_used']
    assert len(d['candidates'])<=3 and all(not x['accepted'] for x in d['candidates'])
    after=path.stat()
    assert (before.st_ino,before.st_size,before.st_mtime_ns,before.st_ctime_ns)==(after.st_ino,after.st_size,after.st_mtime_ns,after.st_ctime_ns)
    assert sha==digest()
    records.append({'case':c,'window_sha256':sha,'named_candidates':d,'wall_ms':ms})
report={'scope':'development_named_marker_not_identity_validation','cases':len(records),
        'default_fields_exact':True,'additive_fields_exact':True,'sources_unchanged':True,
        'hashes':{k:hashlib.sha256(v.read_bytes()).hexdigest() for k,v in
                  [('tool',a.tool),('baseline',a.baseline_tool),('script',Path(__file__)),('cases',a.cases)]},
        'records':records}
(a.output/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='records'},indent=2))
