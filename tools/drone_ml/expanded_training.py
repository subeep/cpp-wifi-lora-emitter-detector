"""Preflight by default. Optimization requires the explicit --start-training flag."""
import argparse
from collections import Counter, defaultdict
import json
import os
import time
from pathlib import Path
import numpy as np
import torch
from torch import nn
from torch.utils.data import Dataset, DataLoader, WeightedRandomSampler
from .common import new_output, sha256, write_json
from .model import SpectrogramCNN
from .residual_model import RFResidualCNN, describe


class ShardedDataset(Dataset):
    def __init__(self,root,manifest,records):
        self.root=Path(root);self.manifest=manifest;self.records=records
        self.classes=manifest['classes'];self.cache={}
    def __len__(self):return len(self.records)
    def __getitem__(self,index):
        row=self.records[index];name=row['shard']
        if name not in self.cache:
            if len(self.cache)>=2:self.cache.pop(next(iter(self.cache)))
            self.cache[name]=np.load(self.root/name,mmap_mode='r',allow_pickle=False)
        image=self.cache[name][row['shard_index']]
        norm=self.manifest['normalization']
        x=((image-norm['mean'])/norm['scale']).astype(np.float32)
        label=self.classes.index(row['label']) if row['label'] in self.classes else -1
        return torch.from_numpy(x[None]),label,index


def audit_manifest(manifest):
    if manifest.get('schema')!='rfmon.drone_ml.sharded_corpus.v1':raise ValueError('unsupported corpus')
    rows=manifest['records'];groups={};hashes={};intervals=set()
    if not rows or [r['index'] for r in rows]!=list(range(len(rows))):raise ValueError('invalid row indexing')
    for r in rows:
        if r['partition'] not in ('train','calibration','unknown_calibration'):raise ValueError('locked partition in development loader')
        if r['augmentation'] is not None and r['partition']!='train':raise ValueError('augmentation leaked outside train')
        value=(r['partition'],r['label'])
        for table,key in ((groups,r['split_group']),(hashes,r['interval_sha256'])):
            if key in table and table[key]!=value:raise ValueError('group/hash leakage across partition or label')
            table[key]=value
        key=(r['recording_id'],r['offset_samples'],r['samples'],r['augmentation'] is not None)
        if key in intervals:raise ValueError('duplicate interval/variant')
        intervals.add(key)
    classes=manifest['classes']
    if classes!=sorted(set(classes)):raise ValueError('class index order invalid')
    if set(classes)!={r['label'] for r in rows if r['partition']=='train' and r['model_eligible']}:
        raise ValueError('class/training eligibility mismatch')
    if set(classes)&{r['label'] for r in rows if r['partition']=='unknown_calibration'}:
        raise ValueError('unknown class leaked into training')
    norm=manifest['normalization']
    if not np.isfinite([norm['mean'],norm['scale']]).all() or norm['scale']<=0:raise ValueError('invalid normalization')
    return True


def training_weights(rows):
    class_groups=defaultdict(set);group_recordings=defaultdict(set);counts=Counter()
    for r in rows:
        class_groups[r['label']].add(r['split_group']);group_recordings[r['split_group']].add(r['recording_id'])
        counts[(r['split_group'],r['recording_id'])]+=1
    return [1/(len(class_groups[r['label']])*len(group_recordings[r['split_group']])*counts[(r['split_group'],r['recording_id'])]) for r in rows]


