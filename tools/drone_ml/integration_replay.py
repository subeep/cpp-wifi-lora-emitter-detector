"""Read-only, train-partition IQ replay through the integrated drone backends.

Publisher recording context is not per-packet truth. No training, threshold
tuning, reserved IQ access or absence labels are authorized by this harness.
"""
import argparse
from collections import defaultdict
from contextlib import contextmanager
import hashlib
import io
import json
import os
from pathlib import Path
import selectors
import struct
import subprocess
import tempfile
import time
import zipfile

import h5py
import numpy as np

from .common import new_output, read_source, sha256, stamp, write_json
from .drff import vector, scalar
from .recognition_review import validate_frozen_review, load_json

HEADER = struct.Struct('<4sIdddQ')


def select_records(review, plan):
    validate_frozen_review(review, plan)
    strata = defaultdict(list)
    for r in review['records']:
        if r['partition'] == 'train':
            strata[(r['label'], r['dataset'], '2G' if r['center_hz'] < 3e9 else '5G')].append(r)
    return [min(rows, key=lambda r: (r['source'], r.get('member') or ''))
            for _, rows in sorted(strata.items())]


@contextmanager
def recording(row, *, development_replay=False):
    # The RAR helper uses an external reader: require a read-only mount too.
    allowed = row.get('partition') == 'train' or (development_replay and row.get('partition') in ('calibration', 'unknown_calibration'))
    if not allowed:
        raise ValueError('reserved or non-training source rejected before opening')
    path = Path(row['source'])
    if not os.statvfs(path).f_flag & os.ST_RDONLY:
        raise ValueError('dataset mount must be read-only for this corpus replay')
    before = stamp(path)
    if before[2] != row['source_stat'][2] or before[4] != row['source_stat'][4]:
        raise ValueError('source size/mtime differs from frozen review')
    rate = int(row['rate_hz'])
    if not 10e6 <= rate <= 250e6:
        raise ValueError('native backend sample rate unsupported')
    provenance = dict(source_stat_before=before, scaling='publisher_float_units', whole_source_sha256=None)

    def bounded(get, total):
        def read(start, count):
            if type(start) is not int or type(count) is not int or not 0 <= start < total or not 1 <= count <= 40000000 or start+count > total:
                raise ValueError('bounded source interval required')
            x = np.asarray(get(start, count), dtype='<c8')
            if x.shape != (count,) or not np.isfinite(x).all():
                raise ValueError('short or non-finite IQ')
            return x
        return read

    try:
        if row['adapter'] == 'drff':
            with read_source(path) as f, h5py.File(f, 'r') as h:
                centers = [scalar(h, k) for k in ('CenterFrequence', 'CenterFreq') if k in h]
                if scalar(h, 'Fs') != rate or not centers or any(c != row['center_hz'] for c in centers):
                    raise ValueError('source rate/center mismatch')
                i, q = vector(h, 'RF0_I'), vector(h, 'RF0_Q')
                if i.shape != q.shape or len(i.shape) != 2 or min(i.shape) != 1 or i.size != row['total_samples']:
                    raise ValueError('source vector mismatch')
                def get(start, count):
                    sl = (slice(start, start+count), 0) if i.shape[1] == 1 else (0, slice(start, start+count))
                    return np.asarray(i[sl], dtype=np.float32)+1j*np.asarray(q[sl], dtype=np.float32)
                yield i.size, bounded(get, i.size), provenance
        elif row['adapter'] == 'ci16':
            total = before[2]//4
            if before[2] % 4 or total != row['total_samples']:
                raise ValueError('int16 source size mismatch')
            provenance['scaling'] = 'signed_int16_divided_by_32768'
            with read_source(path) as f:
                def get(start, count):
                    f.seek(start*4); v = np.frombuffer(f.read(count*4), dtype='<i2').astype(np.float32)/32768.
                    return v[::2]+1j*v[1::2]
                yield total, bounded(get, total), provenance
        elif row['adapter'] == 'leuven':
            with read_source(path) as f, zipfile.ZipFile(f) as z:
                if z.namelist().count(row['member']) != 1:
                    raise ValueError('missing/duplicate reviewed member')
                info = z.getinfo(row['member'])
                if info.file_size != row['member_bytes'] or info.CRC != row['member_crc32'] or info.file_size > 512*1024**2:
                    raise ValueError('member mismatch/size budget')
                raw = z.read(info)
            provenance.update(member_sha256=hashlib.sha256(raw).hexdigest(), member_crc_verified=True)
            with h5py.File(io.BytesIO(raw), 'r') as h:
                d = vector(h, 'uhd_samps')
                if d.dtype.names != ('real', 'imag') or len(d.shape) != 2 or d.shape[0] != 1:
                    raise ValueError('unsupported Leuven layout')
                def get(start, count):
                    v = d[0, start:start+count]
                    return v['real'].astype(np.float32)+1j*v['imag'].astype(np.float32)
                yield d.size, bounded(get, d.size), provenance
        elif row['adapter'] == 'rfuav':
            total = row['total_samples']
            if not 1 <= total <= 125000000 or not row['member'].endswith('.iq'):
                raise ValueError('RAR member budget')
            # Decode only the reviewed member to an anonymous scratch file.
            with tempfile.TemporaryFile() as scratch:
                result = subprocess.run(['unrar', 'p', '-inul', str(path), row['member']],
                                        stdout=scratch, stderr=subprocess.PIPE, timeout=120)
                if result.returncode or scratch.tell() != total*8:
                    raise ValueError('RAR member decode/length failed')
                provenance['member_crc_verified'] = True
                def get(start, count):
                    scratch.seek(start*8)
                    return np.frombuffer(scratch.read(count*8), dtype='<c8')
                yield total, bounded(get, total), provenance
        else:
            raise ValueError('unsupported reviewed adapter')
    finally:
        if stamp(path) != before:
            raise ValueError('source stat changed during replay')


