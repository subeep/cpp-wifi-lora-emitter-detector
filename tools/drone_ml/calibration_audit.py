"""Group-separated development calibration; diagnostic output never activates a policy."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import numpy as np
from scipy.optimize import minimize_scalar
from scipy.special import logsumexp, softmax
import torch
from torch.utils.data import DataLoader
from .predict import InferenceEngine
from .expanded_training import ShardedDataset
from .common import write_json,sha256


def group_fold(rows):
    groups=defaultdict(set)
    for r in rows:groups[r['label']].add(r['split_group'])
    if any(len(g)<2 for g in groups.values()):
        raise ValueError('requires at least two independent groups for each development label')
    fit={sorted(g)[0] for g in groups.values()}
    return np.array([r['split_group'] in fit for r in rows])


def audit(model,corpus_path,output):
    engine=InferenceEngine(model);m=json.loads(Path(corpus_path).read_text());classes=m['classes']
    if classes!=engine.checkpoint['classes']:raise ValueError('class mismatch')
    if m['normalization']!=engine.checkpoint['normalization'] or m['preprocessing']!=engine.checkpoint['preprocessing']:
        raise ValueError('checkpoint/corpus preprocessing mismatch')
    arrays={}
    for partition in ('train','calibration','unknown_calibration'):
        rows=[r for r in m['records'] if r['partition']==partition and r['model_eligible'] and r['augmentation'] is None]
        logits=[];embeddings=[]
        with torch.no_grad():
            for x,_,_ in DataLoader(ShardedDataset(Path(corpus_path).parent,m,rows),batch_size=8):
                z,e=engine.model(x);logits.append(z.numpy());embeddings.append(e.numpy())
        arrays[partition]=(rows,np.concatenate(logits),np.concatenate(embeddings))
    rows,_,emb=arrays['train'];prototypes=[]
    for label in classes:
        means=[]
        for group in sorted({r['split_group'] for r in rows if r['label']==label}):
            ix=[i for i,r in enumerate(rows) if r['label']==label and r['split_group']==group]
            means.append(emb[ix].mean(0))
        vector=np.mean(means,axis=0);prototypes.append(vector/max(1e-12,np.linalg.norm(vector)))
    prototypes=np.asarray(prototypes)
    known,z,e=arrays['calibration'];unknown,uz,ue=arrays['unknown_calibration']
    fit=group_fold(known);ufit=group_fold(unknown)
    y=np.array([classes.index(r['label']) for r in known])
    supported=sorted(set(y.tolist()))
    def nll(logt):
        v=z[fit]/np.exp(logt)
        return float(np.mean(logsumexp(v,axis=1)-v[np.arange(len(v)),y[fit]]))
    opt=minimize_scalar(nll,bounds=(-3,5),method='bounded');temperature=float(np.exp(opt.x))
    p=softmax(z/temperature,axis=1);up=softmax(uz/temperature,axis=1)
    pred=p.argmax(1);upred=up.argmax(1);score=p.max(1);uscore=up.max(1)
    distance=np.maximum(0,1-np.sum(e*prototypes[pred],axis=1))
    udistance=np.maximum(0,1-np.sum(ue*prototypes[upred],axis=1))
    supported_known=np.isin(pred,supported);supported_unknown=np.isin(upred,supported)
    candidates=[]
    for threshold in np.unique(np.r_[0,1,np.quantile(score[fit],np.linspace(0,1,21))]):
        for limit in np.unique(np.r_[0,2,np.quantile(distance[fit],np.linspace(0,1,21))]):
            a=(score>=threshold)&(distance<=limit)&supported_known
            ua=(uscore>=threshold)&(udistance<=limit)&supported_unknown
            if ua[ufit].mean()>.01:continue
            candidates.append((int(((pred==y)&a&fit).sum()),-int(((pred!=y)&a&fit).sum()),float(threshold),float(limit)))
    best=max(candidates) if candidates else (0,0,1.,0.)
    threshold,limit=best[2:]
    def metrics(mask,umask):
        a=(score>=threshold)&(distance<=limit)&supported_known
        ua=(uscore>=threshold)&(udistance<=limit)&supported_unknown
        return dict(known_windows=int(mask.sum()),unknown_windows=int(umask.sum()),
            correct_accept_fraction=float(((pred==y)&a)[mask].mean()),
            wrong_accept_fraction=float(((pred!=y)&a)[mask].mean()),
            abstention_fraction=float((~a)[mask].mean()),unknown_false_accept_fraction=float(ua[umask].mean()),
            closed_set_accuracy=float((pred==y)[mask].mean()))
    report=dict(schema='rfmon.ml.calibration_audit.v1',model_sha256=sha256(model),
        corpus_sha256=sha256(corpus_path),temperature=temperature,
        candidate_threshold=threshold,candidate_max_prototype_distance=limit,
        supported_development_classes=[classes[i] for i in supported],
        fit=metrics(fit,ufit),group_holdout=metrics(~fit,~ufit),
        fit_groups=sorted({r['split_group'] for r,f in zip(known,fit) if f}),
        assessment_groups=sorted({r['split_group'] for r,f in zip(known,fit) if not f}),
        acceptance_policy_enabled=False,final_test_iq_read=False,release_ready=False,
        limitations=['development groups previously inspected','one withheld unknown model only',
            'no verified non-drone background','correlated windows are not independent trials',
            'temperature estimates known-class conditional probabilities only',
            'prototype distances and thresholds are diagnostic, not a live policy'])
    write_json(output,report);return report


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--model',type=Path,required=True);p.add_argument('--corpus',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();audit(a.model,a.corpus,a.output)
