"""Replay exact saved live inputs, or inspect bounded intervals/views in CF32.

Offline diagnostics only. Never labels training data, reads reserved test sets
automatically, changes a checkpoint, or opens a receiver.
"""
import argparse
from collections import Counter
from dataclasses import replace
import hashlib
import json
import math
from pathlib import Path

import numpy as np

from .common import read_source, sha256, stamp
from .live_audit import write_new_json
from .predict import InferenceEngine
from .presence import apply_unqualified_policy


def read_json(path):
    with read_source(path) as f:
        return json.load(f)


def read_interval(path, rate, offset, duration=.008):
    if not math.isfinite(rate) or rate != int(rate) or not 20e6 <= rate <= 500e6:
        raise ValueError('expected integer actual sample rate between 20 and 500 MSps')
    if type(offset) is not int or offset < 0:
        raise ValueError('invalid sample offset')
    n = round(rate * duration)
    if not 1 <= n <= 4_000_000:
        raise ValueError('interval exceeds budget')
    before = stamp(path)
    if before[2] % 8 or (offset + n) * 8 > before[2]:
        raise ValueError('short or misaligned CF32')
    with read_source(path) as f:
        f.seek(offset * 8)
        raw = f.read(n * 8)
    if stamp(path) != before or len(raw) != n * 8:
        raise ValueError('source changed or short read')
    return np.frombuffer(raw, dtype='<c8'), hashlib.sha256(raw).hexdigest()


def quality(iq):
    finite = bool(np.isfinite(iq).all())
    if not finite:
        return dict(finite=False)
    v = iq.astype(np.complex128)
    return dict(finite=True, rms=float(np.sqrt(np.mean(np.abs(v)**2))),
                peak_component=float(max(np.max(np.abs(v.real)), np.max(np.abs(v.imag)))),
                component_ge_099_fraction=float(np.mean((np.abs(v.real) >= .99) | (np.abs(v.imag) >= .99))),
                clipping_interpretation='near-full-scale proxy, not proof of clipping')


def evaluate(engine, iq, rate, offset, sweep):
    original = engine.config
    rows = []
    configs = [('checkpoint', original)]
    extent = .45 * rate - original.rate_hz / 2
    if sweep and rate > original.rate_hz and extent >= 0:
        configs += [('diagnostic_fixed_view', replace(original, auto_frequency_view=False, offset_hz=float(x)))
                    for x in np.linspace(-extent, extent, 5)]
    try:
        for kind, config in configs:
            engine.config = config
            result = apply_unqualified_policy(engine.predict_iq(iq, rate, offset))
            rows.append(dict(view=kind, result=result))
    finally:
        engine.config = original
    return rows


def summarize(rows, expected_label=None):
    primary = [r['views'][0]['result'] for r in rows]
    scores = [next((c['score'] for c in r.get('class_scores', []) if c['label'] == expected_label), None)
              for r in primary]
    scores = [x for x in scores if x is not None]
    return dict(intervals=len(rows), candidate_counts=dict(Counter(r.get('candidate', 'unavailable') for r in primary)),
                engine_status_counts=dict(Counter(r['status'] for r in primary)),
                expected_context_label=expected_label,
                expected_label_top_candidate_count=sum(r.get('candidate') == expected_label for r in primary) if expected_label else None,
                expected_label_mean_relative_score=float(np.mean(scores)) if scores else None,
                interpretation='Candidate agreement only; emitter/window truth is not verified. No detection accuracy or false-alert rate is estimated.')


def audit_file(engine, path, rate, count=12, sweep=False, expected_label=None):
    if not 1 <= count <= 128:
        raise ValueError('interval count must be 1..128')
    if not math.isfinite(rate) or rate != int(rate) or not 20e6 <= rate <= 500e6:
        raise ValueError('expected integer actual sample rate between 20 and 500 MSps')
    before = stamp(path)
    n = round(rate * engine.config.duration_seconds)
    if n < 1 or before[2] % 8 or before[2] // 8 < n:
        raise ValueError('short or misaligned CF32')
    # Deterministic disjoint windows spread across the recording, including first.
    slots = before[2] // 8 // n
    offsets = [int(x) * n for x in np.linspace(0, slots - 1, min(count, slots), dtype=np.int64)]
    rows = []
    for offset in offsets:
        iq, digest = read_interval(path, rate, offset, engine.config.duration_seconds)
        rows.append(dict(offset_samples=offset, iq_sha256=digest, quality=quality(iq),
                         views=evaluate(engine, iq, rate, offset, sweep)))
    if stamp(path) != before:
        raise ValueError('source changed during audit')
    return dict(source=str(Path(path).resolve()), source_stamp=before, sample_rate_hz=rate,
                sampled_seconds=len(rows) * engine.config.duration_seconds,
                source_seconds=before[2] / 8 / rate, rows=rows, summary=summarize(rows, expected_label))