class Worker:
    """Bounded framed stdin/JSON stdout adapter with per-request deadlines."""
    def __init__(self, command, log):
        self.log = open(log, 'wb')
        self.p = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log)
        self.pending = bytearray()
        os.set_blocking(self.p.stdin.fileno(), False)
        os.set_blocking(self.p.stdout.fileno(), False)
        try:
            if not self.exchange(None).get('ready'):
                raise ValueError('invalid worker handshake')
        except BaseException:
            self.close(); raise

    def exchange(self, payload, timeout=45):
        deadline = time.monotonic()+timeout
        remaining = memoryview(payload) if payload is not None else memoryview(b'')
        with selectors.DefaultSelector() as selector:
            selector.register(self.p.stdout, selectors.EVENT_READ)
            if remaining:
                selector.register(self.p.stdin, selectors.EVENT_WRITE)
            while True:
                if not remaining and b'\n' in self.pending:
                    line, _, rest = self.pending.partition(b'\n'); self.pending = bytearray(rest)
                    return json.loads(line)
                if time.monotonic() > deadline:
                    raise TimeoutError('worker response deadline')
                for key, _ in selector.select(.1):
                    if key.fileobj is self.p.stdin:
                        try: n = os.write(self.p.stdin.fileno(), remaining[:1048576])
                        except BlockingIOError: continue
                        remaining = remaining[n:]
                        if not remaining: selector.unregister(self.p.stdin)
                    else:
                        block = os.read(self.p.stdout.fileno(), 65536)
                        if not block: raise RuntimeError('worker channel closed')
                        self.pending.extend(block)
                        if len(self.pending) > 131072:
                            raise ValueError('worker JSON exceeded transport limit')

    def request(self, iq, rate, center, start, epoch=1):
        raw = np.asarray(iq, dtype='<c8').tobytes()
        return self.exchange(HEADER.pack(b'DDIQ', len(iq), rate, center, start/rate, epoch)+raw)

    def close(self):
        if self.p.poll() is None:
            self.p.terminate()
            try: self.p.wait(timeout=2)
            except subprocess.TimeoutExpired: self.p.kill(); self.p.wait()
        self.p.stdin.close(); self.p.stdout.close(); self.log.close()


def native_summary(jobs):
    packets = [p for j in jobs for p in j.get('native_packets', [])]
    evidence = [e for j in jobs for e in j.get('o4_evidence', [])]
    return dict(crc24_packets=len(packets), inner_crc16_packets=sum(p['inner_crc16'] for p in packets),
        telemetry_packets=sum(p['telemetry_parsed'] for p in packets),
        reported_models=sorted({p.get('reported_model') for p in packets if p.get('reported_model')}),
        o4_windows=len(evidence), o4_candidate_windows=sum(e['candidate'] for e in evidence),
        errors=[j['error'] for j in jobs if 'error' in j],
        processing_ms=sum(j['processing_ms'] for j in jobs))


