"""Publisher metadata checks and unit curation; synthetic files, no drone accuracy."""
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import h5py
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.drone_ml.drff import KNOWN, curate, interval, metadata
from tools.drone_ml.common import stamp


def fixture(path, target, condition=1):
    path.parent.mkdir(parents=True, exist_ok=True)
    with h5py.File(path, 'w') as h:
        for key, value in dict(TD=target, State='hover', U='u2', D='d1').items():
            h[key] = np.array([ord(c) for c in value], dtype=np.uint16)[:, None]
        h['Fs'] = [[100_000_000.]]
        h['CenterFrequence'] = [[5_745_000_000.]]
        h['C'] = [[condition]]
        for key in ('RF0_I', 'RF0_Q'):
            h.create_dataset(key, shape=(200_000, 1), dtype='float32', fillvalue=0)


class DRFFTests(unittest.TestCase):
    def test_read_metadata_and_bounded_intervals(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'a.mat'
            fixture(path, 'mini4PRO_1')
            before = stamp(path)
            self.assertEqual(metadata(path)['target'], 'mini4PRO_1')
            with h5py.File(path, 'r') as h:
                self.assertEqual(interval(h, 100, 10).size, 10)
                with self.assertRaises(ValueError):
                    interval(h, 199_999, 10)
                for start, count in ((-1, 10), (0, 4_000_001), (0, 0)):
                    with self.assertRaises(ValueError):
                        interval(h, start, count)
            # Plain h5py open above can touch atime on writable fixtures. Source reader cannot.
            self.assertEqual(stamp(path)[2:3], before[2:3])

    def test_conflicting_frequency_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'a.mat'
            fixture(path, 'mini4PRO_1')
            with h5py.File(path, 'r+') as h:
                h['CenterFreq'] = [[2_437_000_000.]]
            with self.assertRaises(ValueError):
                metadata(path)

    def test_noninteger_condition_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'a.mat'
            fixture(path, 'mini4PRO_1')
            with h5py.File(path, 'r+') as h:
                del h['C']
                h['C'] = [[1.5]]
            with self.assertRaises(ValueError):
                metadata(path)

    def test_indirect_iq_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'a.mat'
            fixture(path, 'mini4PRO_1')
            with h5py.File(path, 'r+') as h:
                del h['RF0_I']
                h['RF0_I'] = h5py.ExternalLink('elsewhere.mat', 'IQ')
            with self.assertRaises(ValueError):
                metadata(path)

    def test_frozen_cross_unit_plan_and_disagreement(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'sources'
            folder = root / 'DRFF-R2/DRFF-R2/dataset3-single_drone_hover'
            for model, units in [(m, range(1, 6)) for m in KNOWN] + [('mini3pro', range(1, 3)), ('mavic3S', range(1, 2))]:
                for unit in units:
                    for c in range(1, 4):
                        fixture(folder / f'{model}_{unit}_hover_c{c}_u2_d1.mat', f'{model}_{unit}', c)
            scripts = root / 'DRFF-R2/code/signal_collection'
            scripts.mkdir(parents=True)
            for name in ('signal_collection1.py', 'signal_collection2.py'):
                (scripts / name).write_text('# synthetic fixture, never executed\n')
            with patch('tools.drone_ml.drff.os.statvfs', return_value=SimpleNamespace(f_flag=os.ST_RDONLY)):
                report = curate(root, Path(tmp) / 'review')
                self.assertEqual(report['groups'], 18)
                self.assertEqual(report['sources'], 54)
                held = [r for r in report['records'] if r['partition'] == 'test']
                self.assertTrue(all(r['split_group'].endswith('_5') for r in held))
                path = folder / 'mini4PRO_5_hover_c1_u2_d1.mat'
                with h5py.File(path, 'r+') as h:
                    del h['TD']
                    h['TD'] = np.array([ord(c) for c in 'mini4PRO_1'], dtype=np.uint16)[:, None]
                with self.assertRaises(ValueError):
                    curate(root, Path(tmp) / 'bad-review')


if __name__ == '__main__':
    unittest.main()
