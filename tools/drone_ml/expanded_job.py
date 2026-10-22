"""Reviewed data expansion followed by controlled reference-model comparisons."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import time
import traceback
from .audit import inventory
from .common import write_json, sha256
from .corpus_sources import inspect
from .expanded_corpus import prepare
from .expanded_training import train, preflight
from .training_job import evaluate
from .diagnostics import diagnose


def run(root,output,previous_review):
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    with (output/'job.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        def status(stage,**extra):
            temp=output/'status.json.tmp'
            write_json(temp,dict(stage=stage,pid=os.getpid(),updated_unix=time.time(),**extra))
            temp.replace(output/'status.json')
        try:
            status('inventory')
            if not (output/'inventory.json').exists():write_json(output/'inventory.json',inventory(root))
            status('source_review')
            review=output/'review/review.json'
            if not review.exists():inspect(root,output/'inventory.json',review.parent,leuven_members=16,rfuav_packs=8)
            old=json.loads(Path(previous_review).read_text());new=json.loads(review.read_text())
            previous={r['split_group']:(r['partition'],r['label']) for r in old['records']}
            for row in new['records']:
                if row['split_group'] in previous and previous[row['split_group']]!=(row['partition'],row['label']):
                    raise ValueError('existing partition changed')
            status('preparing_corpus',reviewed_records=len(new['records']))
            corpus=output/'corpus/corpus.json'
            if not corpus.exists():prepare(review,corpus.parent,windows=48,full_rfuav=True,calibration_windows=12)
            status('preflight');readiness=preflight(corpus,review)
            write_json(output/'readiness.json',readiness)
            reports={}
            # Fast comparator first; neither model becomes the live default automatically.
            for architecture in ('baseline','residual'):
                folder=output/(architecture+'_model')
                if not (folder/'model.pt').exists():
                    status('training',architecture=architecture)
                    train(corpus,review,folder,epochs=40,batch_size=8,architecture=architecture,
                          resume=(folder/'training_state.pt').exists())
                status('evaluating_development',architecture=architecture)
                reports[architecture]=evaluate(folder/'model.pt',corpus)
                write_json(folder/'development_evaluation.json',reports[architecture])
                diagnose(folder/'model.pt',corpus,folder/'diagnostics.json')
            write_json(output/'comparison.json',dict(models=reports,corpus_sha256=sha256(corpus),
                final_test_iq_read=False,release_ready=False,
                calibration_policy_enabled=False,
                limitations=['no_verified_background_truth','one_unknown_calibration_model',
                             '18_classes_without_independent_calibration','live_validation_pending']))
            status('completed_development',release_ready=False)
        except BaseException:
            status('failed',traceback=traceback.format_exc());raise


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--previous-review',type=Path,required=True)
    a=p.parse_args();run(a.root,a.output,a.previous_review)
