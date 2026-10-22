"""Corrupt copies of live MPDUs offline; never transmit or modify source data."""
import copy,json,pathlib,subprocess,tempfile,zlib
root=pathlib.Path(__file__).resolve().parent;repo=root.parent.parent
source=repo/'docs/wifi_enrolment_validation/live-2026-10-16/soak1/events.ndjson'
frame=next(json.loads(l) for l in source.open() if json.loads(l).get('kind')=='frame')
original=bytes.fromhex(frame['mpdu']);cases={}
with tempfile.TemporaryDirectory(prefix='wifi-security-corruption-') as tmp:
 for name in ('bit_errors','short_headers'):
  rows=[]
  for n in range(256):
   f=copy.deepcopy(frame);f['sample_start']=n*1000;f['capture_seq']=n+1
   if name=='bit_errors':
    b=bytearray(original);b[(n//8)%len(b)]^=1<<(n%8)
   else:
    b=bytearray(original[:2+n%8]);b+=zlib.crc32(b).to_bytes(4,'little')
   f['mpdu']=b.hex();f['fcs_valid']=True;rows.append(f)
  p=pathlib.Path(tmp)/(name+'.ndjson');p.write_text(''.join(json.dumps(f)+'\n' for f in rows))
  result=subprocess.run([str(repo/'build-wifi-enrolment/wifi_security_replay'),str(p)],text=True,capture_output=True,timeout=20,check=True)
  d=json.loads(result.stdout);cases[name]={k:d[k] for k in ('frames_ingested','frames_accepted','frames_rejected_fcs','frames_rejected_malformed','incidents_total')}
  assert d['frames_ingested']==256 and d['frames_accepted']==0 and d['incidents_total']==0,cases[name]
(root/'corrupt-frames.json').write_text(json.dumps(cases,indent=2)+'\n');print(json.dumps(cases,indent=2))
