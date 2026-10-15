"""Known received fixtures plus rejection/continuity checks; no RF operations."""
import sys
from pathlib import Path
import unittest
import json
import os
for name in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS"): os.environ[name]="1"
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
TURBO=sys.argv.pop(1)
sys.path.insert(0,str(ROOT/'src/drones/python'))
import backend
from telemetry import packet_telemetry

class DecoderTests(unittest.TestCase):
    def capture(self,name):
        return np.fromfile(ROOT/'tests/fixtures/drones'/name,dtype='<c8')
    def test_mavic_air_received(self):
        result=backend.Analyzer(TURBO).analyze(self.capture('dji_mavic_air_2.fc32'),50e6,2444.5e6)
        self.assertEqual(len(result['events']),1)
        event=result['events'][0]
        self.assertEqual(event['model'],'Mavic Air 2')
        self.assertEqual(event['sequence'],591)
        self.assertAlmostEqual(event['latitude'],51.44633393111904)
        payload=json.loads(event['telemetry']['packet_json'])
        self.assertEqual(len(bytes.fromhex(payload['raw_payload_hex'])),91)
        self.assertIn('v_north',payload)
        self.assertIn('state_info',payload)
    def test_corrupt_payload_rejected(self):
        from droneid_packet import DroneIDPacket
        e=backend.Analyzer(TURBO).analyze(self.capture('dji_mavic_air_2.fc32'),50e6,2444.5e6)['events'][0]
        raw=bytearray.fromhex(json.loads(e['telemetry']['packet_json'])['raw_payload_hex'])
        self.assertTrue(DroneIDPacket(raw).check_crc())
        raw[30]^=1
        self.assertFalse(DroneIDPacket(raw).check_crc())
    def test_mini2_received_and_epoch(self):
        a=backend.Analyzer(TURBO); iq=self.capture('dji_mini2.fc32')
        first=a.analyze(iq,50e6,2444.5e6,epoch=1)['events']
        self.assertGreaterEqual(len(first),7)
        self.assertTrue(all(e['model']=='Mini 2' and e['confirmed'] for e in first))
        self.assertFalse(a.analyze(iq,50e6,2444.5e6,epoch=1)['events'])
        self.assertEqual(len(a.analyze(iq,50e6,2444.5e6,epoch=2)['events']),len(first))
    def test_noise_tone_zero(self):
        rng=np.random.default_rng(170)
        for iq in (np.zeros(100000,np.complex64),np.ones(100000,np.complex64),
                   (rng.normal(size=100000)+1j*rng.normal(size=100000)).astype(np.complex64)):
            self.assertFalse(backend.Analyzer(TURBO).analyze(iq,20e6,2444.5e6)['events'])
    def test_bad_inputs(self):
        a=backend.Analyzer(TURBO)
        for rate,freq in ((0,2444e6),(float('nan'),2444e6),(120e6,2444e6),(20e6,868e6)):
            with self.assertRaises(ValueError): a.analyze(np.zeros(10,np.complex64),rate,freq)
        for iq in (np.array([np.nan],np.complex64),np.empty(0,np.complex64),np.zeros(2000001,np.complex64)):
            with self.assertRaises(ValueError): a.analyze(iq,20e6,2444e6)
    def test_zero_position_and_raw_units(self):
        t=packet_telemetry({'latitude':0.,'longitude':0.,'state_info':0,'v_north':0})
        self.assertIn('zero',t['aircraft_position_status'])
        self.assertTrue(any(f['name']=='v_north (raw)' and 'unverified' in f['status'] for f in t['fields']))

if __name__=='__main__': unittest.main()
