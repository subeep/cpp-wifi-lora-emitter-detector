from dataclasses import replace
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

from tools.drone_ml.common import sha256, stamp
from tools.drone_ml.localizer_audit import audit, score_regions
from tools.drone_ml.localizer_comparison_experiment import cases
from tools.drone_ml.localizer_synthetic import fixture, noise
from tools.drone_ml.rf_localizer_multiscale import MultiscaleConfig, chunk_regions, localize, _deduplicate, _boundary_match


class MultiscaleLocalizerTests(unittest.TestCase):
    def run_iq(self, x, rate=20_000_000, config=MultiscaleConfig()):
        return localize(lambda s,n:x[s:s+n],len(x),rate,len(x)/rate,config)

    def test_weak_simultaneous_signals_and_boundary_join(self):
        x,truth = fixture(7,0)
        r = self.run_iq(x)
        scores = [score_regions(r['regions'],g,20e6) for g in truth]
        self.assertTrue(all(s is not None and s['reference_area_coverage']>.8 for s in scores))
        region = r['regions'][scores[0]['proposal_id']]
        self.assertGreaterEqual(region['chunk_parts'],2)
        self.assertIn('transient_integrated',region['branches'])
        self.assertTrue(r['regions'][scores[2]['proposal_id']]['stable_background_ambiguity'])

    def test_continuous_wide_signal_kept_and_colored_noise_ambiguous(self):
        for kind,x,rate,truth in cases(7):
            if kind not in ('continuous_wide_10db','colored_noise'):
                continue
            r = self.run_iq(x,rate)
            self.assertTrue(any(p['stable_background_ambiguity'] for p in r['regions']))
            if truth:
                self.assertGreater(score_regions(r['regions'],truth[0],rate)['reference_area_coverage'],.8)
            self.assertIsNone(r['drone_presence']); self.assertIsNone(r['accepted_identity'])

    def test_zero_and_missing_observation_do_not_become_absence(self):
        r = self.run_iq(np.zeros(400000,dtype='<c8'))
        self.assertEqual(r['regions'],[]); self.assertIsNone(r['drone_presence'])
        self.assertTrue(r['fallback']['required']); self.assertFalse(r['training_eligible'])
        self.assertTrue(all(c['all_zero'] for c in r['chunks']))
        with self.assertRaisesRegex(ValueError,'insufficient'):
            localize(lambda s,n:np.zeros(n),1000,20e6,.02)
        with self.assertRaisesRegex(ValueError,'short'):
            localize(lambda s,n:np.zeros(n-1),400000,20e6,.02)

    def test_short_final_chunk_is_joined_and_no_padding_invented(self):
        n = 2000000; x = noise(np.random.default_rng(7),n).astype('<c8')
        reads=[]
        def get(s,k):
            self.assertLessEqual(s+k,n); self.assertLessEqual(k,4000000)
            reads.append((s,k)); return x[s:s+k]
        r = localize(get,n,20e6,.1)
        self.assertGreaterEqual(r['chunks'][-1]['pooled_frames'],16)
        self.assertEqual(len(reads),10)
        self.assertEqual(r['analysed_samples']+r['trailing_samples_not_analysed'],n)
        self.assertFalse(any(p['start_sample']>=1996800 for p in r['regions']))

    def test_gain_invariance_and_native_bounds(self):
        x,_ = fixture(7,0)
        first = self.run_iq(x); second = self.run_iq(x*32)
        fields=lambda r:[(p['start_sample'],p['stop_sample'],p['low_hz'],p['high_hz'],p['branches']) for p in r['regions']]
        self.assertEqual(fields(first),fields(second))
        for rate in (25_000_000,100_000_000,200_000_000):
            n=round(rate*.001)+17
            r=self.run_iq(np.zeros(n,dtype='<c8'),rate)
            self.assertEqual(r['analysed_samples']+r['trailing_samples_not_analysed'],n)
            self.assertLess(r['trailing_samples_not_analysed'],1024)

    def test_budgets_and_invalid_config(self):
        x,_ = fixture(7,10)
        r=self.run_iq(x,config=MultiscaleConfig(max_regions_per_record=1))
        self.assertLessEqual(len(r['regions']),1); self.assertGreater(r['record_budget_drops'],0)
        self.assertIsNone(r['drone_presence'])
        for bad in (replace(MultiscaleConfig(),max_regions_per_record=0),
                    replace(MultiscaleConfig(),fast_high_db=float('nan'))):
            with self.assertRaises(ValueError):bad.validate()
        with self.assertRaises(ValueError):chunk_regions(np.full(2048,np.nan),20e6)
        with self.assertRaises(ValueError):chunk_regions(np.zeros(2048),20e6,start_sample=-1)

    def test_stable_region_cannot_absorb_transient_or_bridge_frequencies(self):
        stable=dict(start_sample=0,stop_sample=1000,low_hz=-10.,high_hz=10.,
                    branches=['stable_envelope'],cells=30,high_cells=20,chunk_parts=1,
                    peak_relative_db=10.,stable_background_ambiguity=True)
        transient=dict(stable,start_sample=100,stop_sample=300,branches=['transient_fast'],
                       stable_background_ambiguity=False)
        self.assertEqual(len(_deduplicate([stable,transient])),2)
        self.assertFalse(_boundary_match(stable,transient))
        shifted=dict(transient,low_hz=5.,high_hz=25.)
        self.assertFalse(_boundary_match(transient,shifted))
        continuation=dict(transient,start_sample=250,stop_sample=500,low_hz=-9.,high_hz=11.)
        self.assertTrue(_boundary_match(transient,continuation))

    def test_audit_source_safety_and_algorithm_selection(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); src=root/'sources';src.mkdir();meta=root/'meta';meta.mkdir()
            path=src/'synthetic.cf32';np.zeros(400000,dtype='<c8').tofile(path)
            row=dict(path=str(path),source_sha256=sha256(path),sample_rate_hz=20000000,
                adapter='cf32_le',collection_group='synthetic',partition='development_only')
            manifest=meta/'sources.json'
            def write():manifest.write_text(json.dumps(dict(schema='rfmon.drone_ml.temporal_sources.v1',records=[row])))
            write();before=stamp(path)
            report=audit(manifest,root/'out',.02,algorithm='multiscale')
            self.assertEqual(report['algorithm'],'multiscale');self.assertEqual(stamp(path),before)
            with self.assertRaises(FileExistsError):audit(manifest,root/'out',.02,algorithm='multiscale')
            with self.assertRaisesRegex(ValueError,'unknown'):audit(manifest,root/'invalid',algorithm='invented')
            with self.assertRaisesRegex(ValueError,'baseline'):audit(manifest,root/'wrong',config=MultiscaleConfig())
            row['partition']='test_locked';row['path']='/missing/protected';write()
            with self.assertRaisesRegex(ValueError,'reserved'):audit(manifest,root/'protected',algorithm='multiscale')
            self.assertFalse((root/'protected').exists())


if __name__=='__main__':unittest.main()
