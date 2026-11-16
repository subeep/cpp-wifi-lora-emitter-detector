import unittest
from tools.drone_ml.universal_background import validate_split

class BackgroundSplitTests(unittest.TestCase):
    def rows(self, session):
        return [dict(session=session, source='/tmp/'+session+str(i), sha256=session+str(i),
                     center_hz=f, samples=20_000_000, rate_hz=20_000_000)
                for i,f in enumerate((2412e6,5180e6,5785e6))]
    def test_whole_sessions_and_all_bands(self):
        validate_split(dict(train=self.rows('train'),held_out=self.rows('held')))
    def test_session_leakage_rejected(self):
        with self.assertRaises(ValueError):
            validate_split(dict(train=self.rows('same'),held_out=self.rows('same')))
    def test_copied_capture_rejected(self):
        parts=dict(train=self.rows('train'),held_out=self.rows('held'))
        parts['held_out'][0]['sha256']=parts['train'][0]['sha256']
        with self.assertRaises(ValueError): validate_split(parts)
    def test_missing_band_rejected(self):
        with self.assertRaises(ValueError):
            validate_split(dict(train=self.rows('train'),held_out=self.rows('held')[:2]))
if __name__=='__main__': unittest.main()
