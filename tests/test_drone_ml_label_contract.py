import copy
import json
from pathlib import Path
import tempfile
import unittest

from tools.drone_ml.common import sha256
from tools.drone_ml.label_contract import validate, export


def fixture():
    row=dict(id='a',source=dict(path='/unopened/iq',source_sha256='a'*64,collection_group='one',partition='train'),
        scope='recording_bag',target=dict(presence='drone_system_present',identity='example'),
        evidence=dict(path='/evidence',sha256='b'*64))
    evidence=dict(schema='rfmon.drone_ml.label_evidence.v1',kind='publisher_controlled_recording',reviewed=True,
        reviewer='test',provenance='test fixture only',source_sha256='a'*64,scope=row['scope'],target=row['target'])
    return dict(schema='rfmon.drone_ml.supervision_contract.v1',records=[row]),evidence


class LabelContractTests(unittest.TestCase):
    def test_bag_accepted_only_as_bag(self):
        m,e=fixture();r=validate(m,lambda ref:e)
        self.assertEqual(r['accepted'],1);self.assertEqual(r['records'][0]['training_unit'],'recording_bag')
        m['records'][0]['region']=dict(start_sample=0,stop_sample=10,low_hz=0,high_hz=1)
        self.assertEqual(validate(m,lambda ref:e)['accepted'],0)

    def test_region_cannot_inherit_publisher_recording_label(self):
        m,e=fixture();r=m['records'][0];r['scope']='region';r['boundary_reviewed']=True
        r['region']=dict(start_sample=0,stop_sample=10,low_hz=0,high_hz=1)
        self.assertEqual(validate(m,lambda ref:e)['accepted'],0)
        e=copy.deepcopy(e);e.update(kind='adjudicated_region',scope='region',region=r['region'])
        self.assertEqual(validate(m,lambda ref:e)['accepted'],1)
        e['region']=dict(r['region'],stop_sample=11)
        self.assertEqual(validate(m,lambda ref:e)['accepted'],0)

    def test_target_off_does_not_establish_absence(self):
        m,e=fixture();r=m['records'][0];r.update(scope='controlled_session',target=dict(presence='no_drone',identity=None))
        e.update(kind='controlled_no_drone_session',scope=r['scope'],target=r['target'])
        self.assertEqual(validate(m,lambda ref:e)['accepted'],0)
        e['all_drone_absence_confirmed']=True
        self.assertEqual(validate(m,lambda ref:e)['accepted'],1)

    def test_all_partitions_checked_before_evidence_access(self):
        m,e=fixture();bad=copy.deepcopy(m['records'][0]);bad['id']='b';bad['source']['partition']='test_locked';m['records'].append(bad)
        def fail(ref):raise AssertionError('evidence accessed too early')
        with self.assertRaisesRegex(ValueError,'reserved'):validate(m,fail)

    def test_source_and_group_leakage_rejected(self):
        for same in ('hash','group'):
            m,e=fixture();r=copy.deepcopy(m['records'][0]);r['id']='b';r['source']['partition']='development_only'
            r['source']['source_sha256']='a'*64 if same=='hash' else 'b'*64
            r['source']['collection_group']='other' if same=='hash' else 'one';m['records'].append(r)
            with self.assertRaisesRegex(ValueError,'leakage'):validate(m,lambda ref:e)

    def test_wrong_capture_or_target_rejected(self):
        for change in [dict(dataset_key='wrong'),dict(source_sha256='b'*64),dict(reviewed=False),
                       dict(target=dict(presence='drone_system_present',identity='other'))]:
            m,e=fixture();e.update(change);self.assertEqual(validate(m,lambda ref:e)['accepted'],0)

    def test_context_stays_unlabelled(self):
        m,e=fixture();m['records'][0].update(scope='review_only',target=None)
        def fail(ref):raise AssertionError('context must not require evidence access')
        self.assertEqual(validate(m,fail)['accepted'],0)

    def test_export_hash_guard_and_separate_schema(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);inputs=root/'inputs';inputs.mkdir();m,e=fixture()
            ep=inputs/'e.json';ep.write_text(json.dumps(e));m['records'][0]['evidence']=dict(path=str(ep),sha256=sha256(ep))
            mp=inputs/'m.json';mp.write_text(json.dumps(m));out=root/'out'
            self.assertEqual(export(mp,out)['accepted'],1)
            saved=json.loads((out/'scoped_units.json').read_text())
            self.assertTrue(saved['requires_scope_aware_trainer']);self.assertFalse(saved['legacy_window_label_conversion_allowed'])
            with self.assertRaises(FileExistsError):export(mp,out)
            ep.write_text(json.dumps(dict(e,reviewer='changed')))
            with self.assertRaisesRegex(ValueError,'checksum'):export(mp,root/'bad')
            self.assertFalse((root/'bad').exists())


if __name__=='__main__':unittest.main()
