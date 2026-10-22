import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
import torch

from tools.drone_ml.common import sha256,stamp
from tools.drone_ml.preprocess import STFTConfig,spectrogram
from tools.drone_ml.temporal_inputs import tiles_from_reader,prepare
from tools.drone_ml.temporal_model import TemporalDroneModel,joint_loss,description


class TemporalModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):torch.set_num_threads(2)

    def test_tcn_does_not_leak_future_and_prefix_matches(self):
        torch.manual_seed(2);model=TemporalDroneModel(3).eval()
        x=torch.randn(2,16,64);altered=x.clone();altered[:,8:]+=20
        with torch.no_grad():
            a=model.forward_features(x);b=model.forward_features(altered)
            prefix=model.forward_features(x[:,:8])
        for key in a:
            torch.testing.assert_close(a[key][:,:8],b[key][:,:8],rtol=0,atol=0)
            torch.testing.assert_close(a[key][:,:8],prefix[key],rtol=1e-5,atol=1e-6)

    def test_background_and_unidentified_drone_do_not_train_identity(self):
        p=torch.randn(4,2,requires_grad=True);i=torch.randn(4,3,requires_grad=True)
        loss=joint_loss(dict(presence_logits=p,identity_logits=i),
                        torch.tensor([0,1,1,-1]),torch.tensor([-1,2,-1,-1]))
        loss.backward()
        self.assertTrue(torch.equal(i.grad[[0,2,3]],torch.zeros(3,3)))
        self.assertGreater(float(i.grad[1].abs().sum()),0)
        self.assertTrue(torch.equal(p.grad[3],torch.zeros(2)))
        with self.assertRaises(ValueError):
            joint_loss(dict(presence_logits=p,identity_logits=i),torch.tensor([0,1,1,-1]),torch.tensor([0,2,-1,-1]))

    def test_forward_backward_geometry_and_unqualified_description(self):
        model=TemporalDroneModel(3)
        out=model(torch.randn(1,2,1,256,625))
        self.assertEqual(out['identity_logits'].shape,(1,3))
        loss=joint_loss(out,torch.tensor([1]),torch.tensor([2]));loss.backward()
        self.assertTrue(torch.isfinite(loss))
        self.assertTrue(all(torch.isfinite(p.grad).all() for p in model.parameters() if p.grad is not None))
        d=description(3);self.assertFalse(d['trained']);self.assertFalse(d['live_qualified'])
        self.assertEqual(d['receptive_field_tiles'],127)

    def test_fixed_view_tiles_equal_existing_preprocessor(self):
        rng=np.random.default_rng(4)
        iq=(rng.normal(size=320000)+1j*rng.normal(size=320000)).astype('<c8')
        calls=[]
        def get(start,n):calls.append((start,n));return iq[start:start+n]
        tiles,offsets,meta=tiles_from_reader(get,len(iq),20000000,0,2,0.)
        self.assertEqual(calls,[(0,160000),(160000,160000)])
        self.assertEqual(offsets,[0,160000]);self.assertFalse(meta['auto_frequency_view'])
        for j in range(2):
            np.testing.assert_array_equal(tiles[j],spectrogram(iq[j*160000:(j+1)*160000],20000000,
                STFTConfig(duration_seconds=.008,hop=256)))

    def test_short_or_out_of_band_input_rejected_before_sample_reads(self):
        def forbidden(*args):raise AssertionError('sample payload must not be opened')
        with self.assertRaisesRegex(ValueError,'short'):
            tiles_from_reader(forbidden,160000,20000000,0,16,0.)
        with self.assertRaisesRegex(ValueError,'guard'):
            tiles_from_reader(forbidden,5000000,30000000,0,16,10000000.)

    def test_preview_preserves_source_and_withholds_labels(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);inputs=root/'inputs';inputs.mkdir()
            iq=np.exp(2j*np.pi*.1*np.arange(320000)).astype('<c8');p=inputs/'signal.cf32';iq.tofile(p)
            row=dict(path=str(p),adapter='cf32_le',source_sha256=sha256(p),sample_rate_hz=20000000,
                partition='development_only',collection_group='fixture',publisher_context='not ground truth')
            m=inputs/'manifest.json';m.write_text(json.dumps(dict(schema='rfmon.drone_ml.temporal_sources.v1',records=[row])))
            before=stamp(p);r=prepare(m,0,root/'preview',count=2)
            self.assertEqual(stamp(p),before)
            self.assertEqual(r['context_seconds'],.016);self.assertFalse(r['training_eligible'])
            self.assertIsNone(r['verified_presence_label'])
            row.update(partition='final_test',path='/sealed/never-open')
            m.write_text(json.dumps(dict(schema='rfmon.drone_ml.temporal_sources.v1',records=[row])))
            with self.assertRaisesRegex(ValueError,'reserved'):prepare(m,0,root/'sealed-preview',count=2)


if __name__=='__main__':unittest.main()
