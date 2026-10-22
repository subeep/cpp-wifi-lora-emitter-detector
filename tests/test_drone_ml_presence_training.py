"""Synthetic pipeline and leakage tests, never evidence of RF accuracy."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
import torch

from tools.drone_ml.common import sha256, stamp
from tools.drone_ml.input_audit import read_json
from tools.drone_ml.live_audit import AuditRecorder
from tools.drone_ml.presence_dataset import CONFIG, template, readiness, prepare
from tools.drone_ml.presence_train import train, fit_threshold, normalization
from tools.drone_ml.predict import InferenceEngine


def fixture(root):
    sessions=root/'sessions';sessions.mkdir()
    groups={}
    for part in ('train','calibration','development'):
        for unit in range(2):
            group=f'{part}:{unit}'
            recorder=AuditRecorder(str(sessions),'unlabelled','synthetic fixture, not RF evidence','fixture',CONFIG.metadata())
            groups[recorder.path.name]=(part,group)
            for label in range(2):
                rng=np.random.default_rng(len(groups)*10+label)
                iq=(rng.normal(size=160000)+1j*rng.normal(size=160000)).astype('<c8')
                if label:iq+=4*np.exp(2j*np.pi*.15*np.arange(160000)).astype('<c8')
                recorder.record(iq,dict(run_id=group,source='live',contiguous=True,sample_rate_hz=20000000),
                                dict(selected_offset_hz=0,status='identification_unavailable'))
    review=template(sessions)
    for row in review['records']:
        part,group=groups[row['recorder_session']]
        drone=Path(row['metadata']).stem=='0001'
        row.update(partition=part,collection_group=group,label='drone' if drone else 'background',
                   reviewed=True,label_evidence='synthetic fixture only',independence_evidence='synthetic independent generator seeds',
                   drone_model='synthetic' if drone else None,verified_view_offset_hz=0.)
    return review


class PresenceTrainingTests(unittest.TestCase):
    def test_no_data_or_context_alone_does_not_authorize_training(self):
        with tempfile.TemporaryDirectory() as tmp:
            review=template(Path(tmp)/'absent')
            self.assertFalse(readiness(review)['ready'])
            full=fixture(Path(tmp))
            for row in full['records']:row['reviewed']=False
            self.assertFalse(readiness(full)['ready'])

    def test_group_hash_and_source_run_leakage_blocked(self):
        with tempfile.TemporaryDirectory() as tmp:
            review=fixture(Path(tmp));self.assertTrue(readiness(review)['ready'])
            a=next(i for i,r in enumerate(review['records']) if r['partition']=='train')
            b=next(i for i,r in enumerate(review['records']) if r['partition']=='development')
            for field in ('collection_group','recorder_session','source_run','iq_sha256'):
                changed=copy.deepcopy(review);changed['records'][b][field]=changed['records'][a][field]
                self.assertFalse(readiness(changed)['ready'],field)
            changed=copy.deepcopy(review)
            changed['records'][a]['collection_group']='invented-independent-group'
            self.assertFalse(readiness(changed)['ready'])

    def test_prepare_excludes_reserved_iq_and_training_stays_offline(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);review=fixture(root)
            sealed=dict(review['records'][0],partition='final_test',metadata=str(root/'sealed'/'never-read.json'),
                metadata_sha256='sealed',iq_sha256='sealed',recorder_session='sealed',source_run='sealed',collection_group='sealed')
            review['records'].append(sealed)
            path=root/'review.json';path.write_text(json.dumps(review))
            originals={r['metadata']:stamp(r['metadata']) for r in review['records'][:-1]}
            corpus=prepare(path,root/'prepared')
            self.assertEqual(len(corpus['records']),12)
            self.assertFalse(corpus['final_test_iq_read'])
            for p,before in originals.items():self.assertEqual(stamp(p),before)
            report=train(root/'prepared'/'corpus.json',root/'model',epochs=1,batch_size=4,device='cpu')
            self.assertFalse(report['live_qualified']);self.assertIsNone(report['policy'])
            checkpoint=torch.load(root/'model'/'model.pt',weights_only=True)
            self.assertEqual(checkpoint['classes'],['background','drone'])
            self.assertTrue(all(torch.isfinite(t).all() for t in checkpoint['state_dict'].values()))
            with self.assertRaisesRegex(ValueError,'unsupported checkpoint'):InferenceEngine(root/'model'/'model.pt')
            # Altered tensor blocks a new experiment before output is created.
            tensor=root/'prepared'/corpus['records'][0]['tensor']
            image=np.zeros((256,625),dtype=np.float32)
            with tensor.open('wb') as f:np.save(f,image)
            with self.assertRaisesRegex(ValueError,'hash mismatch'):
                train(root/'prepared'/'corpus.json',root/'bad-model',epochs=1)
            self.assertFalse((root/'bad-model').exists())

    def test_wrong_view_cannot_inherit_label(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);review=fixture(root);review['records'][0]['verified_view_offset_hz']=1e6
            path=root/'review.json';path.write_text(json.dumps(review))
            with self.assertRaisesRegex(ValueError,'differs from reviewed'):prepare(path,root/'prepared')

    def test_no_threshold_when_background_and_drone_indistinguishable(self):
        rows=[dict(label=label,collection_group=f'g{i}',drone_model='synthetic' if label=='drone' else None)
              for i,label in enumerate(('background','drone','background','drone'))]
        self.assertIsNone(fit_threshold(np.array([.99,.99,.99,.99]),rows,.01,.8))
        fitted=fit_threshold(np.array([.1,.9,.2,.8]),rows,.01,.8)
        self.assertEqual(fitted['group_balanced']['background_acceptance'],0)
        self.assertEqual(fitted['group_balanced']['drone_recall'],1)


if __name__=='__main__':unittest.main()
