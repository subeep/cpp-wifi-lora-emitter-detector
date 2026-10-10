import sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/cyclostationary'))
from prepare_local_link_references import open_source,stamp,prepare
with tempfile.TemporaryDirectory() as d:
    root=Path(d);src=root/'source.bin';src.write_bytes(b'unchanged IQ source')
    before=stamp(src)
    with open_source(src) as f:assert f.read()==b'unchanged IQ source'
    assert stamp(src)==before
    for dest in [root,root/'derived']:
        try:prepare(root,dest)
        except ValueError:pass
        else:raise AssertionError('in-place dataset output accepted')
    assert not (root/'derived').exists()
print('no-atime read and rejection of outputs within dataset root passed')

import io
import numpy as np
from prepare_local_link_references import read_ci16_slice,read_complex_slice
v=np.zeros((1,12),dtype=[('real','<f8'),('imag','<f8')])
v['real'][0]=np.arange(12);v['imag'][0]=-np.arange(12)
expected=(np.arange(4,9)-1j*np.arange(4,9)).astype('<c8')
assert read_complex_slice(v,4,5)==expected.tobytes()
raw=np.arange(24,dtype='<i2').tobytes()
assert read_ci16_slice(io.BytesIO(raw),4,5)==raw[16:36]
for read in [lambda:read_complex_slice(v,10,5),lambda:read_ci16_slice(io.BytesIO(raw),10,5)]:
    try:read()
    except ValueError:pass
    else:raise AssertionError('short interval accepted')
for value in [-1,float('nan'),float('inf'),.1001]:
    with tempfile.TemporaryDirectory() as d:
        dest=Path(d)/'not-created'
        try:prepare(Path(d)/'sources',dest,value)
        except ValueError:pass
        else:raise AssertionError('invalid time accepted')
        assert not dest.exists()
print('nonzero source interval, bounds and short-read checks passed')

# A shorter recording must become an explicit skip, never an invented interval.
import contextlib,types,zipfile,json
saved_h5py=sys.modules.get('h5py')
try:
    sys.modules['h5py']=types.SimpleNamespace(File=lambda *_:contextlib.nullcontext({'uhd_samps':v}))
    with tempfile.TemporaryDirectory() as d:
        base=Path(d);root=base/'datasets';folder=root/'KULeuven_DroneRF';folder.mkdir(parents=True)
        src=folder/'mini2vid.zip'
        with zipfile.ZipFile(src,'w') as z:z.writestr('first.mat',b'test HDF reader fixture')
        before=stamp(src)
        assert prepare(root,base/'derived',.1,['mini2vid.zip'])==[]
        m=json.loads((base/'derived'/'manifest.json').read_text())
        assert m['skipped'][0]['available_samples']==12
        assert m['skipped'][0]['requested_sample_offset']==10000000
        assert stamp(src)==before
finally:
    if saved_h5py is None:sys.modules.pop('h5py',None)
    else:sys.modules['h5py']=saved_h5py
print('short selected member is skipped with source preservation and provenance')
