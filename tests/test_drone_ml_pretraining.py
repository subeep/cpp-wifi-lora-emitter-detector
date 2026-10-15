"""Preparation and architecture tests only: no training loops or optimizer steps."""
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
import h5py
import numpy as np
import torch
from tools.drone_ml.corpus_sources import drff_metadata, unit_partition, xml_metadata
from tools.drone_ml.expanded_corpus import augment_iq, window_starts
from tools.drone_ml.expanded_training import audit_manifest, training_weights, cross_unit_contrastive
from tools.drone_ml.residual_model import RFResidualCNN, describe


def fixture():
    rows=[]
    for k,(label,part,group) in enumerate([('a','train','a1'),('b','train','b1'),('a','calibration','a2'),('u','unknown_calibration','u1')]):
        rows.append(dict(index=k,label=label,partition=part,split_group=group,interval_sha256=str(k),
            recording_id=group,offset_samples=0,samples=160000,augmentation=None,model_eligible=True))
    return dict(schema='rfmon.drone_ml.sharded_corpus.v1',records=rows,classes=['a','b'],normalization=dict(mean=0,scale=1))


class PretrainingTests(unittest.TestCase):
    def test_architecture_forward_only_no_optimizer(self):
        with patch('torch.optim.AdamW',side_effect=AssertionError('training forbidden')):
            result=describe(21)
        self.assertEqual(result['parameters'],2137952)
        self.assertEqual(result['logits_shape'],[1,21])
        self.assertEqual(result['optimizer_steps'],0)
        self.assertTrue(result['weights_unchanged'])
        self.assertEqual(result['embedding_dimensions'],128)

    def test_model_requires_physical_shape(self):
        with self.assertRaises(ValueError):RFResidualCNN(2)(torch.zeros(1,1,224,224))

    def test_augmentation_reproducible_preserves_original(self):
        x=np.exp(2j*np.pi*.03*np.arange(20000));before=x.copy()
        a,params=augment_iq(x,20e6,123,'train');b,again=augment_iq(x,20e6,123,'train')
        np.testing.assert_array_equal(x,before);np.testing.assert_array_equal(a,b)
        self.assertEqual(params,again);self.assertIsNone(params['ground_truth_snr'])
        self.assertLessEqual(abs(params['cfo_hz']),100000)
        self.assertTrue(np.isfinite(a).all());self.assertFalse(np.array_equal(a,x))

    def test_short_recordings_reduce_count_instead_of_overlap(self):
        starts=window_starts(10_000_000,800_000,12)
        self.assertEqual(len(starts),10)
        self.assertGreaterEqual(min(np.diff(starts)),800000)
        self.assertGreaterEqual(starts[0],800000)
        self.assertLessEqual(starts[-1]+800000,10_000_000-800000)
        with self.assertRaises(ValueError):window_starts(100,800000,12)

    def test_no_augmentation_on_calibration_or_test(self):
        for part in ('calibration','unknown_calibration','test_locked','unknown_test_locked'):
            with self.assertRaises(ValueError):augment_iq(np.ones(20000),20e6,0,part)

    def test_partition_rules_preserve_prior_holdouts(self):
        self.assertEqual(unit_partition('mini3pro',1),'unknown_calibration')
        self.assertEqual(unit_partition('mavic3S',1),'unknown_test_locked')
        self.assertEqual(unit_partition('mavic3C',1),'unknown_test_locked')
        for model in ('mavicAir2','mavicAir2s','mini4PRO'):
            self.assertEqual(unit_partition(model,5),'test_locked')
            self.assertEqual(unit_partition(model,4),'calibration')
            self.assertEqual(unit_partition(model,2),'train')

    def test_manifest_valid_then_group_leak_detected(self):
        m=fixture();self.assertTrue(audit_manifest(m))
        m['records'][2]['split_group']='a1'
        with self.assertRaises(ValueError):audit_manifest(m)

    def test_identical_iq_cannot_cross_partitions(self):
        m=fixture();m['records'][2]['interval_sha256']='0'
        with self.assertRaises(ValueError):audit_manifest(m)

    def test_final_partition_cannot_enter_loader(self):
        m=fixture();m['records'][2]['partition']='test_locked'
        with self.assertRaises(ValueError):audit_manifest(m)

    def test_unknown_class_cannot_be_training_class(self):
        m=fixture();m['records'][-1]['label']='a'
        with self.assertRaises(ValueError):audit_manifest(m)

    def test_augmented_calibration_rejected(self):
        m=fixture();m['records'][2]['augmentation']={'seed':1}
        with self.assertRaises(ValueError):audit_manifest(m)

    def test_balancing_does_not_reward_long_recordings(self):
        rows=[dict(label='a',split_group='a1',recording_id='long')]*10+[
            dict(label='a',split_group='a1',recording_id='short'),dict(label='b',split_group='b1',recording_id='b')]
        weights=training_weights(rows)
        self.assertAlmostEqual(sum(weights[:10]),weights[10])
        self.assertAlmostEqual(sum(weights[:11]),weights[11])

    def test_contrastive_requires_different_aircraft_groups(self):
        emb=torch.nn.functional.normalize(torch.tensor([[1.,2.],[2.,1.],[1.,1.]]),dim=1)
        labels=torch.tensor([0,0,1])
        self.assertEqual(float(cross_unit_contrastive(emb,labels,['a','a','b'])),0)
        self.assertTrue(torch.isfinite(cross_unit_contrastive(emb,labels,['a','c','b'])))

    def test_publisher_character_condition_is_parsed(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'record.mat'
            with h5py.File(path,'w') as h:
                for key,value in dict(TD='mavic3_1',State='Ascend',U='u1',D='d2',C='c17').items():
                    d=h.create_dataset(key,data=np.array([ord(c) for c in value],dtype=np.uint16)[:,None]);d.attrs['MATLAB_class']=np.bytes_('char')
                h['Fs']=np.array([[100e6]]);h['CenterFrequence']=np.array([[5745e6]])
                h['RF0_I']=np.zeros((128,1),np.float32);h['RF0_Q']=np.ones((128,1),np.float32)
            self.assertEqual(drff_metadata(path)['condition'],17)
            with h5py.File(path,'a') as h:
                del h['C'];h['C']=np.array([[1.5]])
            with self.assertRaises(ValueError):drff_metadata(path)

    def test_residual_checkpoint_loads_in_separate_engine_without_training(self):
        from tools.drone_ml.predict import InferenceEngine
        from tools.drone_ml.expanded_corpus import CONFIG
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'untrained_fixture.pt'
            model=RFResidualCNN(2).eval()
            torch.save(dict(schema='rfmon.drone_ml.expanded_checkpoint.v1',architecture='residual',
                classes=['synthetic_a','synthetic_b'],state_dict=model.state_dict(),preprocessing=CONFIG.metadata(),
                normalization=dict(mean=-10.,scale=10.),policy=None,policy_blockers=['untrained_fixture']),path)
            engine=InferenceEngine(path)
            iq=np.exp(2j*np.pi*.1*np.arange(160000))
            result=engine.predict_iq(iq,20000000)
            self.assertEqual(result['status'],'identification_unavailable')
            self.assertIsNone(result['accepted_class'])
            self.assertFalse(result['model_validated_for_live_use'])

    def test_xml_entities_rejected(self):
        with self.assertRaises(ValueError):xml_metadata(b'<!DOCTYPE test><SignalHoundIQFile/>')

if __name__=='__main__':unittest.main()
