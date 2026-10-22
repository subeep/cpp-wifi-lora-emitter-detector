"""Independent binary presence experiment; never installs a live policy.

Uses reviewed train/calibration/development groups only. Reports window metrics,
not encounter recall or false alerts/hour. Final tests stay sealed.
"""
import argparse
from collections import Counter, defaultdict
from pathlib import Path
import random

import numpy as np
import torch
from torch.utils.data import Dataset, DataLoader, WeightedRandomSampler

from .common import new_output, read_source, sha256
from .input_audit import read_json
from .live_audit import write_new_json
from .model import SpectrogramCNN
from .presence_dataset import CONFIG, LABELS, PARTITIONS, readiness


def image_at(root,row):
    path=(root/row['tensor']).resolve()
    if path.parent!=root or path.suffix!='.npy':raise ValueError('invalid tensor path')
    if path.stat().st_size>700000:raise ValueError('oversized tensor')
    with read_source(path) as f:image=np.load(f,allow_pickle=False)
    if image.shape!=(256,625) or image.dtype!=np.float32 or not np.isfinite(image).all():
        raise ValueError('invalid tensor shape, dtype or values')
    return image


def normalization(root,rows):
    count=0;total=0.;squares=0.
    for row in rows:
        v=image_at(root,row).astype(np.float64)
        count+=v.size;total+=v.sum();squares+=np.square(v).sum()
    mean=float(total/count);scale=float(np.sqrt(max(0.,squares/count-mean*mean)))
    if not np.isfinite([mean,scale]).all() or scale<1e-6:raise ValueError('invalid training normalization')
    return dict(mean=mean,scale=scale)


class Images(Dataset):
    def __init__(self,root,rows,norm):self.root=root;self.rows=rows;self.norm=norm
    def __len__(self):return len(self.rows)
    def __getitem__(self,index):
        row=self.rows[index];image=image_at(self.root,row)
        return torch.from_numpy(((image-self.norm['mean'])/self.norm['scale'])[None]),LABELS.index(row['label'])


def metrics(scores,rows,threshold):
    labels=np.array([r['label']=='drone' for r in rows]);accepted=np.asarray(scores)>=threshold
    groups=defaultdict(list)
    for i,row in enumerate(rows):groups[row['collection_group']].append(i)
    by_group={}
    for g,ids in groups.items():
        y=labels[ids];a=accepted[ids]
        by_group[g]=dict(windows=len(ids),drone_recall=float(a[y].mean()) if y.any() else None,
                         background_acceptance=float(a[~y].mean()) if (~y).any() else None)
    balanced={key:float(np.mean([m[key] for m in by_group.values() if m[key] is not None]))
              for key in ('drone_recall','background_acceptance')}
    return dict(windows=len(rows),threshold=float(threshold),
                drone_recall=float(accepted[labels].mean()),background_acceptance=float(accepted[~labels].mean()),
                group_balanced=balanced,by_group=by_group,
                by_drone_model={name:dict(windows=sum(r.get('drone_model')==name for r in rows),
                    recall=float(np.mean([accepted[i] for i,r in enumerate(rows) if r.get('drone_model')==name])))
                    for name in {r.get('drone_model') for r in rows if r['label']=='drone'}},
                interpretation='window decisions on reviewed views; not false alerts/hour or encounter recall')


def fit_threshold(scores,rows,max_background_acceptance,min_drone_recall):
    options=np.unique(np.r_[0.,np.quantile(scores,np.linspace(0,1,201)),np.nextafter(max(scores),np.inf)])
    feasible=[]
    for threshold in options:
        m=metrics(scores,rows,threshold);b=m['group_balanced']
        if b['background_acceptance']<=max_background_acceptance and b['drone_recall']>=min_drone_recall:
            feasible.append(m)
    if not feasible:return None
    return max(feasible,key=lambda m:(m['group_balanced']['drone_recall'],-m['group_balanced']['background_acceptance'],m['threshold']))


