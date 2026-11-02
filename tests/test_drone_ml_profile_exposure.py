import unittest
from tools.drone_ml.profile_exposure_audit import match_rows


class ExposureTests(unittest.TestCase):
    def test_container_exposure_and_reserved_members_are_not_cleared(self):
        rows = [dict(source='/data/a.zip', member=m, recording_id=m, partition=p, split_group='same-aircraft')
                for m, p in [('one.mat', 'train'), ('two.mat', 'unknown_test_locked')]]
        actual = match_rows(rows, [dict(path='/data/a.zip', member='one.mat'),
                                  dict(path='/data/a.zip'), dict(path='/different/a.zip')])
        self.assertEqual(actual[0]['partitions'], ['train'])
        self.assertTrue(actual[1]['locked_overlap'])
        self.assertEqual(len(actual[1]['matches']), 2)
        self.assertEqual(actual[2]['status'], 'unmatched_not_cleared')


if __name__ == '__main__':
    unittest.main()
