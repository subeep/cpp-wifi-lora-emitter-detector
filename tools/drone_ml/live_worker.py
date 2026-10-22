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
from tools.drone_ml.presence import apply_unqualified_policy
from tools.drone_ml.live_audit import AuditRecorder, CONTEXTS

MAX_HEADER=8192
MAX_SAMPLES=4_000_000


def stft_preview(image, config):
    """Full grid, bounded uint8 display encoding; never changes model input."""
    if image.shape != (256, 625) or not np.isfinite(image).all():
        raise ValueError('invalid live STFT shape or values')
    low=float(image.min());high=max(float(image.max()),low+1.)
    pixels=np.rint(np.clip((image-low)/(high-low),0,1)*255).astype(np.uint8)
    return dict(schema='rfmon.ml.stft.v1',rows=256,cols=625,encoding='u8_frequency_time',
                bytes=pixels.size,db_min=low,db_max=high,rate_hz=config.rate_hz,
                nfft=config.nfft,hop=config.hop,duration_seconds=config.duration_seconds,
                scaling='source_unit_rms_log_power_before_checkpoint_normalization'),pixels.tobytes()


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


def serve(sock,model_path,audit_root=None,audit_context='unlabelled',audit_notes=''):
    engine=InferenceEngine(model_path)
    if (engine.config.duration_seconds!=.008 or engine.config.rate_hz!=20000000
            or engine.config.nfft!=256 or engine.config.hop!=256):
        raise ValueError('live profile requires 8 ms / 20 MSps / nfft 256 / hop 256')
    model_hash=sha256(model_path)
    recorder=None
    audit=dict(status='disabled',path='',saved=0,error='')
    if audit_root:
        try:
            recorder=AuditRecorder(audit_root,audit_context,audit_notes,model_hash,engine.config.metadata())
            audit=recorder.snapshot()
        except Exception as e:
            audit=dict(status='failed',path='',saved=0,error=str(e))
    send(sock,dict(schema='rfmon.ml.ready.v1',model_sha256=model_hash,
        classes=engine.checkpoint['classes'],architecture=engine.checkpoint.get('architecture','baseline'),
        preprocessing=engine.checkpoint['preprocessing'],policy_available=engine.checkpoint.get('policy') is not None,
        validated_for_live_use=False,audit=audit))
    while True:
        request,iq=receive(sock)
        started=time.perf_counter()
        want_preview=request.get('want_stft_preview') in (True,'true','1')
        result=engine.predict_iq(iq,float(request['sample_rate_hz']),int(request.get('offset_samples',0)),
                                 include_stft=want_preview)
        image=result.pop('_stft_image',None)
        pixels=None
        if image is not None:
            result['stft_preview'],pixels=stft_preview(image,engine.config)
        result=apply_unqualified_policy(result)
        # Echo exact provenance, including decimal-string 64-bit identifiers.
        result.update(schema='rfmon.ml.result.v1',request=request,
                      processing_ms=(time.perf_counter()-started)*1000)
        if recorder:
            audit=recorder.record(iq,request,result)
        result['audit']=audit
        send(sock,result)
        # Explicit request opt-in preserves compatibility with already-running
        # older supervisors. Metadata stays small; display pixels are separate.
        if pixels is not None:
            sock.sendall(struct.pack('<I',len(pixels))+pixels)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ipc-fd',type=int,required=True);p.add_argument('--model',required=True)
    p.add_argument('--audit-root');p.add_argument('--audit-context',choices=CONTEXTS,default='unlabelled')
    p.add_argument('--audit-notes',default='')
    args=p.parse_args()
    os.nice(10)
    resource.setrlimit(resource.RLIMIT_AS,(4*1024**3,4*1024**3))
    with socket.socket(fileno=args.ipc_fd) as sock:
        try:serve(sock,args.model,args.audit_root,args.audit_context,args.audit_notes)
        except EOFError:pass
        except Exception as e:
            try:send(sock,dict(schema='rfmon.ml.error.v1',error=str(e)))
            except OSError:pass
            raise


if __name__=='__main__':main()
