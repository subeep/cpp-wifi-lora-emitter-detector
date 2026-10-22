"""Local framed socket worker. No radio ownership and no network listeners."""
import argparse
import json
import math
import os
from pathlib import Path
import resource
import socket
import struct
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
os.environ['OMP_NUM_THREADS']='2'
os.environ['OPENBLAS_NUM_THREADS']='2'
os.environ['MKL_NUM_THREADS']='2'
import numpy as np
from tools.drone_ml.predict import InferenceEngine
from tools.drone_ml.common import sha256

MAX_HEADER=8192
MAX_SAMPLES=4_000_000


def receive_exact(sock,n):
    parts=bytearray()
    while len(parts)<n:
        chunk=sock.recv(min(n-len(parts),131072))
        if not chunk:raise EOFError('worker channel closed')
        parts.extend(chunk)
    return parts


def send(sock,value):
    data=json.dumps(value,allow_nan=False).encode()
    if len(data)>262144:raise ValueError('oversized response')
    sock.sendall(struct.pack('<I',len(data))+data)


def receive(sock):
    n=struct.unpack('<I',receive_exact(sock,4))[0]
    if not 1<=n<=MAX_HEADER:raise ValueError('invalid request header size')
    request=json.loads(receive_exact(sock,n))
    count=int(request['samples']);rate=float(request['sample_rate_hz'])
    if not math.isfinite(rate) or rate!=int(rate) or not 20e6<=rate<=500e6:
        raise ValueError('unsupported sample rate')
    if not 1<=count<=MAX_SAMPLES or count!=round(rate*.008):
        raise ValueError('expected one complete 8 ms interval')
    if request.get('schema')!='rfmon.ml.iq.v1':raise ValueError('unsupported protocol')
    raw=receive_exact(sock,count*8)
    return request,np.frombuffer(raw,dtype='<c8')


def serve(sock,model_path):
    engine=InferenceEngine(model_path)
    if (engine.config.duration_seconds!=.008 or engine.config.rate_hz!=20000000
            or engine.config.nfft!=256 or engine.config.hop!=256):
        raise ValueError('live profile requires 8 ms / 20 MSps / nfft 256 / hop 256')
    send(sock,dict(schema='rfmon.ml.ready.v1',model_sha256=sha256(model_path),
        classes=engine.checkpoint['classes'],architecture=engine.checkpoint.get('architecture','baseline'),
        preprocessing=engine.checkpoint['preprocessing'],policy_available=engine.checkpoint.get('policy') is not None,
        validated_for_live_use=False))
    while True:
        request,iq=receive(sock)
        started=time.perf_counter()
        result=engine.predict_iq(iq,float(request['sample_rate_hz']),int(request.get('offset_samples',0)))
        if result.get('accepted_class') is not None:
            # This first live integration has no qualified release-policy bundle.
            result.update(accepted_class=None,status='identification_unavailable',
                          reason='live_acceptance_not_qualified')
        # Echo exact provenance, including decimal-string 64-bit identifiers.
        result.update(schema='rfmon.ml.result.v1',request=request,
                      processing_ms=(time.perf_counter()-started)*1000)
        send(sock,result)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ipc-fd',type=int,required=True);p.add_argument('--model',required=True)
    args=p.parse_args()
    os.nice(10)
    resource.setrlimit(resource.RLIMIT_AS,(4*1024**3,4*1024**3))
    with socket.socket(fileno=args.ipc_fd) as sock:
        try:serve(sock,args.model)
        except EOFError:pass
        except Exception as e:
            try:send(sock,dict(schema='rfmon.ml.error.v1',error=str(e)))
            except OSError:pass
            raise


if __name__=='__main__':main()