def train(corpus_path,output,epochs=10,batch_size=16,device='auto',seed=17,
          max_background_acceptance=.01,min_drone_recall=.8):
    if not 1<=epochs<=100 or not 1<=batch_size<=64:raise ValueError('invalid training bounds')
    if not 0<=max_background_acceptance<1 or not 0<min_drone_recall<=1:raise ValueError('invalid diagnostic targets')
    if device not in ('auto','cpu','cuda'):raise ValueError('invalid device')
    corpus_path=Path(corpus_path).resolve();root=corpus_path.parent;corpus=read_json(corpus_path)
    if corpus.get('schema')!='rfmon.drone_ml.presence_corpus.v1' or corpus.get('preprocessing')!=CONFIG.metadata():
        raise ValueError('incompatible presence corpus')
    rows=corpus['records']
    status=readiness(dict(schema='rfmon.drone_ml.presence_review.v1',records=rows))
    if not status['ready'] or any(r['partition'] not in PARTITIONS for r in rows):
        raise ValueError('requires reviewed disjoint development-only corpus')
    partitions={p:[r for r in rows if r['partition']==p] for p in PARTITIONS}
    for row in rows:
        image_at(root,row) # bounded format validation before optimizer setup
        if sha256(root/row['tensor'])!=row['tensor_sha256']:raise ValueError('tensor hash mismatch')
    before=sha256(corpus_path)
    norm=normalization(root,partitions['train'])
    device=('cuda' if torch.cuda.is_available() else 'cpu') if device=='auto' else device
    if device=='cuda' and not torch.cuda.is_available():raise ValueError('CUDA unavailable in this Python runtime')
    out=new_output(output,[root])
    random.seed(seed);np.random.seed(seed);torch.manual_seed(seed);torch.set_num_threads(2)
    model=SpectrogramCNN(2).to(device)
    optimizer=torch.optim.AdamW(model.parameters(),lr=.001,weight_decay=.001)
    train_rows=partitions['train'];counts=Counter((r['label'],r['collection_group']) for r in train_rows)
    groups={label:{r['collection_group'] for r in train_rows if r['label']==label} for label in LABELS}
    weights=[1/(len(groups[r['label']])*counts[r['label'],r['collection_group']]) for r in train_rows]
    sampler=WeightedRandomSampler(weights,len(weights),replacement=True,generator=torch.Generator().manual_seed(seed))
    loader=DataLoader(Images(root,train_rows,norm),batch_size=batch_size,sampler=sampler,num_workers=0)
    history=[]
    for epoch in range(epochs):
        model.train();loss_sum=0.;seen=0
        for x,y in loader:
            x=x.to(device);y=y.to(device);optimizer.zero_grad()
            logits,_=model(x);loss=torch.nn.functional.cross_entropy(logits,y)
            if not torch.isfinite(loss):raise ValueError('nonfinite loss')
            loss.backward();optimizer.step();loss_sum+=float(loss.detach())*len(x);seen+=len(x)
        history.append(dict(epoch=epoch+1,loss=loss_sum/seen))
        print(f"presence epoch {epoch+1}/{epochs}: loss={loss_sum/seen:.6f}",flush=True)
    model.eval()
    def predict(partition):
        values=[]
        with torch.no_grad():
            for x,_ in DataLoader(Images(root,partitions[partition],norm),batch_size=batch_size,num_workers=0):
                logits,_=model(x.to(device));values.extend(logits.softmax(1)[:,1].cpu().tolist())
        return np.asarray(values)
    calibration=predict('calibration');development=predict('development')
    candidate=fit_threshold(calibration,partitions['calibration'],max_background_acceptance,min_drone_recall)
    if sha256(corpus_path)!=before or any(sha256(root/r['tensor'])!=r['tensor_sha256'] for r in rows):
        raise ValueError('corpus changed during experiment')
    state={k:v.detach().cpu() for k,v in model.state_dict().items()}
    if any(not torch.isfinite(v).all() for v in state.values()):raise ValueError('nonfinite weights')
    torch.save(dict(schema='rfmon.drone_ml.presence_checkpoint.v1',classes=list(LABELS),
                    architecture='compact_presence',state_dict=state,preprocessing=CONFIG.metadata(),
                    normalization=norm,live_qualified=False,policy=None,corpus_sha256=before),out/'model.pt')
    report=dict(schema='rfmon.drone_ml.presence_training.v1',model_sha256=sha256(out/'model.pt'),
        corpus_sha256=before,seed=seed,device=device,torch_version=str(torch.__version__),history=history,
        normalization=norm,parameters=sum(p.numel() for p in model.parameters()),
        diagnostic_targets=dict(max_group_background_acceptance=max_background_acceptance,min_group_drone_recall=min_drone_recall),
        diagnostic_threshold_fit=candidate,
        development_at_fitted_threshold=(metrics(development,partitions['development'],candidate['threshold']) if candidate else None),
        development_reference_curve=[metrics(development,partitions['development'],t) for t in (.5,.8,.9,.95,.99)],
        live_qualified=False,policy=None,final_test_iq_read=False,
        limitations=['No live promotion or calibrated presence probability.','Thresholds fit on calibration only; development is diagnostic.',
                     'Sparse windows cannot measure false alerts/hour or encounter misses.',
                     'Unknown-drone generalization, independent final tests and live qualification remain required.'])
    write_new_json(out/'report.json',report)
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--epochs',type=int,default=10);p.add_argument('--batch-size',type=int,default=16)
    p.add_argument('--device',choices=('auto','cpu','cuda'),default='auto');p.add_argument('--seed',type=int,default=17)
    p.add_argument('--max-background-acceptance',type=float,default=.01);p.add_argument('--min-drone-recall',type=float,default=.8)
    a=p.parse_args();train(a.corpus,a.output,a.epochs,a.batch_size,a.device,a.seed,a.max_background_acceptance,a.min_drone_recall)


if __name__=='__main__':main()
