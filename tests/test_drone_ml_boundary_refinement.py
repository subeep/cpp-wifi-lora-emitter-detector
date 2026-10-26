from dataclasses import replace
import unittest

import numpy as np

from tools.drone_ml.boundary_experiment import box_union
from tools.drone_ml.localizer_reference_study import duty_fixture, LOWER_TAIL, one_to_one_matches
from tools.drone_ml.rf_localizer_multiscale import localize
from tools.drone_ml.rf_boundary_refinement import BoundaryConfig, reference_quality, refine, refine_parent


class BoundaryTests(unittest.TestCase):
    def parents(self,duty=.3):
        x,t=duty_fixture(7,duty)
        p=localize(lambda s,n:x[s:s+n],len(x),20e6,.02,LOWER_TAIL)
        return x,t,p

    def test_no_quiet_does_not_become_verified_noise(self):
        q=reference_quality(np.ones(100))
        self.assertFalse(q['usable_for_local_edges']);self.assertFalse(q['verified_noise'])
        q=reference_quality(np.r_[np.ones(20),np.full(80,100.)])
        self.assertTrue(q['usable_for_local_edges']);self.assertFalse(q['verified_noise'])
        q=reference_quality(np.tile([1.,100.,100.,100.],25))
        self.assertFalse(q['usable_for_local_edges'])

    def test_development_bursts_refined_with_parent_lineage(self):
        x,t,p=self.parents();calls=[]
        def get(s,n):
            self.assertGreaterEqual(s,0);self.assertLessEqual(s+n,p['analysed_samples'])
            calls.append(n);return x[s:s+n]
        r=refine(get,p)
        self.assertEqual(len(one_to_one_matches(r['regions'],t)),2)
        self.assertEqual(r['read_samples'],sum(calls));self.assertEqual(r['parent_regions'],p['regions'])
        self.assertIsNone(r['drone_presence']);self.assertFalse(r['training_eligible'])
        parents={v['id']:v for v in p['regions']}
        for core in r['regions']:
            parent=parents[core['parent_id']]
            self.assertLessEqual(parent['start_sample'],core['start_sample'])
            self.assertGreaterEqual(parent['stop_sample'],core['stop_sample'])
            self.assertLessEqual(parent['low_hz'],core['low_hz'])
            self.assertGreaterEqual(parent['high_hz'],core['high_hz'])

    def test_no_state_carried_across_calls_and_gain_scaling(self):
        x,t,p=self.parents()
        fields=lambda r:[tuple(c[k] for k in ('start_sample','stop_sample','low_hz','high_hz')) for c in r['regions']]
        a=refine(lambda s,n:x[s:s+n],p)
        b=refine(lambda s,n:x[s:s+n]*8,p)
        self.assertEqual(fields(a),fields(b))
        self.assertEqual(fields(a),fields(refine(lambda s,n:x[s:s+n],p)))

    def test_budget_defers_without_source_read_and_preserves_context(self):
        x,t,p=self.parents()
        def fail(*args):raise AssertionError('unexpected source read')
        r=refine(fail,p,replace(BoundaryConfig(),max_read_samples=2048))
        self.assertEqual(r['regions'],[]);self.assertEqual(r['read_samples'],0)
        self.assertTrue(all(v['status']=='budget_deferred' for v in r['parent_reviews']))
        self.assertEqual(r['parent_regions'],p['regions'])

    def test_invalid_and_short_inputs_rejected(self):
        x,t,p=self.parents()
        with self.assertRaisesRegex(ValueError,'short'):refine(lambda s,n:x[s:s+n-1],p)
        with self.assertRaises(ValueError):reference_quality(np.array([1.,2.,3.,np.nan]))
        with self.assertRaises(ValueError):replace(BoundaryConfig(),max_parents=0).validate()
        with self.assertRaises(ValueError):refine_parent(np.zeros(2048),20e6,0,p['regions'][0])

    def test_union_is_not_sum_of_overlapping_boxes(self):
        boxes=[dict(start_sample=0,stop_sample=10,low_hz=0,high_hz=2)]*2
        self.assertEqual(box_union(boxes,10,4),.5)
        self.assertEqual(box_union([],10,4),0)

    def test_fragmentation_defers_whole_parent_instead_of_cherry_picking(self):
        x,t,p=self.parents()
        parent=dict(id=0,start_sample=0,stop_sample=len(x),low_hz=2e6,high_hz=6e6,
                    stable_background_ambiguity=False)
        cores,q=refine_parent(x,20e6,0,parent,replace(BoundaryConfig(),max_cores_per_parent=1))
        self.assertEqual(cores,[]);self.assertEqual(q['status'],'fragmentation_deferred')


if __name__=='__main__':unittest.main()
