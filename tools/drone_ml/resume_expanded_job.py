"""Continue a prepared experiment on an explicitly selected device; no source imports."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import time
import traceback
from .common import sha256, write_json
from .expanded_training import train
from .training_job import evaluate
from .diagnostics import diagnose


def run(root,device,allow_device_change=False):
    root=Path(root)
    # Same lock as the initial job: the two processes may never train concurrently.
    with (root/'job.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        def status(stage,**extra):
            p=root/'status.json.tmp'
            write_json(p,dict(stage=stage,pid=os.getpid(),device=device,updated_unix=time.time(),**extra))
            p.replace(root/'status.json')
        try:
            corpus=root/'corpus/corpus.json';review=root/'review/review.json'
            residual=root/'residual_model'
            config=json.loads((residual/'run_config.json').read_text())
            if not (root/'baseline_model/model.pt').exists():raise ValueError('completed baseline required')
            if not (residual/'training_state.pt').exists():raise ValueError('saved residual state required')
            if not (residual/'model.pt').exists():
                status('training',architecture='residual')
                train(corpus,review,residual,epochs=config['epochs'],batch_size=config['batch_size'],
                      architecture='residual',device=device,resume=True,allow_device_change=allow_device_change)
            reports={}
            for architecture in ('baseline','residual'):
                folder=root/(architecture+'_model')
                status('evaluating_development',architecture=architecture)
                reports[architecture]=evaluate(folder/'model.pt',corpus)
                write_json(folder/'development_evaluation.json',reports[architecture])
                diagnose(folder/'model.pt',corpus,folder/'diagnostics.json')
            transitions=residual/'device_transitions.json'
            write_json(root/'comparison.json',dict(models=reports,corpus_sha256=sha256(corpus),
                final_test_iq_read=False,release_ready=False,calibration_policy_enabled=False,
                device_transitions=json.loads(transitions.read_text()) if transitions.exists() else [],
                limitations=['no_verified_background_truth','one_unknown_calibration_model',
                             '18_classes_without_independent_calibration','live_validation_pending']))
            status('completed_development',release_ready=False)
        except BaseException:
            status('failed',traceback=traceback.format_exc());raise


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--job',type=Path,required=True)
    p.add_argument('--device',choices=('cpu','cuda'),required=True)
    p.add_argument('--allow-device-change',action='store_true')
    a=p.parse_args();run(a.job,a.device,a.allow_device_change)
