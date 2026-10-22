import json
from pathlib import Path
import socket
import struct
import tempfile
import threading
import unittest
import numpy as np
import torch
from tools.drone_ml.live_worker import receive,receive_exact,serve
from tools.drone_ml.predict import InferenceEngine
from tools.drone_ml.model import SpectrogramCNN
from tools.drone_ml.preprocess import STFTConfig


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
                try:serve(b,model)
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

if __name__=='__main__':unittest.main()
