"""Development-only error, confidence and latency audit; never enable acceptance."""
import argparse
from collections import Counter
import json
from pathlib import Path
import time
import numpy as np
import torch
from torch.utils.data import DataLoader
from .predict import InferenceEngine
from .expanded_training import ShardedDataset
from .common import write_json, sha256


def diagnose(model_path, corpus_path, output):
    engine=InferenceEngine(model_path)
    corpus=json.loads(Path(corpus_path).read_text())
    classes=engine.checkpoint['classes']
    if classes!=corpus['classes']:raise ValueError('class index mismatch')
    if corpus['normalization']!=engine.checkpoint['normalization'] or corpus['preprocessing']!=engine.checkpoint['preprocessing']:
        raise ValueError('checkpoint/corpus preprocessing mismatch')
    result={}
    for partition in ('calibration','unknown_calibration'):
        rows=[r for r in corpus['records'] if r['partition']==partition]
        loader=DataLoader(ShardedDataset(Path(corpus_path).parent,corpus,rows),batch_size=8)
        scores=[];timings=[]
        with torch.no_grad():
            for x,_,_ in loader:
                start=time.perf_counter();logits,_=engine.model(x)
                timings.append((time.perf_counter()-start)*1000/len(x))
                scores.extend(logits.softmax(1).numpy())
        scores=np.asarray(scores);pred=scores.argmax(1);best=scores.max(1)
        correct=np.array([classes[p]==r['label'] for p,r in zip(pred,rows)])
        eligible=np.array([r['model_eligible'] for r in rows])
        confusion=Counter((r['label'],classes[p]) for r,p in zip(rows,pred))
        per_group={}
        for group in sorted({r['split_group'] for r in rows}):
            ix=np.array([r['split_group']==group for r in rows])
            per_group[group]=dict(windows=int(ix.sum()),accuracy=float(correct[ix].mean()),
                                 median_score=float(np.median(best[ix])))
        thresholds=[]
        for threshold in (.5,.8,.9,.95,.97,.98,.99,.995):
            accepted=(best>=threshold)&eligible
            thresholds.append(dict(threshold=threshold,accepted=int(accepted.sum()),
                total=len(rows),eligible=int(eligible.sum()),correct_accepted=int((correct&accepted).sum())))
        result[partition]=dict(groups=per_group,confusion=[dict(truth=a,predicted=b,count=n) for (a,b),n in confusion.items()],
            score_thresholds=thresholds,mean_model_ms_per_window=float(np.mean(timings)),
            latency_scope='batch8 model forward only, not streaming/preprocessing latency')
    report=dict(models=str(model_path),model_sha256=sha256(model_path),corpus_sha256=sha256(corpus_path),
        development=result,policy_enabled=False,final_test_read=False,
        blockers=['only_three_known_classes_calibrated','one_unknown_model','no_verified_background_truth',
                  '18_training_only_classes','no_independent_live_validation'])
    write_json(output,report)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--model',type=Path,required=True);p.add_argument('--corpus',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();diagnose(a.model,a.corpus,a.output)