def preflight(corpus_path,review_path):
    corpus_path,review_path=Path(corpus_path),Path(review_path)
    m=json.loads(corpus_path.read_text());audit_manifest(m)
    review=json.loads(review_path.read_text());split_path=review_path.parent/'split_plan.json'
    if sha256(review_path)!=m['review_sha256'] or sha256(split_path)!=m['split_sha256']:
        raise ValueError('frozen source review/split changed')
    groups=json.loads(split_path.read_text())['groups']
    for r in m['records']:
        if groups[r['split_group']]!=dict(partition=r['partition'],label=r['label']):raise ValueError('source split mismatch')
    shards={s['path']:s for s in m['shards']}
    if len(shards)!=len(m['shards']):raise ValueError('duplicate shard')
    sums=[0.,0.,0]
    rows_by_shard=defaultdict(list)
    for row in m['records']:rows_by_shard[row['shard']].append(row)
    for name,s in shards.items():
        if Path(name).name!=name:raise ValueError('shard path escape')
        p=corpus_path.parent/name
        if p.stat().st_size!=s['bytes'] or p.stat().st_size>32*256*625*4+65536 or sha256(p)!=s['sha256']:
            raise ValueError('shard size/hash mismatch')
        # mmap avoids bulk allocation even if a corrupt NPY header claims a huge tensor.
        a=np.load(p,mmap_mode='r',allow_pickle=False)
        if a.dtype!=np.float32 or list(a.shape)!=s['shape'] or a.shape[1:]!=(256,625) or not 1<=len(a)<=32 or not np.isfinite(a).all():
            raise ValueError('invalid tensor shard')
        for row in rows_by_shard[name]:
            if not 0<=row['shard_index']<len(a):raise ValueError('row/shard mismatch')
            if row['partition']=='train' and row['model_eligible']:
                image=a[row['shard_index']].astype(np.float64)
                sums[0]+=float(image.sum());sums[1]+=float((image**2).sum());sums[2]+=image.size
    if not sums[2]:raise ValueError('no eligible training pixels')
    computed_mean=sums[0]/sums[2]
    computed_scale=np.sqrt(max(0,sums[1]/sums[2]-computed_mean**2))
    if not np.allclose([computed_mean,computed_scale],[m['normalization']['mean'],m['normalization']['scale']],rtol=1e-8,atol=1e-8):
        raise ValueError('normalization not fitted exclusively to eligible training data')
    for r in m['records']:
        if r['shard'] not in shards or not 0<=r['shard_index']<shards[r['shard']]['shape'][0]:raise ValueError('row/shard mismatch')
    used={(r['shard'],r['shard_index']) for r in m['records']}
    if len(used)!=len(m['records']) or len(used)!=sum(s['shape'][0] for s in m['shards']):raise ValueError('tensor assignment mismatch')
    coverage={}
    for label in m['classes']:
        rows=[r for r in m['records'] if r['label']==label and r['augmentation'] is None]
        coverage[label]={p:dict(windows=sum(r['partition']==p for r in rows),
            eligible=sum(r['partition']==p and r['model_eligible'] for r in rows),
            groups=len({r['split_group'] for r in rows if r['partition']==p}),
            recordings=len({r['recording_id'] for r in rows if r['partition']==p})) for p in ('train','calibration')}
    architecture=describe(len(m['classes']))
    eligible=[r for r in m['records'] if r['partition']=='train' and r['model_eligible']]
    batch=next(iter(DataLoader(ShardedDataset(corpus_path.parent,m,eligible),batch_size=2,shuffle=False)))
    model=RFResidualCNN(len(m['classes'])).eval()
    with torch.no_grad():logits,embedding=model(batch[0])
    if not torch.isfinite(logits).all() or not torch.isfinite(embedding).all():raise ValueError('forward preflight failed')
    return dict(schema='rfmon.drone_ml.pretraining_readiness.v1',ready_for_development_training=True,
        training_started=False,optimizer_steps=0,architecture=architecture,coverage=coverage,
        classes=m['classes'],tensors=len(m['records']),training_eligible=len(eligible),
        calibration_eligible=sum(r['partition']=='calibration' and r['model_eligible'] for r in m['records']),
        unknown_calibration_eligible=sum(r['partition']=='unknown_calibration' and r['model_eligible'] for r in m['records']),
        augmented=sum(r['augmentation'] is not None for r in m['records']),
        corpus_sha256=sha256(corpus_path),source_review_sha256=sha256(review_path),
        all_shard_hashes_verified=True,held_test_iq_read=False,release_ready=False,
        cuda_available=torch.cuda.is_available(),
        blockers_for_release=['no_verified_non_drone_training_or_evaluation',
            'most_classes_lack_independent_units_for_calibration_and_test','publisher_context_is_not_window_truth',
            'receiver_and_dataset_shortcuts_require_evaluation','previous_benchmark_test_exposure_not_sealed',
            'live_receiver_and_CPP_export_pending'])


