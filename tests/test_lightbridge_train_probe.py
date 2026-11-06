import unittest

import numpy as np
from tools.lightbridge_train_probe import BINS, PERIOD, compare, extract, pilot_scores


class TrainProbeTests(unittest.TestCase):
    @staticmethod
    def fixture(seed):
        rng=np.random.default_rng(seed)
        tones=rng.choice([1,1j,-1,-1j],size=(48,1024))
        pilots=BINS[::32] % 1024
        tones[:,pilots]=rng.choice([-1,1],size=(48,len(pilots)))
        tones[0]=np.random.default_rng(8).choice([1,-1],size=1024)
        useful=np.fft.ifft(tones,axis=1)
        symbols=np.column_stack((useful[:,-128:],useful))
        quiet=rng.normal(size=PERIOD*4)+1j*rng.normal(size=PERIOD*4)
        return np.r_[quiet,symbols.ravel(),quiet]

    def test_train_timing_and_independent_template(self):
        first,=extract(self.fixture(11))
        second,=extract(self.fixture(12))
        self.assertEqual(first[:2],(4,52))
        self.assertEqual(second[:2],(4,52))
        self.assertEqual(set(np.argsort(pilot_scores(first[3]))[-16:]),set(range(0,512,32)))
        result,=compare(first[3],[(1,second[3])])
        self.assertGreater(result['pilot_mean_percentile'],.95)
        self.assertEqual(result['onset_rank_vs_interior'],1.)

    def test_tone_rejected_by_off_phase_control(self):
        x=np.exp(.1j*np.arange(PERIOD*40))
        self.assertEqual(list(extract(x)),[])
