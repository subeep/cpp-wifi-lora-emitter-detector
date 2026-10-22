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
        n=struct.unpack('<I',read(4))[0];r=json.loads(read(n));read(int(r['samples'])*8)
        if a.model.endswith('crash'):break
        if a.model.endswith('slow'):time.sleep(5)
        send(dict(schema='rfmon.ml.result.v1',request=r,status='identification_unavailable',
                  accepted_class=None,candidate='synthetic',top_candidates=[dict(label='synthetic',score=.8)],
                  processing_ms=1,reason=['synthetic_fixture']))
except (EOFError,BrokenPipeError):pass
