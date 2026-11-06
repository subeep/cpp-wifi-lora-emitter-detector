"""Offline Phantom recording localization and targeted DroneID compatibility replay."""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

import numpy as np
from scipy.fft import fft

RATE = 60_000_000
CENTER = 2_437_500_000
FFT_SIZE = 512
CELL = 8192
# Four broad adjacent frequency regions inside the nominal receiver band.
EDGES = [-112, -56, 0, 56, 112]


def map_file(path):
    """Power maps cover whole cells. A partial final cell is recorded separately."""
    before = path.stat()
    if before.st_size % 8:
        raise ValueError(f'Misaligned CF32 file: {path}')
    taper = np.hanning(FFT_SIZE).astype(np.float32)
    blocks = []
    with path.open('rb') as stream:
        while True:
            samples = np.fromfile(stream, dtype='<c8', count=CELL * 256)
            if not len(samples):
                break
            if not np.isfinite(samples).all():
                raise ValueError(f'Nonfinite IQ: {path}')
            usable = len(samples) // CELL * CELL
            if usable:
                spectra = fft(samples[:usable].reshape(-1, FFT_SIZE) * taper, axis=1)
                power = np.abs(spectra) ** 2
                power = power.reshape(-1, CELL // FFT_SIZE, FFT_SIZE).mean(axis=1)
                columns = [power[:, np.arange(a,b) % FFT_SIZE].mean(axis=1)
                           for a,b in zip(EDGES, EDGES[1:])]
                blocks.append(np.column_stack(columns))
    if path.stat().st_mtime_ns != before.st_mtime_ns or path.stat().st_size != before.st_size:
        raise ValueError(f'Source changed: {path}')
    return np.concatenate(blocks), before.st_size // 8


def inspect(path, total, cell, band, worker):
    count = min(RATE // 50, total)
    start = min(max(0, cell*CELL-count//2), total-count)
    with path.open('rb') as stream:
        stream.seek(start*8)
        raw = stream.read(count*8)
    if len(raw) != count*8:
        raise ValueError('Short candidate window')
    frame = struct.pack('<4sIdddQ', b'DDIQ', count, RATE, CENTER, start/RATE, 1)
    run = subprocess.run([str(worker.resolve())],input=frame+raw,capture_output=True,
                         timeout=60,check=True)
    hello, result = map(json.loads, run.stdout.splitlines())
    if not hello.get('ready') or 'error' in result:
        raise ValueError(f'Unsuccessful native replay: {path}')
    return dict(band=band,peak_cell=int(cell),offset=start,samples=count,
                sha256=hashlib.sha256(raw).hexdigest(),packets=result['native_packets'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventory',type=Path)
    parser.add_argument('worker',type=Path)
    parser.add_argument('output',type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    records = json.loads(args.inventory.read_text())['recordings']
    for record in sorted(records,key=lambda r:r['interference']!='clean'):
        if record['dataset_label'] != 'dji_phantom_4':
            continue
        path = Path(record['path'])
        power,total = map_file(path)
        np.save(args.output/(path.stem+'.npy'),power)
        # Peak cell in each region. Replay the parent wideband IQ with the
        # stock worker's own channel selection. Overlapping windows can repeat.
        views = [inspect(path,total,int(np.argmax(column)),i,args.worker)
                 for i,column in enumerate(power.T)]
        row = dict(path=str(path),mode=record['mode'],interference=record['interference'],
                   total_samples=total,mapped_samples=len(power)*CELL,
                   map_cell_samples=CELL,frequency_edges_hz=[CENTER+n*RATE/FFT_SIZE for n in EDGES],
                   views=views)
        with (args.output/'records.jsonl').open('a') as out:
            print(json.dumps(row),file=out)
        print(path.stem,flush=True)


if __name__ == '__main__':
    main()
