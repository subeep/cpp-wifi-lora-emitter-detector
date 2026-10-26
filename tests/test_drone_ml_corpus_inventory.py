import copy
import hashlib
from pathlib import Path
import tempfile
import unittest

from tools.drone_ml.common import stamp
from tools.drone_ml.corpus_inventory import compile_inventory,publisher_matches,verify_selected


def fixture():
    row=dict(source='/unopened/DJI_mavic_pro_2G.bin',source_stat=[0,0,480000000,0,0,0],dataset='Tampere_DroneRF',
        adapter='ci16',label='DJI_Mavic_Pro',partition='train',split_group='unit_unknown',rate_hz=120000000,
        center_hz=2440000000,total_samples=120000000,recording_id='id1',physical_unit=None,role='drone_controller_context')
    groups={'unit_unknown':dict(partition='train',label='DJI_Mavic_Pro')}
    review=dict(schema='rfmon.drone_ml.expanded_review.v1',records=[row],source_decisions=[])
    plan=dict(schema='rfmon.drone_ml.expanded_split.v1',groups=groups)
    audit=dict(frozen_records=copy.deepcopy(review['records']),frozen_groups=copy.deepcopy(groups),derivative_lineage=[],nist_records=[],fpv_sessions=[])
    catalog=dict(schema='rfmon.drone_ml.publisher_catalog.v1',version='1.0.1',files=[dict(name='DJI_mavic_pro_2G.bin',md5='a'*32)])
    verified=[dict(path=row['source'],md5='a'*32,source_sha256='b'*64,partition='train',recording_id='id1')]
    return review,plan,audit,catalog,verified,dict(records=[])


class CorpusInventoryTests(unittest.TestCase):
    def test_verified_file_becomes_only_recording_bag(self):
        r=compile_inventory(*fixture());entry=r['records'][0]
        self.assertTrue(entry['training_eligible']);self.assertEqual(entry['supervision_scope'],'recording_bag')
        self.assertFalse(entry['per_crop_label_inheritance']);self.assertFalse(r['training_ready'])
        self.assertEqual(r['summary']['verified_negative_records'],0)

    def test_locked_partition_rejected_before_verification(self):
        review,plan,audit,catalog,verified,local=fixture();review['records'][0]['partition']='test_locked'
        with self.assertRaisesRegex(ValueError,'train partition'):publisher_matches(review['records'],catalog)

    def test_missing_mismatched_or_duplicate_hash_receipts_rejected(self):
        for change in ('missing','wrong_md5','duplicate'):
            args=list(fixture())
            if change=='missing':args[4]=[]
            elif change=='wrong_md5':args[4][0]['md5']='c'*32
            else:args[4].append(copy.deepcopy(args[4][0]))
            with self.assertRaises(ValueError):compile_inventory(*args)

    def test_frozen_split_and_label_changes_rejected(self):
        args=list(fixture());args[0]['records'][0]['label']='different'
        with self.assertRaisesRegex(ValueError,'mismatch'):compile_inventory(*args)
        args=list(fixture());args[2]['frozen_groups']['unit_unknown']['partition']='calibration'
        with self.assertRaisesRegex(ValueError,'frozen'):compile_inventory(*args)

    def test_derivatives_never_become_independent_validation(self):
        args=list(fixture());r=args[0]['records'][0]
        args[2]['derivative_lineage']=[dict(path='/derived.cf32',source=r['source'],member=None,
            derivative_partition='development_only',original_partitions=['train'])]
        inventory=compile_inventory(*args);child=inventory['records'][1]
        self.assertEqual(child['collection_group'],'unit_unknown')
        self.assertFalse(child['independent_evaluation_eligible']);self.assertFalse(child['training_eligible'])

    def test_extra_publisher_file_cannot_override_exclusion(self):
        args=list(fixture());args[3]['files'].append(dict(name='Parrot_mambo_control_2G.bin',md5='c'*32))
        args[0]['source_decisions']=[dict(source='/unopened/Parrot_mambo_control_2G.bin',dataset='Tampere_DroneRF',use='excluded',reason='controller_only')]
        inventory=compile_inventory(*args)
        self.assertEqual(inventory['summary']['verified_bags'],1)
        self.assertEqual(inventory['publisher_files_not_admitted'][0]['prior_decision']['use'],'excluded')

    def test_bad_publisher_catalog_and_format_rejected(self):
        args=list(fixture());args[3]['files'][0]['name']='../escape.bin'
        with self.assertRaises(ValueError):publisher_matches(args[0]['records'],args[3])
        args=list(fixture());args[0]['records'][0]['rate_hz']=100000000
        with self.assertRaisesRegex(ValueError,'format'):publisher_matches(args[0]['records'],args[3])

    def test_checksums_preserve_source_stats_and_reject_mismatch(self):
        with tempfile.TemporaryDirectory() as temp:
            p=Path(temp)/'small.bin';payload=b'01234567';p.write_bytes(payload)
            row=dict(source=str(p),total_samples=2,partition='train',recording_id='synthetic')
            pub=dict(md5=hashlib.md5(payload).hexdigest());before=stamp(p)
            result=verify_selected([(row,pub)],Path(temp))
            self.assertEqual(stamp(p),before);self.assertEqual(result[0]['source_sha256'],hashlib.sha256(payload).hexdigest())
            with self.assertRaisesRegex(ValueError,'MD5'):verify_selected([(row,dict(md5='0'*32))],Path(temp))


if __name__=='__main__':unittest.main()
