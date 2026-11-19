import json
import tempfile
import unittest
from pathlib import Path

import numpy as np

from tools.drone_ml.universal_features import NAMES
from tools.drone_ml.universal_presence_energy import DUTY, FLATNESS, build, decide


def facts(duty, flatness):
    v = np.zeros(len(NAMES), np.float32); v[DUTY] = duty; v[FLATNESS] = flatness
    return v


class EnergyPresenceTests(unittest.TestCase):
    def test_rule(self):
        self.assertEqual(decide(facts(.16, .01)), 'signal_present')      # hopping bursts
        self.assertEqual(decide(facts(0, .80)), 'source_noise')          # noise with the receiver spur
        self.assertEqual(decide(facts(0, .98)), 'source_noise')
        self.assertEqual(decide(facts(0, .05)), 'unknown')               # continuous signal, no quiet part
        self.assertEqual(decide(facts(np.nan, .9)), 'unknown')

    def test_only_anechoic_non_ofdm_classes_are_labelled(self):
        with tempfile.TemporaryDirectory() as d:
            store, presence = Path(d) / 'store', Path(d) / 'presence'
            store.mkdir(); presence.mkdir()
            rows = {'a' * 24: ('KULeuven_DroneRF', 'Spektrum DX4e'), 'b' * 24: ('RFUAV', 'FUTABA T14SG'),
                    'c' * 24: ('KULeuven_DroneRF', 'Mini 2'), 'd' * 24: ('KULeuven_DroneRF', 'Q205')}
            for ident, (dataset, model) in rows.items():
                (store / (ident + '.json')).write_text(json.dumps(dict(source_record=dict(dataset=dataset, model=model))))
                if ident != 'd' * 24:
                    np.savez(presence / (ident + '.npz'), values=np.stack([facts(.2, .01), facts(0, .9)]),
                             available=np.ones((2, len(NAMES)), bool))
            report = build(store, presence, Path(d) / 'overlay.json')
            self.assertEqual(set(report['labels']), {'a' * 24})           # RFUAV and OFDM classes untouched
            self.assertEqual(report['labels']['a' * 24]['states'], ['signal_present', 'source_noise'])
            self.assertEqual(report['records_without_features'], ['d' * 24])
            self.assertEqual(report['by_class'], {'Spektrum DX4e': {'signal_present': 1, 'source_noise': 1}})


if __name__ == '__main__':
    unittest.main()
