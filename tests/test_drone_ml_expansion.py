import sys
import subprocess
import unittest
from unittest.mock import patch
import numpy as np
from tools.drone_ml.expanded_corpus import rar_intervals
from tools.drone_ml.calibration_audit import group_fold


class ExpansionTests(unittest.TestCase):
    def test_streamed_ranges_match_without_loading_member(self):
        spawn=subprocess.Popen
        script='import sys,numpy as np;sys.stdout.buffer.write(np.arange(120,dtype=np.float32).view(np.complex64).tobytes())'
        def fixture(*args,**kwargs):return spawn([sys.executable,'-c',script],**kwargs)
        with patch('tools.drone_ml.expanded_corpus.subprocess.Popen',side_effect=fixture):
            result=list(rar_intervals('fixture','member',[4,18,38],8))
        original=np.arange(120,dtype=np.float32).view(np.complex64)
        for start,iq in result:np.testing.assert_array_equal(iq,original[start:start+8])

    def test_bad_archive_ranges_do_not_start_reader(self):
        with patch('tools.drone_ml.expanded_corpus.subprocess.Popen',side_effect=AssertionError('must not spawn')):
            for starts in ([],[-1],[10,1],[0,4]):
                with self.subTest(starts=starts),self.assertRaises(ValueError):list(rar_intervals('x','m',starts,8))

    def test_temperature_fit_uses_disjoint_groups(self):
        rows=[dict(label=label,split_group=label+str(g)) for label in ('a','b') for g in (1,1,2,2)]
        fit=group_fold(rows)
        self.assertTrue({r['split_group'] for r,f in zip(rows,fit) if f}.isdisjoint(
                        {r['split_group'] for r,f in zip(rows,fit) if not f}))
        with self.assertRaises(ValueError):group_fold([dict(label='a',split_group='only_one')])


if __name__=='__main__':unittest.main()
