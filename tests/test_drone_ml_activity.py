"""Activity proxies and development split isolation; no real RF accuracy claims."""
from pathlib import Path
import sys
import unittest
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.drone_ml.activity import assess, rank
from tools.drone_ml.calibration import balanced_outcomes, choose
from tools.drone_ml.development import permitted
from tools.drone_ml.preprocess import STFTConfig, spectrogram
from tools.drone_ml.splits import assign
from tools.drone_ml.train import accept


class ActivityTests(unittest.TestCase):
    def test_noise_is_not_structured(self):
        rng = np.random.default_rng(1)
        iq = rng.normal(size=160000) + 1j * rng.normal(size=160000)
        cfg = STFTConfig(duration_seconds=.008, hop=256)
        q = assess(spectrogram(iq, 20000000, cfg))
        self.assertFalse(q['eligible'])

    def test_broadband_burst_and_tone_are_structured_not_drone_truth(self):
        rng = np.random.default_rng(2)
        iq = rng.normal(size=160000) + 1j * rng.normal(size=160000)
        iq[80000:] *= .01
        cfg = STFTConfig(duration_seconds=.008, hop=256)
        self.assertTrue(assess(spectrogram(iq, 20000000, cfg))['eligible'])
        tone = np.exp(2j * np.pi * 2e6 * np.arange(160000) / 20e6)
        q = assess(spectrogram(tone, 20000000, cfg))
        self.assertTrue(q['eligible'])
        self.assertIn('not_drone_truth', q['interpretation'])

    def test_bad_activity_input(self):
        for image in (np.zeros((256, 2)), np.full((256, 16), np.nan)):
            with self.assertRaises(ValueError):
                assess(image)

    def test_more_activity_ranked_higher(self):
        self.assertGreater(rank(dict(structured_fraction=.5, median_contrast_db=7)),
                           rank(dict(structured_fraction=.1, median_contrast_db=20)))

    def test_test_sources_excluded_before_io(self):
        review = dict(schema='rfmon.drone_ml.drff_review.v1', records=[dict(partition='train') for _ in range(42)] +
                      [dict(partition='test', source='/must/not/read')] + [dict(partition='unknown_test', source='/must/not/read')])
        self.assertEqual(len(permitted(review)), 42)
        self.assertFalse(any(r.get('source') == '/must/not/read' for r in permitted(review)))

    def test_margin_changes_acceptance(self):
        p = np.array([[.6, .4], [.9, .1]])
        d = np.zeros_like(p)
        policy = [dict(min_probability=.5, max_distance=1, min_margin=.3)] * 2
        self.assertEqual(accept(p, d, policy).tolist(), [-1, 0])

    def test_group_balance_is_not_window_balance(self):
        labels = np.array([0] * 101 + [-1])
        decisions = np.array([0] * 100 + [-1, -1])
        groups = ['large'] * 100 + ['small', 'unknown']
        r = balanced_outcomes(decisions, labels, groups)
        self.assertEqual(r['correct_accepted'], .5)

    def test_calibration_search_no_final_partitions(self):
        p = np.array([[.9, .1]] * 6 + [[.1, .9]] * 6 + [[.55, .45]] * 6)
        d = np.array([[.1, .9]] * 6 + [[.9, .1]] * 6 + [[.8, .8]] * 6)
        labels = np.array([0] * 6 + [1] * 6 + [-1] * 6)
        groups = ['a'] * 3 + ['b'] * 3 + ['c'] * 3 + ['d'] * 3 + ['unknown'] * 6
        policy, report = choose(p, d, labels, groups, np.ones(18, dtype=bool), 2, accept)
        self.assertEqual(len(report['trials']), 46)
        self.assertEqual(report['selected']['unknown_accepted'], 0)
        self.assertEqual(report['selected']['correct_accepted'], 1)

    def test_harmful_policy_is_disabled(self):
        p = np.array([[.9, .1]] * 6 + [[.1, .9]] * 6 + [[.999, .001]] * 6)
        d = np.array([[.1, .9]] * 6 + [[.9, .1]] * 6 + [[.01, .9]] * 6)
        labels = np.array([0] * 6 + [1] * 6 + [-1] * 6)
        groups = ['a'] * 3 + ['b'] * 3 + ['c'] * 3 + ['d'] * 3 + ['unknown'] * 6
        policy, report = choose(p, d, labels, groups, np.ones(18, dtype=bool), 2, accept)
        self.assertIsNone(policy)
        self.assertFalse(report['enabled'])
        self.assertEqual(report['selected']['objective'], 0)

    def test_development_split_cannot_include_test(self):
        rows = [dict(split_group='a', label='a', recording_id='a', source_sha256='a',
                     original_source_group='a', offset_samples=0, samples=1)]
        plan = dict(schema='rfmon.drone_ml.split.v1', groups={'a':dict(partition='test',
                    label_evidence='synthetic', independence_evidence='synthetic')})
        with self.assertRaisesRegex(ValueError, 'must not contain test'):
            assign(rows, plan, development_only=True)


if __name__ == '__main__':
    unittest.main()
