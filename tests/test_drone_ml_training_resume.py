"""Interrupted training must resume to the same CPU weights on a synthetic fixture."""
import json
from pathlib import Path
import tempfile
import shutil
import unittest
from unittest.mock import patch
import numpy as np
import torch
from tools.drone_ml.expanded_training import train, resume_configuration


class ResumeTests(unittest.TestCase):
    def test_fixed_pool_matches_adaptive_outputs_and_gradients(self):
        x=torch.randn(2,192,16,40,requires_grad=True)
        adaptive=torch.nn.functional.adaptive_avg_pool2d(x,(2,4))
        fixed=torch.nn.functional.avg_pool2d(x,(8,10),(8,10))
        torch.testing.assert_close(adaptive,fixed)
        upstream=torch.randn_like(adaptive)
        a=torch.autograd.grad(adaptive,x,upstream,retain_graph=True)[0]
        b=torch.autograd.grad(fixed,x,upstream)[0]
        torch.testing.assert_close(a,b)

    def test_device_migration_cannot_change_other_configuration(self):
        previous=dict(device='cpu',epochs=40,batch_size=8,corpus_sha256='fixed',architecture='residual',seed=17)
        current=dict(previous,device='cuda')
        with self.assertRaisesRegex(ValueError,'mismatch'):resume_configuration(previous,current)
        resume_configuration(previous,current,True)
        for key,value in [('epochs',41),('batch_size',16),('corpus_sha256','changed'),('architecture','baseline'),('seed',18)]:
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'mismatch'):
                resume_configuration(previous,dict(current,**{key:value}),True)

    @unittest.skipUnless(torch.cuda.is_available(),'CUDA runtime required')
    def test_cpu_migration_then_cuda_resume_is_reproducible(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);data=root/'data';data.mkdir()
            np.save(data/'s.npy',np.random.default_rng(3).normal(size=(4,256,625)).astype(np.float32))
            rows=[dict(partition='train',model_eligible=True,label=['a','b'][i%2],
                split_group='group'+str(i),recording_id=str(i),shard='s.npy',shard_index=i) for i in range(4)]
            corpus=data/'corpus.json';corpus.write_text(json.dumps(dict(records=rows,classes=['a','b'],
                normalization=dict(mean=0,scale=1),preprocessing={},activity_policy=None)))
            original=torch.optim.AdamW.step;count=[0]
            def interrupted(optimizer,*args,**kwargs):
                count[0]+=1
                if count[0]==3:raise RuntimeError('simulated interruption')
                return original(optimizer,*args,**kwargs)
            with patch('tools.drone_ml.expanded_training.preflight',return_value={'synthetic_fixture':True}):
                with patch.object(torch.optim.AdamW,'step',interrupted):
                    with self.assertRaisesRegex(RuntimeError,'simulated interruption'):
                        train(corpus,data/'review.json',root/'cpu',epochs=3,batch_size=2)
                for name in ('continuous','resumed'):shutil.copytree(root/'cpu',root/name)
                train(corpus,data/'review.json',root/'continuous',epochs=3,batch_size=2,
                      device='cuda',resume=True,allow_device_change=True)
                count[0]=0
                with patch.object(torch.optim.AdamW,'step',interrupted):
                    with self.assertRaisesRegex(RuntimeError,'simulated interruption'):
                        train(corpus,data/'review.json',root/'resumed',epochs=3,batch_size=2,
                              device='cuda',resume=True,allow_device_change=True)
                train(corpus,data/'review.json',root/'resumed',epochs=3,batch_size=2,
                      device='cuda',resume=True,allow_device_change=True)
            a=torch.load(root/'continuous/model.pt',map_location='cpu',weights_only=True)
            b=torch.load(root/'resumed/model.pt',map_location='cpu',weights_only=True)
            self.assertTrue(all(torch.equal(v,b['state_dict'][k]) for k,v in a['state_dict'].items()))
            transitions=json.loads((root/'resumed/device_transitions.json').read_text())
            self.assertEqual(len(transitions),1)
            self.assertEqual(transitions[0]['from_device'],'cpu')
            self.assertEqual(transitions[0]['completed_epochs'],1)

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
