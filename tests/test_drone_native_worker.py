"""Offline worker protocol tests; synthetic silence is not a no-drone corpus."""
import json
import struct
import subprocess
import sys
import unittest
from pathlib import Path

WORKER = sys.argv.pop(1)
ROOT = Path(__file__).resolve().parents[1]


def frame(data, rate=10e6, time=0., epoch=1, centre=2444.5e6):
    return struct.pack('<4sIdddQ', b'DDIQ', len(data)//8, rate, centre, time, epoch)+data


class NativeWorkerTests(unittest.TestCase):
    def run_frames(self, data, *args):
        run = subprocess.run([WORKER, *args], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
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
        # Payload contract: what the packet says, with the model taken from the product table.
        self.assertEqual((p['contract'], p['payload_version'], p['identity_readable']), ('droneid-packet-v1', 2, True))
        self.assertEqual((p['product_code'], p['model'], p['model_source']), (58, 'Mavic Air 2', 'table'))
        self.assertEqual(p['reported_model'], 'Mavic Air 2')

    def test_second_recorded_model_and_channel_selection(self):
        data = (ROOT/'tests/fixtures/drones/dji_mini2.fc32').read_bytes()
        packets = self.run_frames(frame(data, 50e6))[0]['native_packets']
        self.assertGreaterEqual(len(packets), 1)
        for p in packets:
            self.assertEqual((p['product_code'], p['model'], p['identity_readable']), (63, 'Mini 2', True))
            # The fixture's centre metadata is inherited, so the automatic pass finds it; a payload
            # is never reported twice even though documented channels are tried as well.
            self.assertIn(p['channel_selection'], ('documented_droneid_channel',
                                                   'strongest_10MHz_per_window_not_emitter_identification'))
        self.assertEqual(len({(p['payload_hex'], p['sample_index']) for p in packets}), len(packets))

    def test_parallel_decode_gives_the_sequential_output(self):
        data = (ROOT/'tests/fixtures/drones/dji_mini2.fc32').read_bytes()
        # Two messages, so the overlap carried between messages and the duplicate filter are covered.
        half = (len(data)//16)*8
        stream = frame(data[:half], 50e6) + frame(data[half:], 50e6, time=half/8/50e6)
        def rows(*args):
            out = self.run_frames(stream, *args)
            for row in out:
                row.pop('processing_ms')
            return out
        sequential = rows('--threads', '1')
        self.assertGreaterEqual(sum(len(r['native_packets']) for r in sequential), 1)
        self.assertEqual(rows('--threads', '6'), sequential)
        self.assertEqual(rows(), sequential)
        bad = subprocess.run([WORKER, '--no-such-option'], input=b'', stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        self.assertEqual(bad.returncode, 2)

    def test_listed_channels_only_keeps_automatic_pass_when_no_channel_is_in_view(self):
        data = (ROOT/'tests/fixtures/drones/dji_mavic_air_2.fc32').read_bytes()
        # Away from every listed channel the option changes nothing: the automatic pass still runs.
        packets = self.run_frames(frame(data, 50e6, centre=2600e6), '--listed-channels-only')[0]['native_packets']
        self.assertEqual([p['product_code'] for p in packets], [58])
        self.assertNotEqual(packets[0]['channel_selection'], 'documented_droneid_channel')
        # With listed channels in view only those are tried, so nothing is labelled automatic.
        packets = self.run_frames(frame(data, 50e6), '--listed-channels-only')[0]['native_packets']
        self.assertTrue(all(p['channel_selection'] == 'documented_droneid_channel' for p in packets))

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
