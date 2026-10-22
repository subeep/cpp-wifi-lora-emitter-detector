import json
from pathlib import Path
import socket
import struct
import tempfile
import threading
import unittest
import numpy as np
import torch
from tools.drone_ml.live_worker import receive,receive_exact,serve,stft_preview
from tools.drone_ml.predict import InferenceEngine
from tools.drone_ml.model import SpectrogramCNN
from tools.drone_ml.preprocess import STFTConfig,spectrogram
from tools.drone_ml.input_audit import audit_session
from tools.drone_ml.common import sha256


class LiveTests(unittest.TestCase):
    def test_framed_inference_matches_direct_and_preserves_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            model=Path(tmp)/'model.pt'
            torch.manual_seed(17)
            torch.save(dict(schema='rfmon.drone_ml.checkpoint.v1',
                preprocessing=STFTConfig(duration_seconds=.008,hop=256).metadata(),
                normalization=dict(mean=-10.,scale=10.),classes=['a','b'],
                state_dict=SpectrogramCNN(2).state_dict(),policy=None,
                policy_blockers=['unvalidated']),model)
            iq=(np.random.default_rng(1).normal(size=160000)+1j*np.random.default_rng(2).normal(size=160000)).astype('<c8')
            expected=InferenceEngine(model).predict_iq(iq,20e6)
            a,b=socket.socketpair();a.settimeout(10)
            errors=[]
            def run():
                try:serve(b,model,str(Path(tmp)/'audit'),'background_observation','synthetic test only')
                except EOFError:pass
                except Exception as e:errors.append(e)
                finally:b.close()
            t=threading.Thread(target=run);t.start()
            def response():
                size=struct.unpack('<I',receive_exact(a,4))[0]
                return json.loads(receive_exact(a,size))
            try:
                hello=response();self.assertEqual(hello['schema'],'rfmon.ml.ready.v1')
                header=dict(schema='rfmon.ml.iq.v1',samples='160000',sample_rate_hz='20000000',capture_seq='9007199254740993')
                raw=json.dumps(header).encode()
                a.sendall(struct.pack('<I',len(raw))+raw+iq.tobytes())
                got=response()
                self.assertEqual(got['candidate'],expected['candidate'])
                self.assertEqual(got['softmax_score'],expected['softmax_score'])
                self.assertEqual(got['request']['capture_seq'],header['capture_seq'])
                self.assertIsNone(got['accepted_class'])
                self.assertFalse(got['score_is_calibrated_probability'])
                self.assertEqual(got['presence_status'],'unclassified_rf')
                self.assertIsNone(got['drone_present'])
                self.assertEqual(got['audit']['saved'],1)
                audit_dir=Path(got['audit']['path'])
                np.testing.assert_array_equal(np.fromfile(audit_dir/'0000.cf32',dtype='<c8'),iq)
                self.assertFalse(json.loads((audit_dir/'session.json').read_text())['training_eligible'])
                replay=audit_session(InferenceEngine(model),audit_dir,sha256(model))
                self.assertTrue(replay['rows'][0]['replay_matches_saved'])
                self.assertTrue(replay['session_replay_matches_saved'])
                # New supervisors opt in; legacy requests above received only JSON.
                header['want_stft_preview']='true'
                raw=json.dumps(header).encode()
                a.sendall(struct.pack('<I',len(raw))+raw+iq.tobytes())
                visible=response()
                count=struct.unpack('<I',receive_exact(a,4))[0]
                self.assertEqual(count,160000)
                pixels=np.frombuffer(receive_exact(a,count),dtype=np.uint8).reshape(256,625)
                info=visible['stft_preview']
                image=spectrogram(iq,20e6,STFTConfig(duration_seconds=.008,hop=256))
                restored=info['db_min']+pixels.astype(float)*(info['db_max']-info['db_min'])/255
                np.testing.assert_allclose(restored,image,atol=(info['db_max']-info['db_min'])/510+1e-5)
                self.assertEqual(visible['class_scores'],got['class_scores'])
                # Invalid IQ clears the preview; no obsolete pixels or extra frame.
                a.sendall(struct.pack('<I',len(raw))+raw+np.zeros_like(iq).tobytes())
                invalid=response()
                self.assertEqual(invalid['presence_status'],'insufficient_evidence')
                self.assertNotIn('stft_preview',invalid)
                # Same-size corruption must not silently replay as valid evidence.
                with (audit_dir/'0000.cf32').open('r+b') as f:
                    f.write(b'\x00\x00\x00\x00')
                with self.assertRaisesRegex(ValueError,'integrity mismatch'):
                    audit_session(InferenceEngine(model),audit_dir,sha256(model))
            finally:a.close();t.join(10)
            self.assertFalse(t.is_alive());self.assertEqual(errors,[])

    def test_invalid_request_rejected_before_iq_allocation(self):
        for header in (None,dict(schema='rfmon.ml.iq.v1',samples=4000001,sample_rate_hz=500000000),
                       dict(schema='rfmon.ml.iq.v1',samples=160000,sample_rate_hz=float('nan')),
                       dict(schema='bad',samples=160000,sample_rate_hz=20000000)):
            with self.subTest(header=header):
                a,b=socket.socketpair()
                try:
                    raw=json.dumps(header).encode() if header else b''
                    a.sendall(struct.pack('<I',len(raw))+raw)
                    with self.assertRaises(ValueError):receive(b)
                finally:a.close();b.close()

    def test_stft_display_encoding_rejects_invalid_data(self):
        cfg=STFTConfig(duration_seconds=.008,hop=256)
        for data in (np.zeros((256,10)),np.full((256,625),np.nan)):
            with self.assertRaises(ValueError):stft_preview(data,cfg)
        info,pixels=stft_preview(np.full((256,625),-30.),cfg)
        self.assertGreater(info['db_max'],info['db_min'])
        self.assertEqual(len(pixels),160000)

if __name__=='__main__':unittest.main()
