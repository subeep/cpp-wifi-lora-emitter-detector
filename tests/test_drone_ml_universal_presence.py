import unittest
from tools.drone_ml.universal_presence import label,K15,K30,KLB
class PresenceTests(unittest.TestCase):
    def row(self,model,signal,dataset='RFUAV',**kwargs):return dict(model=model,signal_type=signal,dataset=dataset,**kwargs)
    def test_wrong_family_cannot_supply_model(self):
        r=label(self.row('Mini 4 Pro','dji_ofdm'),{'lag_ratios':{K15:30,K30:1}})
        self.assertFalse(r['model_positive']);self.assertEqual(r['state'],'unknown')
    def test_own_family_and_undetected_are_separate(self):
        row=self.row('Mini 3','dji_ofdm')
        self.assertTrue(label(row,{'lag_ratios':{K15:5}})['model_positive'])
        self.assertEqual(label(row,{'lag_ratios':{K15:1}})['state'],'unknown')
    def test_burst_without_control_stays_unknown(self):
        r=label(self.row('JUMPER T14','hopping_controller'),{'lag_ratios':{K15:20}})
        self.assertEqual(r['reason'],'matching_source_no_signal_control_missing')
    def test_confirmed_background_only(self):
        r=self.row('not_a_drone','not_a_drone','Own_X310',background_partition='train')
        self.assertEqual(label(r,{'lag_ratios':{K30:5}})['state'],'background')
        r=self.row('not_a_drone','not_a_drone','DRFF-R2')
        self.assertEqual(label(r,{'lag_ratios':{}})['state'],'unknown')
    def test_dronerfa_background_only_without_drone_timing(self):
        r=self.row('not_a_drone','not_a_drone','DroneRFa')
        self.assertEqual(label(r,{'lag_ratios':{K15:2,K30:1,KLB:3}})['state'],'background')
        for k in (K15,K30,KLB):
            self.assertEqual(label(r,{'lag_ratios':{k:6}})['state'],'unknown')
    def test_unmeasured_dji_family_takes_either_ofdm_timing(self):
        row=self.row('Matrice 30T','dji_ofdm','DroneRFa')
        self.assertEqual(label(row,{'lag_ratios':{K30:7}})['expected_timing'],K30)
        self.assertEqual(label(row,{'lag_ratios':{K15:7,K30:2}})['expected_timing'],K15)
        self.assertEqual(label(row,{'lag_ratios':{KLB:9}})['state'],'unknown')
if __name__=='__main__':unittest.main()
