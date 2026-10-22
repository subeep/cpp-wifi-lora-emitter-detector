"""Dependent calibration audits after one specified training job completes."""
import argparse
import fcntl
import json
from pathlib import Path
import time
import traceback
from .calibration_audit import audit
from .common import write_json


def finish(root):
    root=Path(root)
    with (root/'calibration_job.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        def status(stage,**extra):
            p=root/'calibration_status.json.tmp'
            write_json(p,dict(stage=stage,updated_unix=time.time(),**extra))
            p.replace(root/'calibration_status.json')
        status('waiting_for_training')
        deadline=time.monotonic()+7*86400
        try:
            for name in ('baseline','residual'):
                status('waiting_for_checkpoint',architecture=name)
                while not (root/(name+'_model/model.pt')).exists():
                    if time.monotonic()>deadline:raise TimeoutError('training did not finish within seven days')
                    if json.loads((root/'status.json').read_text())['stage']=='failed':
                        status('blocked_by_training_failure');return
                    time.sleep(15)
                status('calibrating_development',architecture=name)
                audit(root/(name+'_model/model.pt'),root/'corpus/corpus.json',
                      root/(name+'_model/calibration_audit.json'))
            status('waiting_for_comparison')
            while not (root/'comparison.json').exists():
                if time.monotonic()>deadline:raise TimeoutError('comparison did not finish within seven days')
                if json.loads((root/'status.json').read_text())['stage']=='failed':
                    status('blocked_by_training_failure');return
                time.sleep(15)
            comparison=json.loads((root/'comparison.json').read_text())
            ranking=sorted(comparison['models'],key=lambda n:
                comparison['models'][n]['evaluated']['calibration']['group_balanced_accuracy'],reverse=True)
            write_json(root/'development_selection.json',dict(
                development_accuracy_ranking=ranking,selected_for_live_acceptance=None,
                default_model_changed=False,release_ready=False,
                reason='independent class coverage, background and live validation remain incomplete'))
            status('completed_development_audits',release_ready=False)
        except Exception:
            status('failed',traceback=traceback.format_exc());raise


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--job',type=Path,required=True)
    finish(p.parse_args().job)
