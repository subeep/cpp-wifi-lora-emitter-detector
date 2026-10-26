import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

from tools.drone_ml.common import sha256, stamp
from tools.drone_ml.localizer_audit import audit
from tools.drone_ml.localizer_review_pack import build, selections


class LocalizerReviewPackTests(unittest.TestCase):
    def test_uniform_selection_independent_of_proposals_and_truncated_parent(self):
        row=dict(source_sha256='ab'*32,dataset_key='capture')
        record=dict(source=row,analysed_samples=2000000,regions=[])
        first=selections(record,20e6,320000)
        record['regions']=[dict(id=7,start_sample=0,stop_sample=2000000,low_hz=-1e6,high_hz=1e6,
                                branches=['stable_envelope'],stable_background_ambiguity=True)]
        second=selections(record,20e6,320000)
        self.assertEqual(first[0]['start_sample'],second[0]['start_sample'])
        self.assertTrue(any('stable_ambiguous' in s['reasons'] for s in second))
        self.assertTrue(all(0<=s['start_sample']<=1680000 for s in second))

    def test_pack_has_no_labels_preserves_source_and_rejects_protected(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);src=root/'sources';src.mkdir();meta=root/'meta';meta.mkdir()
            path=src/'zero.cf32';np.zeros(400000,dtype='<c8').tofile(path)
            row=dict(path=str(path),source_sha256=sha256(path),sample_rate_hz=20000000,
                     adapter='cf32_le',collection_group='synthetic',partition='development_only')
            manifest=meta/'sources.json';manifest.write_text(json.dumps(dict(
                schema='rfmon.drone_ml.temporal_sources.v1',records=[row])))
            audit(manifest,root/'audit',.02,algorithm='multiscale')
            report=root/'audit/report.json';before=stamp(path)
            result=build([report],root/'pack')
            self.assertEqual(stamp(path),before);self.assertFalse(result['labels_created'])
            self.assertFalse(result['training_eligible']);self.assertFalse(result['final_test_iq_read'])
            entry=result['records'][0]
            self.assertEqual(entry['review_status'],'unreviewed')
            self.assertIsNone(entry['verified_presence_label']);self.assertIsNone(entry['verified_identity_label'])
            with np.load(root/'pack'/entry['tensor_file']) as data:
                self.assertEqual(list(data['log_power_db'].shape),entry['shape'])
                self.assertEqual(data['frequency_offset_hz'].size,1024)
            with self.assertRaises(FileExistsError):build([report],root/'pack')
            # Metadata-only protected source must be rejected before IQ access.
            bad=json.loads(report.read_text());bad['records'][0]['source']['partition']='unknown_test_locked'
            bad['records'][0]['source']['path']='/nonexistent/protected.cf32'
            protected=meta/'protected.json';protected.write_text(json.dumps(bad))
            with self.assertRaisesRegex(ValueError,'reserved'):build([protected],root/'blocked')
            self.assertFalse((root/'blocked').exists())


if __name__=='__main__':unittest.main()
