import json
from pathlib import Path
import tempfile
import unittest

from tools.drone_ml.universal_dronerfa import CODES, blocks_for, channels, parse_name, recording_id, source_record, unpack

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = Path('/media/sudeep/Storage/datasets/DroneRFa/DroneRFa.rar.part')
LAYOUT = ROOT / 'data/drone_ml/2026-11-16/e4/layout_partial.json'


class DroneRFaTests(unittest.TestCase):
    def test_names_and_channels(self):
        self.assertEqual(parse_name('DroneRFa/T0010_D01_S1110.mat'), dict(code='T0010', distance='D01', segment='S1110'))
        self.assertEqual(parse_name('DroneRFa/T10100_S0000.mat'), dict(code='T10100', distance=None, segment='S0000'))
        self.assertIsNone(parse_name('DroneRFa/readme.pdf'))
        self.assertIsNone(parse_name('DroneRFa/T11111_S0000.mat'))          # not in the paper's table
        self.assertEqual(CODES['T0010'], ('Phantom 4 Pro', 'lightbridge'))
        self.assertEqual(channels('T0101'), [('RF0', 2440e6), ('RF1', 5800e6)])
        self.assertEqual(channels('T10010'), [('RF1', 2440e6)])               # 915 MHz channel not used

    def test_blocks_fit_shorter_files(self):
        self.assertEqual(blocks_for(150_000_000), (2, 7, 12))
        self.assertEqual(blocks_for(120_000_000), (2, 7, 11))              # 12-block files: last block instead
        with self.assertRaises(ValueError):
            blocks_for(20_000_000)

    def test_one_recording_per_channel_with_session(self):
        member = dict(name='DroneRFa/T1010_S0110.mat', crc=1, unpacked=2)
        rows = [source_record(ARCHIVE, member, parse_name(member['name']), ch, c) for ch, c in channels('T1010')]
        self.assertEqual(len({r['recording_id'] for r in rows}), 2)
        self.assertEqual(rows[0]['recording_id'], recording_id('DroneRFa.rar', member['name'], 'RF0'))
        self.assertEqual((rows[0]['model'], rows[0]['setting'], rows[0]['session']), ('Mavic 3', 'indoor', 'T1010_indoor'))
        self.assertEqual(rows[1]['center_hz'], 5800e6)

    @unittest.skipUnless(ARCHIVE.exists() and LAYOUT.exists(), 'DroneRFa archive or layout missing')
    def test_one_member_unpacked_with_crc_check(self):
        pdf = next(m for m in json.load(open(LAYOUT)) if m.get('name', '').endswith('.pdf'))
        with tempfile.TemporaryDirectory(dir='/dev/shm') as work:
            path, message = unpack(ARCHIVE, pdf, work)
            self.assertIsNotNone(path, message)
            self.assertTrue(path.read_bytes().startswith(b'%PDF'))
        broken = dict(pdf, end=pdf['end'] - 1000)                            # truncated data: refused
        with tempfile.TemporaryDirectory(dir='/dev/shm') as work:
            self.assertIsNone(unpack(ARCHIVE, broken, work)[0])


if __name__ == '__main__':
    unittest.main()
