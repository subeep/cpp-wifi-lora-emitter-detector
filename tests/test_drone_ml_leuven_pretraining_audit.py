import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import h5py
import numpy as np

from tools.drone_ml.common import stamp
from tools.drone_ml.leuven_pretraining_audit import audit, member_metadata, selected_members


def fixture():
    row = dict(dataset='KULeuven_DroneRF', label='DJI_Mini_2', source='/unopened/mini2vid.zip',
        partition='train', split_group='unit_unknown', adapter='leuven', rate_hz=100_000_000,
        center_hz=2_440_000_000, role='video_or_combined_context', member='mini2vid/mini2_0.mat',
        member_bytes=4096, recording_id='fixture')
    return (dict(schema='rfmon.drone_ml.expanded_review.v1', records=[row]),
            dict(schema='rfmon.drone_ml.expanded_split.v1', groups={'unit_unknown': dict(partition='train', label='DJI_Mini_2')}))


def payload(shape=(1, 100)):
    f = io.BytesIO()
    with h5py.File(f, 'w') as h:
        h.create_dataset('signal', data=np.zeros(shape, dtype=[('real', '<f8'), ('imag', '<f8')]))
    return f.getvalue()


class LeuvenPretrainingAuditTests(unittest.TestCase):
    def test_reserved_archive_member_blocks_entire_archive(self):
        review, plan = fixture()
        extra = copy.deepcopy(review['records'][0])
        extra.update(label='Other', member='other.mat', partition='test_locked', split_group='reserved')
        review['records'].append(extra)
        plan['groups']['reserved'] = dict(label='Other', partition='test_locked')
        with self.assertRaisesRegex(ValueError, 'reserved'):
            selected_members(review, plan)

    def test_wrong_role_or_rate_rejected(self):
        for key, value in [('role', 'controller_only'), ('rate_hz', 20_000_000)]:
            review, plan = fixture(); review['records'][0][key] = value
            with self.assertRaisesRegex(ValueError, 'scope or format'):
                selected_members(review, plan)

    def test_metadata_shape_check(self):
        self.assertEqual(member_metadata(payload())['total_samples'], 100)
        with self.assertRaisesRegex(ValueError, 'shape'):
            member_metadata(payload((2, 50)))

    def test_integrity_receipt_and_checksum_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); source_dir = root/'sources'; source_dir.mkdir()
            archive = source_dir/'mini2vid.zip'; data = payload()
            review, plan = fixture(); row = review['records'][0]
            with zipfile.ZipFile(archive, 'w') as z: z.writestr(row['member'], data)
            with zipfile.ZipFile(archive) as z: row['member_crc32'] = z.getinfo(row['member']).CRC
            row.update(source=str(archive), member_bytes=len(data))
            rp, pp = source_dir/'review.json', source_dir/'plan.json'
            rp.write_text(json.dumps(review)); pp.write_text(json.dumps(plan))
            digest = hashlib.md5(archive.read_bytes()).hexdigest(); before = stamp(archive)
            with patch('tools.drone_ml.leuven_pretraining_audit.PUBLISHER_MD5', digest):
                result = audit(rp, pp, root/'audit')
            self.assertEqual(stamp(archive), before)
            self.assertFalse(result['training_ready']); self.assertFalse(result['scoped_training_exported'])
            self.assertEqual(result['members'][0]['member_sha256'], hashlib.sha256(data).hexdigest())
            with patch('tools.drone_ml.leuven_pretraining_audit.PUBLISHER_MD5', '0'*32):
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    audit(rp, pp, root/'bad')


if __name__ == '__main__': unittest.main()
