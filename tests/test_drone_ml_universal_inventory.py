"""No RF data: prove locked paths cannot be opened through names or aliases."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from tools.drone_ml.universal_inventory import SourceGuard, locked_name, proposal
from tools.drone_ml.universal_classes import build


class UniversalInventoryTests(unittest.TestCase):
    def test_locked_families_and_admitted_boundaries(self):
        for name in ('mavicAir2_5_hover.mat', 'mavicAir2_8_hover.mat', 'mavicAir2s_5_hover.mat',
                     'mavicAir2s_7_hover.mat', 'mini4PRO_5_hover.mat', 'mavic3C_1.mat', 'mavic3S_1.mat'):
            self.assertTrue(locked_name(name), name)
        for name in ('mavicAir2_4_hover.mat', 'mavicAir2s_4_hover.mat', 'mini4PRO_4_hover.mat',
                     'mavic3_1_hover.mat', 'mini3pro_1_hover.mat'):
            self.assertFalse(locked_name(name), name)

    def test_guard_rejects_before_open_including_aliases(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            locked = root/'mavicAir2_5_hover.mat'; locked.write_bytes(b'never read')
            alias = root/'alias.mat'; alias.symlink_to(locked)
            hardlink = root/'hardlink.mat'; os.link(locked, hardlink)
            safe = root/'mavicAir2_1_hover.mat'; safe.write_bytes(b'safe')
            guard = SourceGuard(root)
            with patch('tools.drone_ml.universal_inventory.os.open', side_effect=AssertionError('opened')):
                for path in (locked, alias, hardlink):
                    with self.assertRaises(PermissionError): guard.open(path)
            with self.assertRaises(PermissionError): guard.check(safe, '../outside.mat')
            with self.assertRaises(PermissionError): guard.check(safe, 'mavic3S_1.mat')
            with guard.open(safe) as f: self.assertEqual(f.read(), b'safe')

    def test_types_do_not_silently_name_unresolved_rows(self):
        self.assertIsNone(proposal(None, 'RFUAV')[0])
        self.assertEqual(proposal('Inspire 2', 'Tampere_DroneRF')[0], 'lightbridge')
        self.assertEqual(proposal('not_a_drone', 'Own_X310')[0], 'not_a_drone')

    def test_class_reporting_does_not_infer_cross_dataset_independence(self):
        def row(source, unit, flags=()):
            return dict(model='Mini 3', dataset=source, aircraft=unit, flags=list(flags),
                        signal_type='dji_ofdm', type_reason='test fixture')
        rows = [row('one', 'A'), row('one', 'B'), row('two', 'C')]
        self.assertEqual(build({'records': rows})['classes'][0]['reporting_flag'], 'single_unit')
        rows.append(row('one', 'D', ['locked_unreadable']))
        self.assertEqual(build({'records': rows})['classes'][0]['reporting_flag'], 'single_unit')
        rows.append(row('one', 'E'))
        self.assertEqual(build({'records': rows})['classes'][0]['reporting_flag'], 'multi_aircraft')


if __name__ == '__main__':
    unittest.main()
