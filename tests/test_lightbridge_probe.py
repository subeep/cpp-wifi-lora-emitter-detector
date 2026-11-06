import unittest
import numpy as np
from tools.lightbridge_probe import features

class ProbeTests(unittest.TestCase):
    def test_invalid_and_constant(self):
        self.assertTrue(features(np.zeros(160000,dtype=np.complex64),60e6)['constant'])
        x=np.ones(160000,dtype=np.complex64);x[-1]=np.nan
        self.assertFalse(features(x,60e6)['finite'])

    def test_tone_and_immutability(self):
        for dtype in (np.complex64,np.complex128):
            x=np.exp(2j*np.pi*np.arange(262144)/64).astype(dtype)
            before=x.copy();f=features(x,60e6)
            np.testing.assert_array_equal(x,before)
            self.assertAlmostEqual(f['peak_offset_hz'],60e6/64,delta=60e6/4096)
            # Absolute real correlation peaks include negative half-periods;
            # equal harmonics need not rank the fundamental first.
            self.assertTrue(all(p['lag_samples']%32==0 for p in f['autocorrelation_peaks']))

if __name__=='__main__':unittest.main()
