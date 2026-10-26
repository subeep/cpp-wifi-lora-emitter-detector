from dataclasses import replace
import unittest

import numpy as np

from tools.drone_ml.localizer_comparison_experiment import cases
from tools.drone_ml.localizer_reference_study import duty_fixture,one_to_one_matches
from tools.drone_ml.rf_context_reference import ContextConfig,chunk,localize


class ContextReferenceTests(unittest.TestCase):
    def run_iq(self,x,rate=20_000_000,**kw):
        return localize(lambda s,n:x[s:s+n],len(x),rate,len(x)/rate,
            segments=[(0,len(x))],continuity='synthetic_contiguous',**kw)

    def test_high_duty_bursts_have_distinct_geometry(self):
        x,t=duty_fixture(7,.9);r=self.run_iq(x)
        self.assertEqual(len(one_to_one_matches(r['regions'],t)),2)
        self.assertIsNone(r['drone_presence']);self.assertIsNone(r['accepted_identity'])
        self.assertFalse(r['training_eligible']);self.assertFalse(r['history_persisted'])

    def test_weak_wide_burst_and_noise_development_cases(self):
        for name,x,rate,t in cases(7):
            if name not in ('white_noise','colored_noise','signal_minus5db'):continue
            r=self.run_iq(x,rate)
            if not t:self.assertEqual(r['regions'],[])
            else:
                pairs=one_to_one_matches(r['regions'],t)
                self.assertIn(0,[p['reference_index'] for p in pairs])

    def test_common_mode_gain_step_is_ambiguous(self):
        _,x,rate,_=next(c for c in cases(7) if c[0]=='noise_gain_step')
        r=self.run_iq(x,rate)
        self.assertTrue(any(p['broad_common_mode_ambiguity'] for p in r['regions']))
        self.assertIsNone(r['drone_presence'])

    def test_unverified_continuity_defers_before_source_read(self):
        def fail(*args):raise AssertionError('unexpected IQ access')
        for scope,segs in [('unverified',[(0,400000)]),('declared_contiguous',None)]:
            r=localize(fail,400000,20e6,.02,continuity=scope,segments=segs)
            self.assertEqual(r['status'],'continuity_deferred');self.assertIsNone(r['drone_presence'])

    def test_segments_never_read_or_merge_across_gap(self):
        x,t=duty_fixture(7,.9);ranges=[(0,150000),(250000,400000)];calls=[]
        def get(s,n):
            self.assertTrue(any(a<=s and s+n<=b for a,b in ranges));calls.append((s,n));return x[s:s+n]
        r=localize(get,len(x),20e6,.02,segments=ranges,continuity='declared_contiguous')
        self.assertTrue(calls);self.assertEqual(r['undeclared_gap_samples'],100000)
        for p in r['regions']:
            a,b=ranges[p['segment_id']];self.assertTrue(a<=p['start_sample']<p['stop_sample']<=b)
        self.assertFalse(r['cross_block_stitching'])

    def test_sample_index_analysis_does_not_claim_wall_time(self):
        x=np.zeros(400000,dtype='<c8')
        r=localize(lambda s,n:x[s:s+n],len(x),20e6,.02,segments=[(0,len(x))],continuity='sample_index_only')
        self.assertFalse(r['wall_time_interpretation_allowed']);self.assertEqual(r['regions'],[])
        self.assertTrue(r['fallback']['required']);self.assertIsNone(r['drone_presence'])

    def test_invalid_segments_rejected_before_read(self):
        def fail(*args):raise AssertionError('unexpected IQ access')
        for spans in [[(0,200000),(100000,400000)],[(0,400001)],[(10,0)],[]]:
            with self.assertRaises(ValueError):localize(fail,400000,20e6,.02,segments=spans,continuity='declared_contiguous')

    def test_invalid_samples_thresholds_and_short_reads(self):
        with self.assertRaises(ValueError):replace(ContextConfig(),reference_quantile=float('nan')).validate()
        with self.assertRaises(ValueError):chunk(np.full(20000,np.nan),20e6,0)
        with self.assertRaisesRegex(ValueError,'short'):
            localize(lambda s,n:np.zeros(n-1),400000,20e6,.02,segments=[(0,400000)],continuity='synthetic_contiguous')

    def test_budgets_report_drops(self):
        x,t=duty_fixture(7,.3);r=self.run_iq(x,config=replace(ContextConfig(),max_regions_per_record=1))
        self.assertEqual(len(r['regions']),1);self.assertGreater(r['dropped_regions'],0)

    def test_gain_invariance_and_no_cross_call_history(self):
        x,t=duty_fixture(7,.3)
        fields=lambda r:[tuple(p[k] for k in ('start_sample','stop_sample','low_hz','high_hz')) for p in r['regions']]
        a=fields(self.run_iq(x));self.run_iq(np.zeros(len(x),dtype='<c8'))
        self.assertEqual(a,fields(self.run_iq(x*16)))


if __name__=='__main__':unittest.main()
