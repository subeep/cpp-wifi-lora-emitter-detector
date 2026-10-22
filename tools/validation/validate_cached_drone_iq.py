#!/usr/bin/env python3
"""Replay hash-verified cached dataset excerpts with the production DSP worker.

Accepts existing project reference manifests. Model names stay context only;
never enables named-family acceptance or creates an independent test split.
"""
import argparse,hashlib,json,pathlib,subprocess

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--manifest',type=pathlib.Path,action='append',required=True);ap.add_argument('--tool',type=pathlib.Path,required=True);ap.add_argument('--output',type=pathlib.Path,required=True);a=ap.parse_args()
    if a.output.exists():raise ValueError('Output directory must be new')
    a.output.mkdir(parents=True);tool=a.tool.resolve();results=[];seen=set()
    for manifest in a.manifest:
        for r in json.loads(manifest.read_text())['records']:
            src=pathlib.Path(r['path']).resolve()
            if src in seen:raise ValueError('Repeated recording path')
            seen.add(src);before=digest(src)
            if before!=r['sha256']:raise ValueError('Working copy differs from recorded checksum')
            cmd=[str(tool),'shadow-replay','--input',str(src),'--format',r['format'],'--sample-rate',str(r['sample_rate_hz']),'--center',str(r['capture_center_hz']),'--capture-samples',str(r['samples']),'--continuous-samples',str(r['samples'])]
            p=subprocess.run(cmd,capture_output=True,text=True,timeout=30,check=True);d=json.loads(p.stdout)
            unchanged=digest(src)==before;identity_unknown=d['classification']['link_family']=='unknown' and d['classification']['model_status']=='not_trained'
            tiles=d['tiles'];accepted=any(t['dsp_evidence']['named_family_acceptance_enabled'] for t in tiles)
            name=f'{len(results):03d}-{src.stem}.json';(a.output/name).write_text(json.dumps(d,indent=2)+'\n')
            results.append({'recording':src.name,'source_manifest':str(manifest),'sha256':before,'partition':'development_only','link_family_truth':r.get('link_family_truth'),'original_unchanged':unchanged,'windows':len(tiles),'identity_stays_unknown':identity_unknown,'named_acceptance_enabled':accepted,'result':name,'passed':unchanged and identity_unknown and not accepted and bool(tiles)})
            print(src.name,len(tiles),'windows',flush=True)
    passed=bool(results) and all(r['passed'] for r in results)
    (a.output/'summary.json').write_text(json.dumps({'schema':'rfmon.cached_dataset_replay.v1','passed':passed,'scope':'bounded sampled-window regression; not whole-recording recall or independently validated family accuracy','tool_sha256':digest(tool),'radio_opened':False,'model_trained':False,'records':results},indent=2)+'\n')
    if not passed:raise SystemExit(1)
if __name__=='__main__':main()
