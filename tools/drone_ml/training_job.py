"""Sequential standalone training job and development-only model comparison."""
import argparse
from collections import Counter, defaultdict
import json
import os
from pathlib import Path
import time
import traceback
import numpy as np
import torch
from torch.utils.data import DataLoader
from .common import sha256, write_json
from .expanded_training import train, ShardedDataset
from .model import SpectrogramCNN
from .residual_model import RFResidualCNN


def evaluate(checkpoint_path,corpus_path):
    checkpoint=torch.load(checkpoint_path,map_location='cpu',weights_only=True)
    corpus=json.loads(Path(corpus_path).read_text())
    classes=checkpoint['classes'];factory=RFResidualCNN if checkpoint['architecture']=='residual' else SpectrogramCNN
    model=factory(len(classes)).eval();model.load_state_dict(checkpoint['state_dict']);torch.set_num_threads(2)
    result={}
    for partition in ('calibration','unknown_calibration'):
        rows=[r for r in corpus['records'] if r['partition']==partition]
        loader=DataLoader(ShardedDataset(Path(corpus_path).parent,corpus,rows),batch_size=8,shuffle=False)
        scores=[]
        with torch.no_grad():
            for x,_,_ in loader:
                logits,_=model(x);scores.append(logits.softmax(1).numpy())
        p=np.concatenate(scores);pred=p.argmax(1);eligible=np.array([r['model_eligible'] for r in rows])
        entry=dict(windows=len(rows),eligible_windows=int(eligible.sum()),groups=len({r['split_group'] for r in rows}),
            prediction_counts=dict(Counter(classes[k] for k in pred)),
            maximum_softmax_quantiles={str(q):float(np.quantile(p.max(1),q)) for q in (.5,.9,.95)},
            acceptance_policy_available=False,model_validated_for_live_use=False)
        if partition=='calibration':
            truth=np.array([classes.index(r['label']) for r in rows]);correct=pred==truth
            entry.update(closed_set_accuracy=float(correct.mean()),eligible_closed_set_accuracy=float(correct[eligible].mean()),
                group_balanced_accuracy=float(np.mean([correct[np.array([r['split_group']==g for r in rows])].mean()
                    for g in sorted({r['split_group'] for r in rows})])),
                per_class={label:dict(windows=int(sum(r['label']==label for r in rows)),
                    correct=int(np.sum(correct & np.array([r['label']==label for r in rows]))))
                    for label in sorted({r['label'] for r in rows})})
            votes=[]
            for rid in sorted({r['recording_id'] for r in rows}):
                indices=[i for i,r in enumerate(rows) if r['recording_id']==rid]
                best=int(p[indices].mean(0).argmax())
                votes.append(dict(recording_id=rid,truth=rows[indices[0]]['label'],prediction=classes[best],correct=classes[best]==rows[indices[0]]['label']))
            entry.update(recording_votes=votes,recording_vote_accuracy=float(np.mean([r['correct'] for r in votes])))
        else:
            entry['interpretation']='unknown-class score diagnostics; no calibrated acceptance or unknown-drone claim'
        result[partition]=entry
    return dict(architecture=checkpoint['architecture'],model_sha256=sha256(checkpoint_path),evaluated=result,
                scope='development calibration only; three known classes and Mini 3 Pro unknowns; no final test',release_ready=False)


def run(corpus,review,output):
    output=Path(output);started=time.time();reports={}
    def status(**values):
        temporary=output/'job_status.json.tmp'
        write_json(temporary,dict(pid=os.getpid(),started_unix=started,updated_unix=time.time(),**values))
        temporary.replace(output/'job_status.json')
    try:
        for architecture in ('residual','baseline'):
            status(status='training',architecture=architecture,completed_architectures=list(reports))
            folder=output/(architecture+'_model')
            train(corpus,review,folder,epochs=40,batch_size=8,architecture=architecture,device='cpu')
            status(status='evaluating_development',architecture=architecture,completed_architectures=list(reports))
            reports[architecture]=evaluate(folder/'model.pt',corpus)
            write_json(folder/'development_evaluation.json',reports[architecture])
        write_json(output/'comparison.json',dict(models=reports,corpus_sha256=sha256(corpus),final_test_iq_read=False,
            recipe='same fixed 40 epochs, seed 17, batch 8, augmentation and sampling',release_ready=False))
        status(status='completed',completed_architectures=list(reports),comparison=str(output/'comparison.json'))
    except BaseException as error:
        status(status='failed',error=str(error),completed_architectures=list(reports),traceback=traceback.format_exc())
        raise


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus',type=Path,required=True);p.add_argument('--review',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();run(a.corpus,a.review,a.output)
if __name__=='__main__':main()
