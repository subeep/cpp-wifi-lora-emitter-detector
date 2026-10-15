"""Interrupted training must resume to the same CPU weights on a synthetic fixture."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import numpy as np
import torch
from tools.drone_ml.expanded_training import train


class ResumeTests(unittest.TestCase):
    def test_epoch_resume_matches_uninterrupted_training(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);data=root/'data';data.mkdir()
            rng=np.random.default_rng(3)
            np.save(data/'s.npy',rng.normal(size=(4,256,625)).astype(np.float32))
            rows=[dict(partition='train',model_eligible=True,label=['a','b'][i%2],
                split_group='group'+str(i),recording_id=str(i),shard='s.npy',shard_index=i) for i in range(4)]
            corpus=data/'corpus.json';corpus.write_text(json.dumps(dict(records=rows,classes=['a','b'],
                normalization=dict(mean=0,scale=1),preprocessing={},activity_policy=None)))
            with patch('tools.drone_ml.expanded_training.preflight',return_value={'synthetic_fixture':True}):
                train(corpus,data/'review.json',root/'continuous',epochs=2,batch_size=2)
                original=torch.optim.AdamW.step;count=[0]
                def interrupted(optimizer,*args,**kwargs):
                    count[0]+=1
                    if count[0]==3:raise RuntimeError('simulated interruption')
                    return original(optimizer,*args,**kwargs)
                with patch.object(torch.optim.AdamW,'step',interrupted):
                    with self.assertRaisesRegex(RuntimeError,'simulated interruption'):
                        train(corpus,data/'review.json',root/'resumed',epochs=2,batch_size=2)
                saved=torch.load(root/'resumed/training_state.pt',weights_only=True)
                self.assertEqual(saved['completed_epochs'],1)
                train(corpus,data/'review.json',root/'resumed',epochs=2,batch_size=2,resume=True)
            a=torch.load(root/'continuous/model.pt',weights_only=True)
            b=torch.load(root/'resumed/model.pt',weights_only=True)
            self.assertTrue(all(torch.equal(v,b['state_dict'][k]) for k,v in a['state_dict'].items()))
            progress=json.loads((root/'resumed/progress.json').read_text())
            self.assertEqual(progress['optimizer_steps'],4)
            self.assertEqual(progress['status'],'completed')

if __name__=='__main__':unittest.main()
