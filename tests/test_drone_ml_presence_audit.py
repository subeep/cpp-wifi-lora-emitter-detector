"""Decision isolation, input preservation, evidence provenance and bounded IO."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from tools.drone_ml.input_audit import read_interval, audit_file
from tools.drone_ml.live_audit import AuditRecorder
from tools.drone_ml.presence import apply_unqualified_policy
from tools.drone_ml.preprocess import STFTConfig


class PresenceAuditTests(unittest.TestCase):
    def test_confident_candidate_never_becomes_presence_or_identity(self):
        for status in ('known_class_candidate', 'unknown_rf_signal', 'identification_unavailable', 'insufficient_evidence'):
            original=dict(status=status,accepted_class='DJI_Mini_2',softmax_score=.99999,candidate='DJI_Mini_2')
            result=apply_unqualified_policy(original)
            self.assertIsNone(result['accepted_class'])
            self.assertIsNone(result['drone_present'])
            self.assertIsNone(result['drone_presence_probability'])
            self.assertFalse(result['presence_validated'])
            self.assertEqual(result['candidate'],'DJI_Mini_2')
            self.assertEqual(original['accepted_class'],'DJI_Mini_2')
            self.assertEqual(result['presence_status'],'insufficient_evidence' if status=='insufficient_evidence' else 'unclassified_rf')

    def test_audit_preserves_exact_iq_and_stops_at_byte_budget(self):
        with tempfile.TemporaryDirectory() as tmp:
            iq=np.arange(10,dtype=np.float32).astype('<c8')
            before=iq.copy()
            r=AuditRecorder(tmp,'background_observation','operator context','hash',{})
            with patch('tools.drone_ml.live_audit.MAX_BYTES',80):
                first=r.record(iq,{'capture_seq':'9007199254740993'},{'status':'insufficient_evidence'})
                second=r.record(iq,{}, {})
            self.assertEqual(first['status'],'limit_reached')
            self.assertEqual(second['saved'],1)
            self.assertEqual((r.path/'0000.cf32').read_bytes(),iq.tobytes())
            np.testing.assert_array_equal(iq,before)
            item=json.loads((r.path/'0000.json').read_text())
            self.assertEqual(item['request']['capture_seq'],'9007199254740993')
            self.assertFalse(item['training_eligible'])
            self.assertNotEqual(r.path,AuditRecorder(tmp,'unlabelled','','hash',{}).path)

    def test_recorder_failure_does_not_throw_into_inference(self):
        with tempfile.TemporaryDirectory() as tmp:
            r=AuditRecorder(tmp,'controller_only','','hash',{})
            # Existing files must never be overwritten, even after an error.
            (r.path/'0000.cf32').write_bytes(b'preserve')
            result=r.record(np.ones(10,dtype='<c8'),{}, {})
            self.assertEqual(result['status'],'failed')
            self.assertEqual((r.path/'0000.cf32').read_bytes(),b'preserve')
            self.assertEqual(r.record(np.ones(10),{}, {})['saved'],0)

    def test_bounded_read_and_source_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'input.cf32'
            iq=np.arange(320000,dtype=np.float32).astype('<c8')
            path.write_bytes(iq.tobytes())
            before=path.stat()
            value,_=read_interval(path,20e6,160000)
            np.testing.assert_array_equal(value,iq[160000:])
            after=path.stat()
            self.assertEqual((before.st_atime_ns,before.st_mtime_ns,before.st_size),(after.st_atime_ns,after.st_mtime_ns,after.st_size))
            for rate,offset in ((20e6,320000),(1,0),(20e6,-1),(float('nan'),0)):
                with self.assertRaises(ValueError):read_interval(path,rate,offset)

    def test_audit_samples_later_intervals_without_changing_profile(self):
        class Engine:
            config=STFTConfig(duration_seconds=.008,hop=256,auto_frequency_view=True)
            def predict_iq(self,iq,rate,offset):
                return dict(status='identification_unavailable',candidate='a',selected_offset_hz=self.config.offset_hz)
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'input.cf32'
            path.write_bytes(np.ones(600000,dtype='<c8').tobytes())
            engine=Engine();original=engine.config
            result=audit_file(engine,path,25e6,2,True)
            self.assertEqual([r['offset_samples'] for r in result['rows']],[0,400000])
            self.assertEqual(len(result['rows'][0]['views']),6)
            self.assertIs(engine.config,original)
            self.assertFalse(result['rows'][0]['views'][0]['result']['presence_validated'])


if __name__=='__main__':unittest.main()
