import json
import unittest
from unittest.mock import patch

import numpy as np

from tools.drone_ml.dsp_triage import assess, predict_with_triage


class Engine:
    def __init__(self):
        self.inputs = []

    def predict_iq(self, iq, rate, offset=0, *, include_stft=False):
        self.inputs.append((iq, rate, offset, include_stft))
        return dict(status='development_prediction_only', candidate='fixture',
                    accepted_class=None, softmax_score=.99)


class DspTriageTests(unittest.TestCase):
    def test_noise_tone_mixture_and_short_burst_all_reach_model_unchanged(self):
        rng = np.random.default_rng(418)
        noise = (rng.normal(size=160000) + 1j*rng.normal(size=160000)).astype(np.complex64)
        tone = np.exp(2j*np.pi*np.arange(noise.size)*.125).astype(np.complex64)
        burst = np.zeros_like(noise)
        burst[79000:79032] = noise[79000:79032]
        for name, iq in [('noise', noise), ('tone', tone), ('mixture', tone+.01*noise), ('burst', burst)]:
            with self.subTest(name=name):
                original = iq.copy()
                engine = Engine()
                result = predict_with_triage(engine, iq, 20e6, 123, include_stft=True)
                self.assertEqual(len(engine.inputs), 1)
                self.assertIs(engine.inputs[0][0], iq)
                self.assertEqual(engine.inputs[0][1:], (20e6, 123, True))
                np.testing.assert_array_equal(iq, original)
                self.assertEqual(result['softmax_score'], .99)
                self.assertIsNone(result['accepted_class'])
                self.assertEqual(result['dsp_triage']['route'], 'model')
                self.assertFalse(result['dsp_triage']['non_drone_rejection_enabled'])
                json.dumps(result, allow_nan=False)
        self.assertEqual(assess(noise, 20e6)['review_flag'], 'noise_like_review_only')
        self.assertEqual(assess(tone, 20e6)['review_flag'], 'tone_like_review_only')

    def test_full_interval_quality_check_and_no_negative_presence_claim(self):
        iq = np.ones(160000, dtype=np.complex64)
        broken = iq.copy();broken[45678] = complex(float('nan'), 1)
        for x in (iq, np.zeros_like(iq), broken):
            engine = Engine()
            result = predict_with_triage(engine, x, 20e6)
            self.assertEqual(engine.inputs, [])
            self.assertEqual(result['status'], 'insufficient_evidence')
            self.assertNotIn('candidate', result)
            self.assertNotIn('drone_present', result)
            json.dumps(result, allow_nan=False)

    def test_disable_and_diagnostic_failure_preserve_model_path(self):
        iq = np.ones(160000, dtype=np.complex64)
        engine = Engine()
        with patch('tools.drone_ml.dsp_triage.assess', side_effect=RuntimeError('fixture')) as measure:
            result = predict_with_triage(engine, iq, 20e6, enabled=False)
            measure.assert_not_called()
            self.assertNotIn('dsp_triage', result)
            result = predict_with_triage(engine, iq, 20e6)
            self.assertEqual(result['dsp_triage']['reason'], 'triage_failed_open')
        self.assertEqual(len(engine.inputs), 2)

    def test_bounded_features_preserve_absolute_sample_positions(self):
        iq = np.arange(4_000_000, dtype=np.float32).astype(np.complex64)
        report = assess(iq, 500e6)
        self.assertLessEqual(report['measured_samples'], 32768)
        self.assertEqual(report['frame_offsets_samples'][0], 0)
        self.assertEqual(report['frame_offsets_samples'][-1], len(iq)-1024)
        self.assertEqual(report['frequency_bin_hz'], 500e6/1024)
        self.assertEqual(report['route'], 'model')


if __name__ == '__main__':
    unittest.main()
