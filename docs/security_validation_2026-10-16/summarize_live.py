import hashlib,json,pathlib
root=pathlib.Path(__file__).resolve().parent;p=root/'live36'
s=json.loads((p/'snapshot.json').read_text());e=json.loads((p/'enrolment-validation.json').read_text())
keys=['frames_accepted','captures_ingested','incidents_total','incidents_open','queue_events_dropped','queue_captures_dropped','input_lines_rejected','storage_error','persist_error','recorded_bytes','coverage','flood','beacon_replay','transactions','frames_by_type','frames_by_phy']
r={k:s[k] for k in keys};r['baselines']=[{k:b[k] for k in ['channel','learned_analysed_s','windows','captures_excluded','holders'] if k in b} for b in s['baselines']]
r['enrolment']={k:e[k] for k in ['dropped_batches','queued_after_stop','restart_equal','storage_error','reload_error']}
r['retained_files']={};count=0
for f in [p/'events.ndjson.1',p/'events.ndjson']:
 if not f.exists():continue
 r['retained_files'][f.name]={'bytes':f.stat().st_size,'sha256':hashlib.sha256(f.read_bytes()).hexdigest()}
 for line in f.open():
  if json.loads(line).get('kind')=='capture':count+=1
r['retained_capture_records']=count;r['full_session_replay_possible']=count==s['captures_ingested']
(root/'live-summary.json').write_text(json.dumps(r,indent=2)+'\n')
print(json.dumps(r,indent=2))
