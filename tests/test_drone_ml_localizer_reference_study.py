from dataclasses import replace
import unittest

import numpy as np

from tools.drone_ml.localizer_audit import audit
from tools.drone_ml.localizer_reference_study import LOWER_TAIL, duty_fixture, one_to_one_matches
from tools.drone_ml.rf_localizer_multiscale import MultiscaleConfig, localize


class ReferenceStudyTests(unittest.TestCase):
    def test_one_large_proposal_cannot_match_two_bursts(self):
        truth = [dict(start=s, count=10, low_hz=0, high_hz=1) for s in (0, 10)]
        region = dict(id=9, start_sample=0, stop_sample=20, low_hz=0, high_hz=1)
        self.assertEqual(len(one_to_one_matches([region], truth)), 1)
        regions = [dict(region, id=i, start_sample=s, stop_sample=s+10) for i,s in enumerate((0,10))]
        self.assertEqual(len(one_to_one_matches(regions, truth)), 2)
        self.assertEqual(one_to_one_matches([], truth), [])

    def test_assignment_maximizes_match_count_before_iou(self):
        truth = [dict(start=s, count=10, low_hz=0, high_hz=1) for s in (0, 5)]
        regions = [dict(id=i, start_sample=s, stop_sample=s+10, low_hz=0, high_hz=1)
                   for i,s in enumerate((0,-3))]
        matches = one_to_one_matches(regions, truth, .3)
        self.assertEqual({(p['reference_index'],p['proposal_id']) for p in matches}, {(0,1),(1,0)})

    def test_high_duty_development_fixture_preserves_two_bursts(self):
        x, truth = duty_fixture(7, .7)
        results = [localize(lambda s,n:x[s:s+n],len(x),20e6,.02,c) for c in (MultiscaleConfig(),LOWER_TAIL)]
        self.assertEqual(len(one_to_one_matches(results[0]['regions'],truth)),0)
        self.assertEqual(len(one_to_one_matches(results[1]['regions'],truth)),2)
        self.assertIsNone(results[1]['drone_presence'])
        self.assertIsNone(results[1]['accepted_identity'])
        self.assertFalse(results[1]['training_eligible'])

    def test_lower_tail_zero_input_is_unknown_and_requires_fallback(self):
        x=np.zeros(400000,dtype='<c8')
        r=localize(lambda s,n:x[s:s+n],len(x),20e6,.02,LOWER_TAIL)
        self.assertEqual(r['regions'],[])
        self.assertIsNone(r['drone_presence'])
        self.assertTrue(r['fallback']['required'])
        self.assertEqual(r['algorithm'],'multiscale_lower_tail_v1')

    def test_reference_mismatch_rejected_before_source_access(self):
        with self.assertRaisesRegex(ValueError,'reference'):
            replace(LOWER_TAIL,reference_method='invalid').validate()
        for algorithm,config in [('multiscale',LOWER_TAIL),('multiscale_lower_tail',MultiscaleConfig())]:
            with self.assertRaisesRegex(ValueError,'mismatch'):
                audit('/missing/source','/missing/output',config=config,algorithm=algorithm)


if __name__=='__main__':unittest.main()
