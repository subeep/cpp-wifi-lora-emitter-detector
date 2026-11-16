"""D1: live worker for the universal classifier. Same framed socket protocol as live_worker.py.

No radio ownership and no network listeners. Loads one package (`universal_package.py`).

Per 8 ms interval (20 MSps only; the network was trained on 20 MSps views and nothing else):
  - the existing spectrogram (also sent as the STFT preview) pooled to the training input, and
    the 29 measured facts;
  - gated type scores for the interval (the family gate: a type only where its own measurement
    fired), reported as `top_candidates` so older panels keep working.
On the last interval of a capture the worker combines the capture's intervals exactly as the
C4 evaluation does (`universal_c4.Views.capture` + `decide`) and reports `universal.capture`:
the type (or none), the ranked models of that type (up to 3) and whether they pass the naming
limits (score and distance). Rows, agreement across captures and the family check against the
row's measured link are the app's job.
"""
import argparse
import os
from pathlib import Path
import resource
import socket
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
os.environ['OMP_NUM_THREADS'] = '2'
os.environ['OPENBLAS_NUM_THREADS'] = '2'
os.environ['MKL_NUM_THREADS'] = '2'
import numpy as np
import torch
from tools.drone_ml.common import sha256
from tools.drone_ml.live_worker import receive, send, stft_preview
from tools.drone_ml.preprocess import spectrogram
from tools.drone_ml.universal_c2_train import Runner, TwoLevel
from tools.drone_ml.universal_c4 import Namer, Views, explain
from tools.drone_ml.universal_cache import CONFIG
from tools.drone_ml.universal_features import NAMES, extract
from tools.drone_ml.universal_package import ARCHITECTURE, FORMAT
from tools.drone_ml.universal_train import Norm

RATE = 20_000_000
SCHEMA = 'rfmon.ml.universal.v1'


def load_package(path):
    pkg = torch.load(path, weights_only=False, map_location='cpu')
    if pkg.get('format') != FORMAT or pkg.get('architecture') != ARCHITECTURE:
        raise ValueError('not a universal classifier package')
    net = TwoLevel(len(NAMES), len(pkg['models']), pkg.get('use_facts', True))
    net.load_state_dict(pkg['state_dict']); net.eval()
    if 'scale_mask' not in pkg['norm']:
        raise ValueError('package norm has no fixed scale mask (single live views would be scaled per batch)')
    run = Runner(net, Norm.from_state(pkg['norm']), pkg['models'], pkg['timing'])
    return pkg, run, Namer(pkg['class_stats'], pkg['models'])


def pooled(image):
    """The training input (`universal_cache.image`) from the full 256 x 625 spectrogram."""
    return image[:, :624].reshape(128, 2, 156, 4).mean(axis=(1, 3)).astype(np.float16)


class Captures:
    """Intervals of the capture in progress; a new capture key drops an unfinished one."""

    def __init__(self):
        self.key, self.items = None, []

    def add(self, request, item):
        key = (str(request.get('run_id')), str(request.get('radio_session')), str(request.get('capture_seq')),
               str(request.get('generation')))
        if key != self.key or int(request.get('interval_index', 0)) == 0:
            self.key, self.items = key, []
        self.items.append((int(request.get('interval_index', 0)), item))
        last = int(request.get('interval_index', 0)) == int(request.get('interval_count', 1)) - 1
        return last


def capture_decision(pkg, run, namer, items, expected):
    if [i for i, _ in items] != list(range(expected)):
        return dict(schema=SCHEMA + '.capture', intervals=len(items), type=None, models=[], named=False,
                    reason='incomplete capture (an interval was skipped)')
    d = dict(spec=np.stack([it['spec'] for _, it in items]), facts=np.stack([it['facts'] for _, it in items]),
             available=np.stack([it['available'] for _, it in items]), meta=[{} for _ in items])
    v = Views(run, d, pkg['model_type'])
    out = explain(v.capture(range(len(items))), pkg['decision']['type_thresholds'], pkg['decision']['naming'], namer)
    out.update(schema=SCHEMA + '.capture', intervals=len(items))
    return out


