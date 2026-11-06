"""Read-only Phantom waveform exploration and existing DroneID baseline replay.

This is NOT a Lightbridge decoder. Never infers transport or telemetry from a
dataset label or an autocorrelation peak. Output must not be used as final-test
model evaluation: these intervals are now protocol-development observations.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

import numpy as np


def features(iq, rate):
    if not np.isfinite(iq).all():
        return {'finite': False}
    x = np.array(iq[:262144], dtype=np.complex128, copy=True)
    x -= x.mean()
    power = float(np.mean(np.abs(x)**2))
    if power == 0:
        return {'finite': True, 'constant': True}
    n = len(x)
    size = 1 << (2*n-1).bit_length()
    ac = np.fft.ifft(np.abs(np.fft.fft(x, size))**2).real[:8193]
    # Linear, overlap-normalized real autocorrelation; exploratory only.
    ac /= power * (n-np.arange(len(ac)))
    peaks = np.flatnonzero((np.abs(ac[2:-1]) > np.abs(ac[1:-2])) &
                          (np.abs(ac[2:-1]) > np.abs(ac[3:])))+2
    peaks = sorted(peaks, key=lambda k: abs(ac[k]), reverse=True)[:8]
    frames = x[:(n//4096)*4096].reshape(-1,4096)
    spectrum = np.mean(abs(np.fft.fftshift(np.fft.fft(frames*np.hanning(4096),axis=1),axes=1))**2,axis=0)
    cumulative = np.cumsum(spectrum)/spectrum.sum()
    lo,hi = np.searchsorted(cumulative,[.005,.995])
    return dict(finite=True, mean_power=power, measured_samples=n,
                power_99pct_span_hz=float((hi-lo)*rate/4096),
                peak_offset_hz=float((np.argmax(spectrum)-2048)*rate/4096),
                autocorrelation_peaks=[dict(lag_samples=int(k),lag_us=float(k/rate*1e6),
                                           real_normalized=float(ac[k])) for k in peaks])


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inventory',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--worker',type=Path,required=True)
    a=p.parse_args()
    if a.output.exists():raise ValueError('Choose a new output file')
    records=[r for r in json.loads(a.inventory.read_text())['recordings']
             if r['dataset_label']=='dji_phantom_4']
    rate=60e6;count=1200000 # 20 ms; real unpadded original samples.
    with a.output.open('x') as report:
        for r in records:
            path=Path(r['path']);before=path.stat()
            if before.st_size%8 or before.st_size<count*8:raise ValueError('Invalid IQ extent')
            total=before.st_size//8
            for offset in (0,(total-count)//2,total-count):
                with path.open('rb') as f:
                    f.seek(offset*8);raw=f.read(count*8)
                if len(raw)!=count*8:raise ValueError('Short read')
                row=dict(source=str(path),mode=r['mode'],interference=r['interference'],
                         offset_samples=offset,samples=count,sample_rate_hz=rate,
                         center_hz=2437.5e6,format='cf32_le',
                         interval_sha256=hashlib.sha256(raw).hexdigest(),
                         lightbridge_telemetry_verified=False,
                         measurements=features(np.frombuffer(raw,dtype='<c8'),rate))
                # Clean recordings only: existing native decoder is a baseline,
                # NOT a Lightbridge parser. One process/interval resets state.
                if r['interference']=='clean':
                    frame=struct.pack('<4sIdddQ',b'DDIQ',count,rate,2437.5e6,offset/rate,1)+raw
                    try:
                        run=subprocess.run([str(a.worker.resolve())],input=frame,
                            capture_output=True,timeout=45)
                        row['baseline_returncode']=run.returncode
                        row['baseline_output']=[json.loads(line) for line in run.stdout.splitlines()]
                    except subprocess.TimeoutExpired:
                        row['baseline_error']='timeout_45s'
                after=path.stat()
                if (before.st_size,before.st_mtime_ns)!=(after.st_size,after.st_mtime_ns):
                    raise ValueError('Source changed during read')
                report.write(json.dumps(row,allow_nan=False)+'\n');report.flush()
            print(path.name,r['interference'],flush=True)


if __name__=='__main__':main()
