"""Offline worker protocol tests; synthetic silence is not a no-drone corpus."""
import json
import struct
import subprocess
import sys
import unittest
from pathlib import Path

WORKER = sys.argv.pop(1)
ROOT = Path(__file__).resolve().parents[1]


def frame(data, rate=10e6, time=0., epoch=1):
    return struct.pack('<4sIdddQ', b'DDIQ', len(data)//8, rate, 2444.5e6, time, epoch)+data


class NativeWorkerTests(unittest.TestCase):
    def run_frames(self, data):
        run = subprocess.run([WORKER], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr.decode())
        rows = [json.loads(line) for line in run.stdout.splitlines()]
        self.assertTrue(rows.pop(0)['ready'])
        for row in rows:
            self.assertNotIn('error', row)
            self.assertEqual(row['events'], [])
            self.assertFalse(row['identity_confirmed'])
        return rows

    def test_recorded_dji_packet_is_separate_unauthenticated_evidence(self):
        data = (ROOT/'tests/fixtures/drones/dji_mavic_air_2.fc32').read_bytes()
        rows = self.run_frames(frame(data, 50e6))
        self.assertEqual(len(rows[0]['native_packets']), 1)
        p = rows[0]['native_packets'][0]
        self.assertTrue(p['outer_crc24']); self.assertTrue(p['inner_crc16'])
        self.assertEqual(p['reported_serial'], '1WNBH3900201N1')
        self.assertEqual(p['generation'], 'unverified')
        self.assertFalse(p['identity_authenticated'])

    def test_o4_context_requires_continuity(self):
        silence = bytes(80000*8)
        for second, epoch, expected in ((.008, 1, 1), (.016, 1, 0), (.008, 2, 0)):
            rows = self.run_frames(frame(silence)+frame(silence, time=second, epoch=epoch))
            self.assertEqual(len(rows[0]['o4_evidence']), 0)
            self.assertEqual(len(rows[1]['o4_evidence']), expected)
            if expected:
                e = rows[1]['o4_evidence'][0]
                self.assertFalse(e['candidate']); self.assertEqual(e['timestamp'], 0.)

    def test_packet_crossing_job_boundary_retains_context(self):
        data = (ROOT/'tests/fixtures/drones/dji_mavic_air_2.fc32').read_bytes()
        split = 180000
        rows = self.run_frames(frame(data[:split*8], 50e6)+
                               frame(data[split*8:], 50e6, time=split/50e6))
        recovered = [p for row in rows for p in row['native_packets'] if p.get('sequence') == 591]
        self.assertEqual(len(recovered), 1)
        self.assertEqual(recovered[0]['reported_serial'], '1WNBH3900201N1')

    def test_nonfinite_input_reports_error_and_recovers(self):
        bad = struct.pack('<ff', float('nan'), 0.)
        run = subprocess.run([WORKER], input=frame(bad)+frame(bytes(160000*8), time=.01),
                             stdout=subprocess.PIPE, timeout=30)
        rows = [json.loads(line) for line in run.stdout.splitlines()]
        self.assertIn('non-finite', rows[1]['error'])
        self.assertNotIn('error', rows[2])
        self.assertEqual(len(rows[2]['o4_evidence']), 1)

    def test_bad_or_oversized_header_is_rejected_before_payload_allocation(self):
        for header in (b'broken', struct.pack('<4sIdddQ', b'DDIQ', 40000001, 250e6, 2444.5e6, 0., 1),
                       struct.pack('<4sIdddQ', b'DDIQ', 2000000, 10e6, 2444.5e6, 0., 1)):
            run = subprocess.run([WORKER], input=header, stdout=subprocess.PIPE, timeout=5)
            self.assertEqual(run.returncode, 2)


if __name__ == '__main__':
    unittest.main()