def handle(pkg, run, namer, captures, request, iq):
    rate = float(request['sample_rate_hz'])
    base = dict(status='insufficient_evidence', candidate='', top_candidates=[], selected_offset_hz=0.0)
    if rate != RATE:
        base['reason'] = ['universal classifier is trained on 20 MSps views only']
        return base, None
    x = np.asarray(iq, np.complex64)
    if not np.isfinite(x).all() or float(np.mean(np.abs(x - x.mean()) ** 2)) < 1e-24:
        base['reason'] = ['nonfinite or constant IQ']
        return base, None
    image = spectrogram(x, RATE, CONFIG)
    f = extract(x)
    item = dict(spec=pooled(image), facts=np.asarray(f['values'], np.float32), available=np.asarray(f['available'], bool))
    _, gt, _, _ = run.predict(dict(spec=item['spec'][None], facts=item['facts'][None], available=item['available'][None],
                                   meta=[{}]))
    types = pkg['types']
    order = np.argsort(-gt[0])
    result = dict(status='ok', candidate=types[order[0]], selected_offset_hz=0.0,
                  top_candidates=[dict(label=types[j], score=float(gt[0][j])) for j in order[:3]],
                  reason=['type scores after the family gate; model names come once per capture'])
    universal = dict(schema=SCHEMA, interval=dict(types={t: round(float(gt[0][j]), 4) for j, t in enumerate(types)},
                                                 allowed=[t for j, t in enumerate(types) if gt[0][j] > 0]))
    if captures.add(request, item):
        universal['capture'] = capture_decision(pkg, run, namer, captures.items, int(request.get('interval_count', 1)))
    result['universal'] = universal
    return result, image


def serve(sock, package_path):
    pkg, run, namer = load_package(package_path)
    accuracy = pkg['accuracy']
    send(sock, dict(schema='rfmon.ml.ready.v1', model_sha256=sha256(package_path), classes=pkg['models'],
                    architecture=pkg['architecture'], preprocessing=dict(rate_hz=RATE, samples=160000, duration_seconds=.008),
                    policy_available=True, validated_for_live_use=False,
                    universal=dict(schema=SCHEMA, types=pkg['types'], decision=pkg['decision']['version'],
                                   seed=pkg['seed'], development_only=True,
                                   model_types={m: pkg['types'][t] for m, t in zip(pkg['models'], pkg['model_type'])},
                                   accuracy=accuracy, background=pkg['background']),
                    audit=dict(status='disabled', path='', saved=0, error='')))
    captures = Captures()
    while True:
        request, iq = receive(sock)
        started = time.perf_counter()
        want_preview = request.get('want_stft_preview') in (True, 'true', '1')
        result, image = handle(pkg, run, namer, captures, request, iq)
        pixels = None
        if want_preview and image is not None:
            result['stft_preview'], pixels = stft_preview(image, CONFIG)
        result.update(schema='rfmon.ml.result.v1', request=request, processing_ms=(time.perf_counter() - started) * 1000,
                      audit=dict(status='disabled', path='', saved=0, error=''))
        send(sock, result)
        if pixels is not None:
            sock.sendall(struct.pack('<I', len(pixels)) + pixels)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--ipc-fd', type=int, required=True); p.add_argument('--model', required=True)
    p.add_argument('--audit-root'); p.add_argument('--audit-context'); p.add_argument('--audit-notes')
    args = p.parse_args()
    os.nice(10)
    resource.setrlimit(resource.RLIMIT_AS, (4 * 1024 ** 3, 4 * 1024 ** 3))
    torch.set_num_threads(2)
    with socket.socket(fileno=args.ipc_fd) as sock:
        try:
            serve(sock, args.model)
        except EOFError:
            pass
        except Exception as e:
            try:
                send(sock, dict(schema='rfmon.ml.error.v1', error=str(e)))
            except OSError:
                pass
            raise


if __name__ == '__main__':
    main()
