import unittest
import numpy as np
from tools.drone_ml.universal_views import deployment_view, offsets, LENGTH

class ViewTests(unittest.TestCase):
    def test_tone_is_translated_and_amplitude_preserved(self):
        rate=80_000_000; carrier=13_000_000; residual=1_000_000
        n=round(rate*.0082);t=np.arange(n)/rate
        x=.25*np.exp(2j*np.pi*(carrier+residual)*t)
        y=deployment_view(x,rate,carrier)
        self.assertEqual(y.shape,(LENGTH,))
        self.assertAlmostEqual(float(np.mean(np.abs(y))),.25,places=3)
        precise=y.astype(np.complex128)
        frequency=np.angle(np.vdot(precise[:-1],precise[1:]))*20e6/(2*np.pi)
        self.assertAlmostEqual(frequency,residual,delta=2)
    def test_native_samples_are_preserved(self):
        rng=np.random.default_rng(4)
        x=(rng.normal(size=164000)+1j*rng.normal(size=164000)).astype(np.complex64)
        np.testing.assert_array_equal(deployment_view(x,20_000_000,0),x[2000:162000])
    def test_offsets_of_one_window_do_not_accumulate(self):
        # All offsets of one complex128 window, in order (the B1 loop): the window must be left
        # unchanged, and a tone 1 MHz above one offset must appear in that offset's view only.
        rate=100_000_000;n=round(rate*.0082);t=np.arange(n)/rate;grid=offsets(rate)
        for k in (0,3,6):
            x=np.exp(2j*np.pi*(grid[k]+1e6)*t);before=x.copy()
            power=[float(np.mean(np.abs(deployment_view(x,rate,f))**2)) for f in grid]
            np.testing.assert_array_equal(x,before)
            self.assertEqual(int(np.argmax(power)),k)
            self.assertAlmostEqual(power[k],1.,places=2)
            self.assertLess(sorted(power)[-2],.1)          # neighbouring views overlap at their filter edge
    def test_bad_geometry_and_nonfinite_rejected(self):
        with self.assertRaises(ValueError):offsets(10_000_000)
        with self.assertRaises(ValueError):deployment_view(np.ones(164000),20_000_000,1)
        with self.assertRaises(ValueError):deployment_view(np.ones(100),20_000_000,0)
        with self.assertRaises(ValueError):deployment_view(np.full(164000,np.nan),20_000_000,0)
if __name__=='__main__':unittest.main()
