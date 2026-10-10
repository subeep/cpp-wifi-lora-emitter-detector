import sys,tempfile
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/cyclostationary'))
from prepare_extended_link_references import scalar,matlab_text,label_conflicts,complex_prefix,command_bytes,xml_metadata,checked_source,prepare

assert scalar([[100e6]])==100e6
assert matlab_text(np.array([[65],[66]],dtype='u2'))=='AB'
assert label_conflicts('DRFF-R2','dataset7-environment/indoor_environment.mat','mini5PRO_1')==['environment_folder_has_drone_target_metadata']
assert label_conflicts('DRFF-R2','dataset7-environment/outdoor_environment.mat','')==['environment_absence_unverified']
assert label_conflicts('DRFF-R2','dataset6-wifi_mixed/mini4PRO_1_c1.mat','mini4PRO_1')==[]
assert label_conflicts('RFUAV','FRSKY X14','FLYSKY X14')==['filename_and_target_metadata_disagree']
assert label_conflicts('RFUAV','DJI MINI4 PRO','DJI MINI4 PRO')==[]
i=np.arange(8,dtype=np.float32).reshape(-1,1);q=-i
for a,b in [(i,q),(i.T,q.T)]:
 assert np.array_equal(np.frombuffer(complex_prefix(a,b,5),dtype='<c8'),(i[:5,0]+1j*q[:5,0]))
for call in [lambda:scalar([1,2]),lambda:scalar(float('nan')),lambda:scalar(0),
 lambda:complex_prefix(i,q,9),lambda:complex_prefix(i,q.T,5),
 lambda:complex_prefix(i,np.full_like(q,np.nan),5),lambda:xml_metadata(b'<!DOCTYPE x><x/>')]:
 try:call()
 except ValueError:pass
 else:raise AssertionError('invalid metadata/IQ accepted')
xml=b'<SignalHoundIQFile><Drone>test</Drone><DataType>Complex Float</DataType><SampleRate>100000000</SampleRate><CenterFrequency>2440000000</CenterFrequency><SampleCount>100000000</SampleCount><IFBandwidth>100000000</IFBandwidth></SignalHoundIQFile>'
assert xml_metadata(xml)['SampleRate']==100e6
assert command_bytes([sys.executable,'-c','print("abc",end="")'],16)==b'abc'
assert command_bytes([sys.executable,'-c','import os; os.write(1,b"abcdef")'],3,True)==b'abc'
for cmd,limit,prefix,timeout in [([sys.executable,'-c','print("x")'],8,True,1),
 ([sys.executable,'-c','print("abcdef")'],3,False,1),
 ([sys.executable,'-c','import time; time.sleep(2)'],8,False,.05)]:
 try:command_bytes(cmd,limit,prefix,timeout)
 except (ValueError,TimeoutError):pass
 else:raise AssertionError('short/oversized/stalled stream accepted')
with tempfile.TemporaryDirectory() as d:
 root=Path(d);(root/'source').write_bytes(b'test')
 for call in [lambda:checked_source(root,'source'),lambda:checked_source(root,'../escape'),lambda:prepare(root,root/'out')]:
  try:call()
  except ValueError:pass
  else:raise AssertionError('writable source/escaping path/in-place output accepted')
 assert not (root/'out').exists()
print('extended reader: metadata conflicts, IQ bounds, subprocess limits and source isolation passed')
