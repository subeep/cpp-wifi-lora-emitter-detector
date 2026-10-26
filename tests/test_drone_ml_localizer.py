import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

from tools.drone_ml.common import sha256, stamp
from tools.drone_ml.localizer_audit import audit, score_regions
from tools.drone_ml.localizer_synthetic import fixture, noise
from tools.drone_ml.rf_localizer import LocalizerConfig, chunk_regions, localize, proposal_view


class LocalizerTests(unittest.TestCase):
    def test_noise_and_zero_do_not_assert_absence(self):
        rng = np.random.default_rng(11); n = 400000
        for x in (np.zeros(n, dtype='<c8'), noise(rng, n).astype('<c8')):
            r = localize(lambda s, k:x[s:s+k], n, 20e6, .02)
            self.assertEqual(len(r['regions']), 0)
            self.assertIsNone(r['drone_presence']); self.assertIsNone(r['accepted_identity'])
            self.assertTrue(r['fallback']['required']); self.assertFalse(r['training_eligible'])

    def test_multisignal_boundary_and_continuous_region(self):
        x, truth = fixture(11, 15)
        r = localize(lambda s, k:x[s:s+k], len(x), 20e6, .02)
        scores = [score_regions(r['regions'], g, 20e6) for g in truth]
        self.assertTrue(all(s is not None and s['reference_area_coverage']>.8 for s in scores))
        wide = r['regions'][scores[0]['proposal_id']]
        self.assertGreaterEqual(wide['chunk_parts'], 2)
        self.assertGreater(wide['stop_sample'], .013*20e6-2048)
        self.assertTrue(r['regions'][scores[2]['proposal_id']]['touches_observation_edge'])
        self.assertFalse(r['reference_coordinates_used'])

    def test_native_rates_bounds_and_trailing_coverage(self):
        for rate in (20_000_000, 25_000_000, 100_000_000, 200_000_000):
            n = round(rate*.001)+17; x = np.zeros(n, dtype='<c8'); reads=[]
            def get(s, k):
                reads.append((s, k)); return x[s:s+k]
            r = localize(get, n, rate, n/rate)
            self.assertTrue(all(0<k<=4_000_000 and s+k<=n for s,k in reads))
            self.assertEqual(r['analysed_samples']+r['trailing_samples_not_analysed'], n)
            self.assertLess(r['trailing_samples_not_analysed'], 1024)

    def test_invalid_input_and_view_guards(self):
        with self.assertRaises(ValueError):LocalizerConfig(high_db=float('nan')).validate()
        with self.assertRaises(ValueError):LocalizerConfig(max_regions_per_chunk=10000).validate()
        with self.assertRaises(ValueError):chunk_regions(np.full(2048, np.nan), 20e6)
        with self.assertRaises(ValueError):chunk_regions(np.zeros(2048), 1e6)
        with self.assertRaises(ValueError):proposal_view(-30e6, 1e6, 20e6)
        v = proposal_view(-36e6, -17e6, 100e6)
        self.assertEqual(v['rate_hz'], 25000000); self.assertTrue(v['margin_satisfied'])
        edge = proposal_view(48e6, 50e6, 100e6)
        self.assertEqual(edge['rate_hz'], 100000000); self.assertFalse(edge['margin_satisfied'])

    def test_truncation_is_visible(self):
        x, _ = fixture(11, 15)
        regions, quality = chunk_regions(x[:200192], 20e6, config=LocalizerConfig(max_regions_per_chunk=1))
        self.assertEqual(len(regions), 1); self.assertGreater(quality['dropped_regions'], 0)

    def test_geometry_does_not_match_other_time(self):
        region = dict(id=0, start_sample=100, stop_sample=200, low_hz=-10, high_hz=10,
                      proposed_view=dict(offset_hz=0, rate_hz=20))
        g = dict(start=300, count=100, low_hz=-10, high_hz=10)
        self.assertIsNone(score_regions([region], g, 20e6))
        g['start'] = 100
        score = score_regions([region], g, 20e6)
        self.assertEqual(score['rectangle_iou'], 1)

    def test_partition_hash_output_and_source_preservation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); src = root/'sources'; src.mkdir(); meta = root/'meta'; meta.mkdir()
            path = src/'test.cf32'; np.zeros(400000, dtype='<c8').tofile(path)
            row = dict(path=str(path), source_sha256=sha256(path), sample_rate_hz=20000000,
                       adapter='cf32_le', collection_group='synthetic', partition='development_only')
            manifest = meta/'sources.json'
            def write():manifest.write_text(json.dumps(dict(schema='rfmon.drone_ml.temporal_sources.v1', records=[row])))
            write(); before = stamp(path)
            r = audit(manifest, root/'out', seconds=.02)
            self.assertTrue(r['source_stats_preserved']); self.assertEqual(stamp(path), before)
            with self.assertRaises(FileExistsError):audit(manifest, root/'out', seconds=.02)
            row['source_sha256'] = '0'*64; write()
            with self.assertRaisesRegex(ValueError, 'checksum'):audit(manifest, root/'bad', seconds=.02)
            row['partition'] = 'unknown_test_locked'; row['path'] = '/nonexistent/protected'; write()
            with self.assertRaisesRegex(ValueError, 'reserved'):audit(manifest, root/'protected', seconds=.02)
            self.assertFalse((root/'protected').exists())


if __name__ == '__main__':unittest.main()