def run_case(row, root, case_dir, seconds):
    started = time.monotonic()
    result = dict(context_label=row['label'], dataset=row['dataset'], source=row['source'], member=row.get('member'),
                  partition=row['partition'], collection_group=row['split_group'], source_recording_id=row['recording_id'],
                  sample_rate_hz=row['rate_hz'], center_hz=row['center_hz'], training_eligible=False)
    case_dir.mkdir()
    try:
        with recording(row) as (available, get, provenance):
            rate = int(row['rate_hz']); center = row['center_hz']
            limit = min(available, round(rate*seconds)); chunk = round(rate*.16)
            digest = hashlib.sha256(); jobs = []; peak = 0.; clipped = 0
            worker = Worker([str(root/'build/drone_native_worker')], case_dir/'native.stderr.log')
            try:
                for at in range(0, limit, chunk):
                    iq = get(at, min(chunk, limit-at)); digest.update(iq.tobytes())
                    peak = max(peak, float(np.abs(iq.real).max()), float(np.abs(iq.imag).max()))
                    clipped += int(np.count_nonzero((np.abs(iq.real) >= .999) | (np.abs(iq.imag) >= .999)))
                    response = worker.request(iq, rate, center, at)
                    response.update(input_start_sample=at, input_samples=len(iq))
                    jobs.append(response)
            finally:
                worker.close()
            write_json(case_dir/'native.json', jobs)
            result.update(available_samples=available, tested_samples=limit, tested_seconds=limit/rate,
                          decoded_iq_sha256=digest.hexdigest(), source_provenance=provenance,
                          component_peak=peak, component_ge_0999_fraction=clipped/limit,
                          native_first_prefix=native_summary(jobs[:1]), native_extended=native_summary(jobs))
            if rate <= 100000000:
                count = min(2000000, round(rate*.02), limit)
                windows = min(4, limit//count)
                offsets = [int(v) for v in np.linspace(0, limit-count, windows)]
                legacy = Worker(['/usr/bin/python3.10', '-u', str(root/'src/drones/python/backend.py'),
                                 '--turbo', str(root/'build/librfmon_drone_turbo.so')], case_dir/'legacy.stderr.log')
                outputs = []
                try:
                    for epoch, at in enumerate(offsets, 1):
                        r = legacy.request(get(at, count), rate, center, at, epoch)
                        r.update(input_start_sample=at, input_samples=count); outputs.append(r)
                finally:
                    legacy.close()
                write_json(case_dir/'legacy.json', outputs)
                events = [e for r in outputs for e in r['events'] if e.get('confirmed')]
                result['legacy'] = dict(status='tested', samples=windows*count, seconds=windows*count/rate,
                    confirmed_broadcasts=len(events), reported_models=sorted({e.get('model','') for e in events}),
                    errors=[r['error'] for r in outputs if 'error' in r], processing_ms=sum(r.get('processing_ms',0) for r in outputs))
            else:
                result['legacy'] = dict(status='unsupported_live_sample_rate', confirmed_broadcasts=0)
            # Retain only a short, provenance-bound prefix for subsequent input preparation.
            n = min(limit, round(rate*.288), 40000000)
            iq = get(0, n)
            with (case_dir/'model_prefix.cf32').open('xb') as f: f.write(iq.tobytes())
            result['model_prefix'] = dict(path=str(case_dir/'model_prefix.cf32'), samples=n, start_sample=0,
                sha256=sha256(case_dir/'model_prefix.cf32'), labels_created=False, training_eligible=False)
        result.update(status='complete', source_stat_preserved=True)
    except Exception as e:
        result.update(status='failed', error=f'{type(e).__name__}: {e}')
    result['wall_seconds'] = time.monotonic()-started
    write_json(case_dir/'result.json', result)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--review', type=Path, required=True); p.add_argument('--plan', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True); p.add_argument('--seconds', type=float, default=1.)
    args = p.parse_args()
    if not .16 <= args.seconds <= 1.: raise ValueError('replay budget: 160 ms to 1 second/source')
    review, plan = load_json(args.review), load_json(args.plan)
    rows = select_records(review, plan)
    root = Path(__file__).resolve().parents[2]
    output = new_output(args.output, [Path(r['source']).parent for r in rows]).resolve()
    metadata = dict(schema='rfmon.drone_ml.integration_replay.v1', selection='first lexical training record per model/dataset/band',
        review_sha256=sha256(args.review), plan_sha256=sha256(args.plan), native_worker_sha256=sha256(root/'build/drone_native_worker'),
        harness_sha256=sha256(__file__), records=rows, seconds_per_source=args.seconds,
        final_test_iq_read=False, independent_evaluation=False, training_started=False)
    write_json(output/'manifest.json', metadata)
    results = []
    for n, row in enumerate(rows):
        write_json(output/'progress.json', dict(phase='replaying', completed=n, total=len(rows), current=row['label']))
        r = run_case(row, root, output/f'case_{n:02d}', args.seconds); results.append(r)
        write_json(output/'results.json', results)
        print(json.dumps(dict(case=n, label=row['label'], status=r['status'], native=r.get('native_extended'), legacy=r.get('legacy'), error=r.get('error'))), flush=True)
    write_json(output/'progress.json', dict(phase='complete', completed=len(results), total=len(rows), failed=sum(r['status']!='complete' for r in results)))


if __name__ == '__main__':
    main()
