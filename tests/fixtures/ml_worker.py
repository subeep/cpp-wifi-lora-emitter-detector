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
hello=dict(schema='rfmon.ml.ready.v1',model_sha256='fixture',architecture='fixture',classes=['synthetic'])
if 'universal' in a.model:
    hello['universal']=dict(schema='rfmon.ml.universal.v1',types=['dji_ofdm','lightbridge','hopping_controller','not_a_drone'],
        decision='universal_c4_decision_v3',model_types={'Mini 4 Pro':'dji_ofdm','Air 3S':'dji_ofdm'},
        accuracy={'Mini 4 Pro':dict(same_unit=dict(captures=40,named=30,top1_right=29),unseen_aircraft=dict(captures=20,named=1,top1_right=1))})
send(hello)
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
        if 'universal' in a.model:
            result['universal']=dict(schema='rfmon.ml.universal.v1',interval=dict(types=dict(dji_ofdm=.9,lightbridge=0,hopping_controller=0,not_a_drone=.1),allowed=['dji_ofdm','not_a_drone']))
            if int(r.get('interval_index',0))==int(r.get('interval_count',1))-1:
                result['universal']['capture']=dict(schema='rfmon.ml.universal.v1.capture',intervals=int(r.get('interval_count',1)),
                    type='dji_ofdm',best_type='dji_ofdm',type_score=.95,named=True,reason='named',distance=4.5,distance_limit=20.,score_limit=.6,
                    models=[dict(name='Mini 4 Pro',score=.8),dict(name='Air 3S',score=.15),dict(name='Avata 2',score=.05)])
            if a.model.endswith('baduniversal'):result['universal']['interval']['types']['dji_ofdm']=1.5
            if a.model.endswith('refuseduniversal'):     # the worker's form for a refused interval
                result.update(status='insufficient_evidence',candidate='',top_candidates=[],reason=['nonfinite or constant IQ'])
                result['universal']=dict(schema='rfmon.ml.universal.v1',interval=dict(types={},allowed=[]))
                if int(r.get('interval_index',0))==int(r.get('interval_count',1))-1:
                    result['universal']['capture']=dict(schema='rfmon.ml.universal.v1.capture',intervals=int(r.get('interval_count',1)),
                        type=None,models=[],named=False,reason='incomplete capture (an interval was refused: nonfinite or constant IQ)')
        if r.get('dsp_triage') in (True,'true','1'):
            result['dsp_triage']=dict(schema='rfmon.ml.dsp_triage.v1',mode='observe',route='model',
                assessment='unresolved_activity',review_flag='noise_like_review_only',reason='fixture',
                model_invoked=True,processing_ms=.1,non_drone_rejection_enabled=False,thresholds_validated=False)
            if a.model.endswith('badtriage'):result['dsp_triage']['route']='non_drone'
        if r.get('want_stft_preview') in (True,'true','1'):
            result['stft_preview']=dict(schema='rfmon.ml.stft.v1',rows=256,cols=625,bytes=160000,
                encoding='u8_frequency_time',db_min=-100,db_max=20,rate_hz=20000000,nfft=256,hop=256,
                duration_seconds=.008,scaling='source_unit_rms_log_power_before_checkpoint_normalization')
            if a.model.endswith('badpreview'):result['stft_preview']['rows']=1024
        send(result)
        if 'stft_preview' in result:s.sendall(struct.pack('<I',160000)+bytes(range(256))*625)
        if a.model.endswith('delayedspread') and int(r.get('interval_index',0))==0:time.sleep(.2)
except (EOFError,BrokenPipeError):pass
