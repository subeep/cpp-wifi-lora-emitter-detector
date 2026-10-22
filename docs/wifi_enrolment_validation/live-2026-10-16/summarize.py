import collections,json,pathlib
root=pathlib.Path(__file__).resolve().parent
result={}
for name in ('ch1','ch6','sweep24','sweep5','soak1'):
 p=root/name/'events.ndjson'
 if not p.exists():continue
 events=[json.loads(l) for l in p.open()]
 c=[x for x in events if x['kind']=='capture'];f=[x for x in events if x['kind']=='frame']
 d={'captures':len(c),'channels':sorted(set(x['channel'] for x in c)), 'received_samples':sum(x['samples_received'] for x in c),'frames':len(f),'phy_frames':dict(collections.Counter(x['phy'] for x in f)), 'overflows':sum(bool(x['overflows']) for x in c),'timeouts':sum(x['timed_out'] for x in c),'exceptions':sum(x['exception'] for x in c),'short_captures':sum(x['samples_received']<x['samples_requested'] for x in c),'burst_cap_reached':sum(x['burst_cap_reached'] for x in c),'security_queue_rejections':sum(x['events_rejected_by_queue'] for x in c),'max_processing_s':max((x['processing_s'] for x in c),default=0)}
 q=root/name/'enrolment-validation.json'
 if q.exists():
  e=json.loads(q.read_text());devices=e.pop('devices');d.update(e);d.update(interfaces=len(devices),observations=sum(x['observations'] for x in devices.values()),rf_accepted=sum(x['accepted'] for x in devices.values()),rf_rejected=sum(x['rejected'] for x in devices.values()),profiles=sum(len(x['profiles']) for x in devices.values()),all_new=all(x['label']=='NEW DEVICE' for x in devices.values()))
 q=root/name/'snapshot.json'
 if q.exists():
  s=json.loads(q.read_text());d['incidents_total']=s.get('incidents_total')
 result[name]=d
(root/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
