import json
from pathlib import Path
import socket
import struct
import threading
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'data/drone_ml/2026-11-14/universal_package/universal_v3.pt'
STORE = Path('/media/sudeep/Storage/drone_universal_classifier/2026-11-16/store-e4')
CACHE = Path('/media/sudeep/Storage/drone_universal_classifier/2026-11-16/cache-v3')


def frame(sock, request, iq):
    data = json.dumps(request).encode()
    sock.sendall(struct.pack('<I', len(data)) + data + np.asarray(iq, '<c8').tobytes())


def reply(sock):
    def exact(n):
        out = bytearray()
        while len(out) < n:
            chunk = sock.recv(n - len(out))
            if not chunk:
                raise EOFError
            out.extend(chunk)
        return bytes(out)
    return exact(struct.unpack('<I', exact(4))[0])


def request(i, n=3, rate=20e6, seq=7):
    return dict(schema='rfmon.ml.iq.v1', samples=int(rate * .008), sample_rate_hz=rate, interval_index=i,
                interval_count=n, capture_seq=seq, generation=1, run_id='r', radio_session=1, offset_samples=i)


@unittest.skipUnless(PACKAGE.exists(), 'universal package not built')
class WorkerProtocolTests(unittest.TestCase):
    def setUp(self):
        from tools.drone_ml.universal_worker import serve
        self.app, worker = socket.socketpair()
        self.thread = threading.Thread(target=lambda: self._serve(serve, worker), daemon=True)
        self.thread.start()
        self.hello = json.loads(reply(self.app))

    def _serve(self, serve, sock):
        try:
            serve(sock, str(PACKAGE))
        except (EOFError, OSError):
            pass

    def tearDown(self):
        self.app.close()

    def test_handshake_carries_types_models_and_accuracy(self):
        h = self.hello
        self.assertEqual(h['schema'], 'rfmon.ml.ready.v1')
        self.assertEqual(h['architecture'], 'universal_two_level_v1')
        self.assertIn('Air 3S', h['classes'])
        self.assertEqual(h['universal']['types'], ['dji_ofdm', 'lightbridge', 'hopping_controller', 'not_a_drone'])
        self.assertIn('same_unit', h['universal']['accuracy']['Air 3S'])

    def test_noise_capture_gives_no_type_and_one_capture_decision(self):
        rng = np.random.default_rng(1)
        for i in range(3):
            iq = ((rng.normal(size=160000) + 1j * rng.normal(size=160000)) * .01).astype(np.complex64)
            frame(self.app, dict(request(i), want_stft_preview=True), iq)
            r = json.loads(reply(self.app))
            self.assertEqual(r['schema'], 'rfmon.ml.result.v1')
            self.assertEqual(len(reply(self.app)), 160000)            # STFT preview pixels follow
            self.assertEqual(r['request']['interval_index'], i)
            self.assertEqual('capture' in r['universal'], i == 2)
        self.assertIsNone(r['universal']['capture']['type'])
        self.assertFalse(r['universal']['capture']['named'])

    def test_refused_last_interval_still_ends_the_capture(self):
        rng = np.random.default_rng(2)
        for i in range(3):
            iq = ((rng.normal(size=160000) + 1j * rng.normal(size=160000)) * .01).astype(np.complex64)
            if i == 2:
                iq = np.zeros(160000, np.complex64)                     # constant: refused
            frame(self.app, request(i, seq=8), iq)
            r = json.loads(reply(self.app))
        self.assertEqual(r['status'], 'insufficient_evidence')
        capture = r['universal']['capture']
        self.assertEqual(capture['schema'], 'rfmon.ml.universal.v1.capture')
        self.assertIsNone(capture['type'])
        self.assertFalse(capture['named'])
        self.assertIn('refused', capture['reason'])
        self.assertEqual(r['universal']['interval']['types'], {})

    def test_other_rates_are_refused(self):
        frame(self.app, request(0, rate=50e6), np.zeros(400000, np.complex64))
        r = json.loads(reply(self.app))
        self.assertEqual(r['status'], 'insufficient_evidence')
        self.assertNotIn('universal', r)


@unittest.skipUnless(PACKAGE.exists() and STORE.exists() and CACHE.exists(), 'package, view store or cache missing')
class WorkerParityTests(unittest.TestCase):
    def test_worker_inputs_and_decision_match_the_evaluation(self):
        from tools.drone_ml.preprocess import spectrogram
        from tools.drone_ml.universal_c4 import Views, explain
        from tools.drone_ml.universal_cache import CONFIG
        from tools.drone_ml.universal_c2_train import load
        from tools.drone_ml.universal_features import extract
        from tools.drone_ml.universal_worker import Captures, capture_decision, handle, load_package, pooled
        pkg, run, namer = load_package(PACKAGE)
        d = load(CACHE, 'unit_test')
        rows = [i for i, m in enumerate(d['meta']) if m['model'] == 'Air 3S']
        rec = d['meta'][rows[0]]['recording_id']
        rows = sorted((i for i in rows if d['meta'][i]['recording_id'] == rec), key=lambda i: d['meta'][i]['view_index'])[:3]
        views = np.load(STORE / (rec + '.npy'), mmap_mode='r')
        caps = Captures()
        for k, i in enumerate(rows):
            x = np.asarray(views[d['meta'][i]['view_index']])
            self.assertTrue(np.array_equal(pooled(spectrogram(x, 20_000_000, CONFIG)), d['spec'][i]))
            f = extract(x)
            # The cache may hold v1 facts (29); v2 facts append to them, so the cached ones are a prefix.
            n = d['facts'].shape[1]
            np.testing.assert_allclose(np.nan_to_num(np.asarray(f['values'][:n], np.float32)), np.nan_to_num(d['facts'][i]), rtol=1e-5)
            result, _ = handle(pkg, run, namer, caps, dict(request(k, n=len(rows))), x)
        sub = dict(spec=d['spec'][rows], facts=d['facts'][rows], available=d['available'][rows], meta=[d['meta'][i] for i in rows])
        expected = explain(Views(run, sub, pkg['model_type']).capture(range(len(rows))),
                           pkg['decision']['type_thresholds'], pkg['decision']['naming'], namer)
        got = result['universal']['capture']
        self.assertEqual((got['type'], got['named'], [m['name'] for m in got['models']]),
                         (expected['type'], expected['named'], [m['name'] for m in expected['models']]))
        self.assertEqual(got['type'], 'dji_ofdm')
        np.testing.assert_allclose([m['score'] for m in got['models']], [m['score'] for m in expected['models']], atol=1e-4)

    def test_network_runs_once_per_interval(self):
        from tools.drone_ml.universal_worker import Captures, handle, load_package
        pkg, run, namer = load_package(PACKAGE)
        calls = []
        predict = run.predict
        run.predict = lambda d, **k: (calls.append(len(d['spec'])), predict(d, **k))[1]
        rng = np.random.default_rng(3); caps = Captures()
        for i in range(3):
            iq = ((rng.normal(size=160000) + 1j * rng.normal(size=160000)) * .01).astype(np.complex64)
            result, _ = handle(pkg, run, namer, caps, request(i), iq)
        self.assertIn('capture', result['universal'])
        self.assertEqual(calls, [1, 1, 1])


if __name__ == '__main__':
    unittest.main()
