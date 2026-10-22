"""Deterministic IPC fixture; never reads a model or connects to hardware."""
import argparse,json,socket,struct,time
p=argparse.ArgumentParser();p.add_argument('--ipc-fd',type=int);p.add_argument('--model')
a=p.parse_args()
s=socket.socket(fileno=a.ipc_fd)
def read(n):
    b=bytearray()
    while len(b)<n:
        part=s.recv(n-len(b))
        if not part:raise EOFError
        b.extend(part)
    return b
def send(x):
    b=json.dumps(x).encode();s.sendall(struct.pack('<I',len(b))+b)
send(dict(schema='rfmon.ml.ready.v1',model_sha256='fixture',architecture='fixture',classes=['synthetic']))
try:
    while True:
        n=struct.unpack('<I',read(4))[0];r=json.loads(read(n));iq=read(int(r['samples'])*8)
        if a.model.endswith('checkspread'):
            first=struct.unpack('<ff',iq[:8]);offset=int(r['offset_samples'])
            if first!=(float(offset),float(offset)+.5):raise ValueError('wrong input interval')
        if a.model.endswith('crash'):break
        if a.model.endswith('slow'):time.sleep(5)
        result=dict(schema='rfmon.ml.result.v1',request=r,status='identification_unavailable',
                  accepted_class='unqualified_identity',candidate='synthetic',top_candidates=[dict(label='synthetic',score=.99999)],
                  processing_ms=1,selected_offset_hz=0,reason=['synthetic_fixture'])
        if r.get('want_stft_preview') in (True,'true','1'):
            result['stft_preview']=dict(schema='rfmon.ml.stft.v1',rows=256,cols=625,bytes=160000,
                encoding='u8_frequency_time',db_min=-100,db_max=20,rate_hz=20000000,nfft=256,hop=256,
                duration_seconds=.008,scaling='source_unit_rms_log_power_before_checkpoint_normalization')
            if a.model.endswith('badpreview'):result['stft_preview']['rows']=1024
        send(result)
        if 'stft_preview' in result:s.sendall(struct.pack('<I',160000)+bytes(range(256))*625)
        if a.model.endswith('delayedspread') and int(r.get('interval_index',0))==0:time.sleep(.2)
except (EOFError,BrokenPipeError):pass
