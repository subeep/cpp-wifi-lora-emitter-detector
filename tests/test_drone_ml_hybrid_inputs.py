import unittest
from pathlib import Path
import tempfile
import numpy as np
import torch
from tools.drone_ml.hybrid_inputs import causal_view, frequency_views, align_features, prepare_case, TILE, FEATURES
from tools.drone_ml.hybrid_model import FEATURES as MODEL_FEATURES
from tools.drone_ml.integration_replay import recording, select_records
from tools.drone_ml.common import sha256
from tools.drone_ml.hybrid_feature_policy import common_dsp_inputs


class HybridInputTests(unittest.TestCase):
    def test_source_specific_quality_masks_cannot_leak_through_baseline_policy(self):
        x = torch.zeros(1, 4, 9); a = torch.zeros_like(x, dtype=torch.bool)
        a[:, :, :5] = True
        expected = common_dsp_inputs(x, a)
        # Two sources have the same measured common features, but only one has
        # trustworthy original ADC scale. It must not reveal that dataset bit.
        x[:, :, 5] = .3; a[:, :, 5] = True
        actual = common_dsp_inputs(x, a)
        torch.testing.assert_close(expected[0], actual[0])
        torch.testing.assert_close(expected[1], actual[1])

    def test_reserved_source_fails_before_any_path_access(self):
        with self.assertRaisesRegex(ValueError, 'reserved'):
            with recording(dict(partition='test_locked', source='/never/open')):
                self.fail('reserved reader entered')
        with self.assertRaisesRegex(ValueError, 'reserved'):
            with recording(dict(partition='unknown_test_locked', source='/never/open'), development_replay=True):
                self.fail('development mode admitted final data')

    def test_selection_never_promotes_calibration_or_final_test(self):
        records = [dict(label='Mini', dataset='fixture', center_hz=2440e6, source=f'/{p}',
                        split_group=p, partition=p, recording_id=p)
                   for p in ('train', 'calibration', 'test_locked')]
        review = dict(schema='rfmon.drone_ml.expanded_review.v1', records=records)
        plan = dict(schema='rfmon.drone_ml.expanded_split.v1',
                    groups={r['split_group']:dict(partition=r['partition'],label=r['label']) for r in records})
        self.assertEqual([r['partition'] for r in select_records(review, plan)], ['train'])

    def test_view_grid_and_feature_schema(self):
        self.assertEqual(FEATURES, MODEL_FEATURES)
        for rate in (20000000, 100000000, 120000000, 200000000):
            offsets = frequency_views(rate)
            self.assertIn(0, offsets)
            self.assertTrue(np.all(np.diff(offsets) <= 16000000))
            if rate > 20000000:
                self.assertLessEqual(max(np.abs(offsets))+10000000, .45*rate+.01)

    def test_causal_downconversion_has_no_future_tile_leakage(self):
        rate = 40000000; factor = 2; n = 17*TILE*factor
        t = np.arange(n)/rate
        x = (.1*np.exp(2j*np.pi*6000000*t)).astype(np.complex64)
        y, meta = causal_view(x, rate, 5000000., 16)
        # A source tone 1 MHz above the selected center remains 1 MHz.
        measured = np.angle(np.sum(np.conj(y[:-1])*y[1:], dtype=np.complex128))*20000000/(2*np.pi)
        self.assertAlmostEqual(measured, 1000000., delta=2.)
        changed = x.copy(); changed[9*TILE*factor:] = 0
        other, _ = causal_view(changed, rate, 5000000., 16)
        np.testing.assert_array_equal(y[:8*TILE], other[:8*TILE])
        self.assertEqual(meta['future_samples_required'], 0)
        self.assertEqual(meta['fir_group_delay_native_samples'], 20)

    def test_dsp_windows_only_join_at_their_end_and_reject_other_epochs(self):
        e = dict(profile='o4_structure_v2', continuity_epoch=1, start_sample=0, sample_count=2*TILE,
                 cp_correlation=.2, lag_prominence=4., cyclic_correlation=.1,
                 cyclic_prominence=2., harmonic_correlation=.1)
        values, masks = align_features([e], 16)
        self.assertFalse(masks[0].any()); self.assertTrue(masks[1,:5].all())
        self.assertFalse(masks[2:].any()); self.assertFalse(masks[:,5:].any())
        self.assertAlmostEqual(float(values[1,0]), .2)
        with self.assertRaisesRegex(ValueError, 'duplicate'): align_features([e, e], 16)
        with self.assertRaisesRegex(ValueError, 'foreign-epoch'): align_features([{**e,'continuity_epoch':2}], 16)
        with self.assertRaisesRegex(ValueError, 'misaligned'): align_features([{**e,'start_sample':TILE}], 16)

    def test_silent_input_stays_invalid_without_creating_absence_labels(self):
        worker = Path(__file__).resolve().parents[1]/'build/drone_native_worker'
        if not worker.is_file(): self.skipTest('build native worker for offline transport integration')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); prefix = root/'synthetic.cf32'
            np.zeros(3*TILE, dtype='<c8').tofile(prefix)
            case = dict(partition='train', collection_group='synthetic_test_not_a_corpus_label',
                        sample_rate_hz=20000000, center_hz=2440e6,
                        model_prefix=dict(path=str(prefix),sha256=sha256(prefix),samples=3*TILE),
                        source_provenance=dict(scaling='publisher_float_units'))
            manifest = prepare_case(case, root/'prepared', worker)
            row = manifest['views'][0]
            with np.load(row['path'], allow_pickle=False) as tensors:
                self.assertFalse(tensors['valid'].any())
                self.assertFalse(tensors['available'].any())
            self.assertEqual(row['valid_tiles'], 0)
            self.assertIsNone(row['presence_label'])
            self.assertFalse(row['training_eligible'])


if __name__ == '__main__':
    unittest.main()
