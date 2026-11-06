import tempfile
import unittest
from pathlib import Path

import numpy as np

from tools.lightbridge_full_scan import CELL, map_file
from tools.lightbridge_prefix_check import contrast


class FullScanTests(unittest.TestCase):
    def test_activity_localization_and_tail(self):
        n = CELL * 258 + 39
        x = np.zeros(n, dtype='<c8')
        x[CELL*256:CELL*257] = np.exp(2j*np.pi*np.arange(CELL)*80/512)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'capture.dat'
            x.tofile(path)
            power, total = map_file(path)
        self.assertEqual(total, n)
        self.assertEqual(power.shape, (258,4))
        self.assertEqual(np.unravel_index(power.argmax(),power.shape),(256,3))

    def test_prefix_recurs_at_discovered_phase(self):
        rng = np.random.default_rng(4)
        data = rng.normal(size=(183,1024))+1j*rng.normal(size=(183,1024))
        x = np.column_stack((data[:,-128:],data)).ravel()
        _,first,second = contrast(x,1024,128,1152)
        self.assertGreater(min(first,second),.7)
        self.assertLess(abs(contrast(x,1024,128,1159)[2]),.15)