def audit_session(engine, directory, model_hash):
    directory = Path(directory).resolve()
    session = read_json(directory / 'session.json')
    if session.get('schema') != 'rfmon.drone_ml.live_audit.v1':
        raise ValueError('unsupported audit session')
    rows = []
    for path in sorted(directory.glob('[0-9][0-9][0-9][0-9].json')):
        if len(rows) >= 64:
            raise ValueError('session exceeds interval limit')
        item = read_json(path)
        if item.get('schema') != 'rfmon.drone_ml.audit_interval.v1':
            raise ValueError('unsupported audit interval')
        iq_path = (directory / item['iq_file']).resolve()
        if iq_path.parent != directory or iq_path.name != path.stem + '.cf32':
            raise ValueError('invalid audit IQ path')
        rate = float(item['request']['sample_rate_hz'])
        offset = int(item['request'].get('offset_samples', 0))
        iq, digest = read_interval(iq_path, rate, 0, engine.config.duration_seconds)
        if digest != item['iq_sha256'] or iq.nbytes != item['iq_bytes'] or iq_path.stat().st_size != iq.nbytes:
            raise ValueError('audit IQ integrity mismatch')
        views = evaluate(engine, iq, rate, offset, False)
        replay = views[0]['result']
        saved = item['result']
        parity = None
        if model_hash == session['model_sha256']:
            a, b = saved.get('class_scores', []), replay.get('class_scores', [])
            parity = (saved.get('candidate') == replay.get('candidate')
                      and saved.get('status') == replay.get('status')
                      and saved.get('selected_offset_hz') == replay.get('selected_offset_hz')
                      and len(a) == len(b)
                      and all(x['label'] == y['label'] and abs(x['score'] - y['score']) <= 1e-6 for x, y in zip(a, b)))
        rows.append(dict(interval=path.stem, request=item['request'], iq_sha256=digest,
                         saved_result=saved, replay_matches_saved=parity, quality=quality(iq), views=views))
    if not rows:
        raise ValueError('no complete recorded intervals')
    return dict(session=session, source=str(directory), rows=rows, summary=summarize(rows),
                replay_comparison='score tolerance 1e-6; only compared for identical checkpoint hash',
                session_replay_matches_saved=(all(row['replay_matches_saved'] for row in rows)
                                              if model_hash == session['model_sha256'] else None))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--model', type=Path, required=True)
    inputs = p.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--iq', type=Path)
    inputs.add_argument('--session', type=Path)
    p.add_argument('--sample-rate-hz', type=int)
    p.add_argument('--intervals', type=int, default=12)
    p.add_argument('--sweep-views', action='store_true')
    p.add_argument('--expected-context-label')
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.iq and args.sample_rate_hz is None:
        p.error('--iq requires the actual --sample-rate-hz')
    if args.session and (args.sample_rate_hz is not None or args.sweep_views or args.expected_context_label):
        p.error('session replay uses recorded metadata and the checkpoint view')
    if args.output.exists():
        p.error('output already exists; choose a new report path')
    output = args.output.resolve()
    source = (args.session or args.iq).resolve()
    if output == args.model.resolve() or output == source or (args.session and source in output.parents):
        p.error('write reports outside source sessions and checkpoint files')
    engine = InferenceEngine(args.model)
    if args.expected_context_label and args.expected_context_label not in engine.checkpoint['classes']:
        p.error('expected label is not in this checkpoint')
    digest = sha256(args.model)
    result = (audit_session(engine, args.session, digest) if args.session else
              audit_file(engine, args.iq, args.sample_rate_hz, args.intervals, args.sweep_views, args.expected_context_label))
    result.update(schema='rfmon.drone_ml.input_audit.v1', model=str(args.model.resolve()),
                  model_sha256=digest, preprocessing=engine.config.metadata(),
                  training_eligible=False, live_identity_enabled=False)
    write_new_json(output, result)
    print(json.dumps(result['summary'], indent=2))


if __name__ == '__main__':
    main()
