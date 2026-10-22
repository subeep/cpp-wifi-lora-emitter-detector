"""Optional bounded recording of exact inference inputs, outside acquisition.

Session context is an operator assertion, never automatic window ground truth.
Errors disable this recorder only. Existing sessions are never overwritten.
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import uuid

import numpy as np

CONTEXTS = ('unlabelled', 'background_observation', 'controller_only',
            'mini2_linked_idle', 'mini2_linked_video')
MAX_INTERVALS = 64
MAX_BYTES = 256 * 1024 * 1024


def write_new_json(path, value):
    with Path(path).open('x') as f:
        json.dump(value, f, indent=2, allow_nan=False)
        f.write('\n')


class AuditRecorder:
    def __init__(self, root, context, notes, model_hash, preprocessing):
        if context not in CONTEXTS or len(notes) > 1024:
            raise ValueError('invalid audit session context or notes')
        root = Path(root)
        if not root.is_absolute():
            raise ValueError('audit output must be an absolute directory')
        name = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:12]
        self.path = root / name
        self.path.mkdir(parents=True, exist_ok=False)
        self.saved = 0
        self.bytes = 0
        self.status = 'recording'
        self.error = ''
        write_new_json(self.path / 'session.json', dict(
            schema='rfmon.drone_ml.live_audit.v1', session_id=name,
            operator_context=context, notes=notes, model_sha256=model_hash,
            preprocessing=preprocessing, window_truth_verified=False,
            training_eligible=False, split_group=name,
            max_intervals=MAX_INTERVALS, max_iq_bytes=MAX_BYTES,
            coverage='only submitted 8 ms intervals, not continuous or full-band',
            instruction='Review emitter/window evidence before assigning labels; keep entire sessions in one partition.',
        ))

    def snapshot(self):
        return dict(status=self.status, path=str(self.path), saved=self.saved,
                    iq_bytes=self.bytes, error=self.error)

    def record(self, iq, request, result):
        if self.status != 'recording':
            return self.snapshot()
        raw = np.asarray(iq, dtype='<c8').tobytes()
        if self.saved >= MAX_INTERVALS or self.bytes + len(raw) > MAX_BYTES:
            self.status = 'limit_reached'
            return self.snapshot()
        try:
            stem = f'{self.saved:04d}'
            # Metadata is written last; readers only consume complete pairs.
            with (self.path / (stem + '.cf32')).open('xb') as f:
                f.write(raw)
            finite = bool(np.isfinite(iq).all())
            quality = dict(finite=finite)
            if finite:
                v = np.asarray(iq, dtype=np.complex128)
                quality.update(rms=float(np.sqrt(np.mean(np.abs(v)**2))),
                               peak_component=float(max(np.max(np.abs(v.real)), np.max(np.abs(v.imag)))),
                               component_ge_099_fraction=float(np.mean((np.abs(v.real) >= .99) | (np.abs(v.imag) >= .99))),
                               clipping_interpretation='near-full-scale proxy only; depends on receiver scaling')
            write_new_json(self.path / (stem + '.json'), dict(
                schema='rfmon.drone_ml.audit_interval.v1', iq_file=stem + '.cf32',
                iq_sha256=hashlib.sha256(raw).hexdigest(), iq_bytes=len(raw),
                request=request, result=result, quality=quality,
                window_truth_verified=False, training_eligible=False,
            ))
            self.saved += 1
            self.bytes += len(raw)
            if self.saved >= MAX_INTERVALS or self.bytes >= MAX_BYTES:
                self.status = 'limit_reached'
        except Exception as e:
            self.status = 'failed'
            self.error = str(e)
        return self.snapshot()
