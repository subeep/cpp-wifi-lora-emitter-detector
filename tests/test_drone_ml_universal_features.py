import unittest
import numpy as np
from tools.drone_ml.universal_features import extract,NAMES
from tools.drone_ml.cp_lag_probe import block_ratios,BLOCK

class FeatureTests(unittest.TestCase):
    def test_zero_has_missing_evidence(self):
        r=extract(np.zeros(160000,dtype=np.complex64))
        self.assertFalse(r['available'].any())
        self.assertTrue(np.isfinite(r['values']).all())
    def test_existing_timing_statistic_is_preserved(self):
        rng=np.random.default_rng(19)
        x=(rng.normal(size=160000)+1j*rng.normal(size=160000)).astype(np.complex64)
        r=extract(x);expected=block_ratios(x[:BLOCK],20e6)
        for name,value in expected.items():
            self.assertAlmostEqual(r['lag_ratios'][name],value,delta=1e-6*max(1,abs(value)))
        self.assertEqual(len(r['values']),len(NAMES))
    def test_burst_duration_and_period(self):
        rng=np.random.default_rng(20)
        x=(rng.normal(size=160000)+1j*rng.normal(size=160000)).astype(np.complex64)*.001
        for start in (20000,60000,100000):x[start:start+10000]*=100
        r=extract(x);d=dict(zip(NAMES,r['values']))
        self.assertEqual(r['burst_complete_count'],3)
        self.assertAlmostEqual(d['burst_length_s'],.0005,delta=2*256/20e6)
        self.assertAlmostEqual(d['burst_period_s'],.002,delta=2*256/20e6)
    def test_nonfinite_rejected(self):
        with self.assertRaises(ValueError):extract(np.full(160000,np.inf))
if __name__=='__main__':unittest.main()
