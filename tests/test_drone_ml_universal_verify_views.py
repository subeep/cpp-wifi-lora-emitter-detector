import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
from tools.drone_ml.universal_verify_views import verify
from tools.drone_ml.universal_views import LENGTH, RATE, VERSION


class VerifyViewsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.inputs, self.store = root / 'inputs', root / 'store'
        self.inputs.mkdir(); self.store.mkdir()
        source = root / 'source.bin'
        source.write_bytes(b'fixture')
        self.ident = 'a' * 24
        row = dict(recording_id=self.ident, source=str(source), model='fixture',
                   dataset='fixture', readable=True, flags=[], rate_hz=RATE, center_hz=2400000000)
        manifests = {'inventory.json': dict(records=[row]),
                     'classes.json': dict(status='reviewed', classes=[dict(model='fixture')]),
                     'background_split.json': dict(partitions={})}
        for name, value in manifests.items():
            self.write(self.inputs / name, value)
        self.write(self.store / 'config.json', dict(smoke=False, inputs={
            str(self.inputs / name): hashlib.sha256((self.inputs / name).read_bytes()).hexdigest()
            for name in manifests}))
        array = np.ones((3, LENGTH), dtype='<c8')
        self.iq = self.store / (self.ident + '.npy')
        np.save(self.iq, array)
        self.meta = self.store / (self.ident + '.json')
        self.write(self.meta, dict(source_record=row,
            source_signature=[source.stat().st_size, source.stat().st_mtime_ns],
            rate_hz=RATE, samples_per_window=LENGTH, view_version=VERSION,
            output_bytes=self.iq.stat().st_size, iq_sha256=hashlib.sha256(array.tobytes()).hexdigest(),
            windows=[dict(offset_hz=0, center_hz=row['center_hz']) for _ in range(3)]))
        self.write(self.store / 'report.json', dict(complete=True, errors=[],
            completed_recordings=1, total_windows=3, windows_per_class={'fixture': 3}))

    @staticmethod
    def write(path, value):
        path.write_text(json.dumps(value))

    def test_complete_store_passes(self):
        result = verify(self.inputs, self.store)
        self.assertTrue(result['passed'])
        self.assertEqual(result['windows'], 3)
        self.assertFalse(result['original_iq_opened'])

    def test_same_size_iq_corruption_fails(self):
        data = np.load(self.iq, mmap_mode='r+')
        data[0, 0] = 9
        data.flush(); del data
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            verify(self.inputs, self.store)

    def test_changed_input_fails(self):
        path = self.inputs / 'classes.json'
        path.write_text(path.read_text() + '\n')
        with self.assertRaisesRegex(ValueError, 'input manifest changed'):
            verify(self.inputs, self.store)

    def test_extra_iq_output_fails(self):
        np.save(self.store / 'orphan.npy', np.zeros(1))
        with self.assertRaisesRegex(ValueError, 'unexpected or missing IQ'):
            verify(self.inputs, self.store)

    def test_incomplete_build_fails(self):
        path = self.store / 'report.json'
        report = json.loads(path.read_text()); report['complete'] = False
        self.write(path, report)
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            verify(self.inputs, self.store)


if __name__ == '__main__':
    unittest.main()
