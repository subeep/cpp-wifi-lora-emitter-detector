"""Read-only comparison of recorded live security decisions with offline replay."""
import hashlib,json,pathlib,subprocess,sys
root=pathlib.Path(__file__).resolve().parent
repo=root.parent.parent
tool=repo/'build-wifi-enrolment/wifi_security_replay'
inputs=[repo/'docs/wifi_enrolment_validation/live-2026-10-16'/n for n in ('ch1','ch6','sweep24','sweep5','soak1')]
# Fresh high-traffic run is assessed separately: bounded recording rotates away earlier input.
results=[]
for folder in inputs:
 recording=folder/'events.ndjson';saved=folder/'snapshot.json'
 before=hashlib.sha256(recording.read_bytes()).hexdigest()
 proc=subprocess.run([str(tool),str(recording)],capture_output=True,text=True,timeout=60,check=True)
 replay=json.loads(proc.stdout);live=json.loads(saved.read_text())
 excluded={'recent','recorded_bytes'}
 differences=[k for k in sorted(live.keys()|replay.keys()) if k not in excluded and live.get(k)!=replay.get(k)]
 r=dict(run=folder.name,semantic_difference_keys=differences,input_unchanged=before==hashlib.sha256(recording.read_bytes()).hexdigest(),sha256=before,frames=live['frames_accepted'],incidents=live['incidents_total'],coverage=live['coverage'],flood=live['flood'],beacon_replay=live['beacon_replay'],transactions=live['transactions'],baselines=[{k:b[k] for k in ('channel','learned_analysed_s','windows','captures_excluded','holders') if k in b} for b in live['baselines']],input_lines_rejected=replay['input_lines_rejected'])
 r['passed']=not differences and r['input_unchanged'] and not r['input_lines_rejected'];results.append(r)
 print(folder.name,'PASS' if r['passed'] else 'FAIL','frames',r['frames'],'differences',differences)
(root/'ambient-audit.json').write_text(json.dumps({'executable_sha256':hashlib.sha256(tool.read_bytes()).hexdigest(),'cases':results,'passed':all(r['passed'] for r in results)},indent=2)+'\n')
if not all(r['passed'] for r in results):sys.exit(1)