def cross_unit_contrastive(embedding,labels,groups,temperature=.1):
    """Only same-class, different-unit positives. Missing positives contribute zero."""
    similarity=embedding@embedding.T/temperature
    eye=torch.eye(len(labels),device=labels.device,dtype=torch.bool)
    positive=(labels[:,None]==labels[None,:]) & ~eye
    independent=torch.tensor([[a!=b for b in groups] for a in groups],device=labels.device)
    positive &= independent
    valid=positive.any(1)
    if not valid.any():return embedding.sum()*0
    logprob=similarity-torch.logsumexp(similarity.masked_fill(eye,float('-inf')),dim=1,keepdim=True)
    means=(logprob.masked_fill(~positive,0).sum(1)/positive.sum(1).clamp_min(1))
    return -means[valid].mean()


def train(corpus_path,review_path,output,epochs=40,batch_size=8,architecture='residual',device='cpu',resume=False):
    """Explicit opt-in entry point; preflight and fixed epochs, never final-test IQ."""
    if not 1<=epochs<=100 or not 2<=batch_size<=64:raise ValueError('invalid training configuration')
    readiness=preflight(corpus_path,review_path)
    m=json.loads(Path(corpus_path).read_text())
    out=Path(output).resolve() if resume else new_output(output,[Path(corpus_path).parent,Path(review_path).parent,Path('/media/sudeep/Storage/datasets')])
    corpus_hash=sha256(corpus_path)
    configuration=dict(epochs=epochs,batch_size=batch_size,architecture=architecture,device=device,seed=17,corpus_sha256=corpus_hash)
    if resume and json.loads((out/'run_config.json').read_text())!=configuration:
        raise ValueError('resume configuration/corpus mismatch')
    if not resume:write_json(out/'run_config.json',configuration)
    def atomic_json(name,data):
        temporary=out/(name+'.tmp');write_json(temporary,data);temporary.replace(out/name)
    def atomic_save(name,data):
        temporary=out/(name+'.tmp');torch.save(data,temporary);temporary.replace(out/name)
    torch.manual_seed(17);np.random.seed(17);torch.set_num_threads(2);torch.use_deterministic_algorithms(True)
    rows=[r for r in m['records'] if r['partition']=='train' and r['model_eligible']]
    dataset=ShardedDataset(Path(corpus_path).parent,m,rows)
    sampler=WeightedRandomSampler(training_weights(rows),len(rows),replacement=True,generator=torch.Generator().manual_seed(17))
    loader=DataLoader(dataset,batch_size=batch_size,sampler=sampler,num_workers=0)
    model=(RFResidualCNN(len(m['classes'])) if architecture=='residual' else SpectrogramCNN(len(m['classes']))).to(device)
    optimizer=torch.optim.AdamW(model.parameters(),lr=3e-4,weight_decay=.01)
    scheduler=torch.optim.lr_scheduler.CosineAnnealingLR(optimizer,T_max=epochs,eta_min=1e-5)
    history=[];start_epoch=0;steps=0;started=time.monotonic()
    if resume:
        state=torch.load(out/'training_state.pt',map_location=device,weights_only=True)
        model.load_state_dict(state['model']);optimizer.load_state_dict(state['optimizer']);scheduler.load_state_dict(state['scheduler'])
        torch.set_rng_state(state['torch_rng'].cpu());sampler.generator.set_state(state['sampler_rng'].cpu())
        history=state['history'];start_epoch=state['completed_epochs'];steps=state['optimizer_steps']
        if device=='cuda':torch.cuda.set_rng_state_all(state['cuda_rng'])
    atomic_json('progress.json',dict(status='training',pid=os.getpid(),architecture=architecture,completed_epochs=start_epoch,
        total_epochs=epochs,optimizer_steps=steps,total_batches_per_epoch=len(loader)))
    for epoch in range(start_epoch,epochs):
        model.train();total=0.
        for batch_index,(x,y,indices) in enumerate(loader,1):
            optimizer.zero_grad();logits,emb=model(x.to(device));y=y.to(device)
            loss=nn.functional.cross_entropy(logits,y,label_smoothing=.05)
            loss=loss+.1*cross_unit_contrastive(emb,y,[rows[int(i)]['split_group'] for i in indices])
            if not torch.isfinite(loss):raise ValueError('nonfinite training loss')
            loss.backward();nn.utils.clip_grad_norm_(model.parameters(),5,error_if_nonfinite=True);optimizer.step()
            total+=float(loss.detach())*len(y);steps+=1
            if batch_index==1 or batch_index%25==0:
                atomic_json('progress.json',dict(status='training',pid=os.getpid(),architecture=architecture,
                    epoch=epoch+1,completed_epochs=epoch,total_epochs=epochs,batch=batch_index,total_batches_per_epoch=len(loader),
                    optimizer_steps=steps,elapsed_this_process_seconds=time.monotonic()-started,
                    running_training_loss=total/min(batch_index*batch_size,len(rows))))
        scheduler.step();history.append(dict(epoch=epoch+1,loss=total/len(rows)));print(json.dumps(history[-1]),flush=True)
        atomic_save('training_state.pt',dict(model=model.state_dict(),optimizer=optimizer.state_dict(),scheduler=scheduler.state_dict(),
            torch_rng=torch.get_rng_state(),sampler_rng=sampler.generator.get_state(),
            cuda_rng=torch.cuda.get_rng_state_all() if device=='cuda' else [],history=history,
            completed_epochs=epoch+1,optimizer_steps=steps))
        atomic_json('progress.json',dict(status='training',pid=os.getpid(),architecture=architecture,
            completed_epochs=epoch+1,total_epochs=epochs,optimizer_steps=steps,history=history,
            elapsed_this_process_seconds=time.monotonic()-started))
    # This broader development checkpoint cannot inherit the baseline's acceptance policy.
    checkpoint=dict(schema='rfmon.drone_ml.expanded_checkpoint.v1',architecture=architecture,
        state_dict={k:v.cpu() for k,v in model.state_dict().items()},classes=m['classes'],
        preprocessing=m['preprocessing'],normalization=m['normalization'],activity_policy=m['activity_policy'],
        policy=None,policy_blockers=['expanded_model_requires_fresh_calibration_and_independent_evaluation'],
        development_only=True,model_validated_for_live_use=False)
    atomic_save('model.pt',checkpoint)
    write_json(out/'training_report.json',dict(history=history,architecture=architecture,readiness=readiness,
        model_sha256=sha256(out/'model.pt'),corpus_sha256=corpus_hash,optimizer_steps=steps,release_ready=False))
    atomic_json('progress.json',dict(status='completed',completed_epochs=epochs,total_epochs=epochs,optimizer_steps=steps,model=str(out/'model.pt')))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus',type=Path,required=True);p.add_argument('--review',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--start-training',action='store_true')
    p.add_argument('--resume',action='store_true')
    p.add_argument('--epochs',type=int,default=40);p.add_argument('--batch-size',type=int,default=8)
    p.add_argument('--architecture',choices=('residual','baseline'),default='residual')
    p.add_argument('--device',choices=('cpu','cuda'),default='cpu')
    a=p.parse_args()
    if a.resume and not a.start_training:p.error('--resume requires --start-training')
    if a.start_training:train(a.corpus,a.review,a.output,a.epochs,a.batch_size,a.architecture,a.device,a.resume)
    else:
        out=new_output(a.output,[a.corpus.parent,a.review.parent,Path('/media/sudeep/Storage/datasets')])
        r=preflight(a.corpus,a.review);write_json(out/'readiness.json',r)
        print(json.dumps({k:r[k] for k in ('training_started','optimizer_steps','tensors','training_eligible')}))
if __name__=='__main__':main()
