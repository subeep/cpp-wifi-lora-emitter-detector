#!/usr/bin/env python3
"""Prepare fixed conducted-lab vectors only. Does not open or transmit with SDRs.
Synthetic identities only; no target-address or SSID override. IQ preflight is
self-consistency with a software encoder, not an independent on-air oracle.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import zlib
import numpy as np

RATE = 20_000_000
BSSID = bytes.fromhex('020000face01')
CLIENTS = [bytes.fromhex(f'020000face{n:02x}') for n in range(16,20)]
SSID = b'RFMON-HACKRF-LAB'
CHANNEL = 11
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fixture_encoder',ROOT/'tests/generate_wifi_ofdm.py')
encoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(encoder)

def mpdu(subtype,seq=7,target=CLIENTS[0],retry=False,tsf=1_000_000,bssid=BSSID):
    header=struct.pack('<HH',subtype<<4,0)+target+bssid+bssid+struct.pack('<H',seq<<4)
    if retry:header=header[:1]+bytes([8])+header[2:]
    if subtype==8:
        body=struct.pack('<QHH',tsf,100,1)+bytes([0,len(SSID)])+SSID+bytes([1,1,12,3,1,CHANNEL])
    else:body=struct.pack('<H',7)
    data=header+body
    return data+struct.pack('<I',zlib.crc32(data))

def iq_packet(data):
    wave=np.r_[np.zeros(80),encoder.encode_mpdu(data),np.zeros(80)]
    scale=48/max(np.max(np.abs(wave.real)),np.max(np.abs(wave.imag)))
    return np.column_stack((np.rint(wave.real*scale),np.rint(wave.imag*scale))).astype(np.int8)

def fnv(raw):
    value=14695981039346656037
    for byte in raw:value=((value^byte)*1099511628211)&((1<<64)-1)
    return str(value)

def prepare(directory):
    directory.mkdir(parents=True,exist_ok=False)
    vectors=directory/'preflight';vectors.mkdir()
    schedule={
        'probe':[(.3,mpdu(8,target=b'\xff'*6))],
        # Exact beacon repetition with no intervening higher TSF is a negative.
        'baseline':[(.05+n*.1,mpdu(8,target=b'\xff'*6)) for n in range(10)],
        'retry_negative':[(.02+n*.03,mpdu(12,retry=True)) for n in range(33)],
        'target_distinct':[(.02+n*.03,mpdu(12,seq=n)) for n in range(33)],
        'target_repeated':[(.02+n*.03,mpdu(12)) for n in range(33)],
        'ap_distinct':[(.02+n*.03,mpdu(12,seq=n,target=CLIENTS[n%4])) for n in range(33)],
        'ap_repeated':[(.02+n*.03,mpdu(12,target=CLIENTS[n%4])) for n in range(33)],
        # Fresh identity avoids the long baseline beacon exhausting retention.
        # Redundant copies make the three required stages robust to missed frames.
        'historical_replay':[(.1+n*.1,mpdu(8,target=b'\xff'*6,bssid=bytes.fromhex('020000face02'))) for n in range(6)]+
                            [(1.+n*.1,mpdu(8,seq=8,target=b'\xff'*6,tsf=2_000_000,bssid=bytes.fromhex('020000face02'))) for n in range(8)]+
                            [(2.2+n*.1,mpdu(8,target=b'\xff'*6,bssid=bytes.fromhex('020000face02'))) for n in range(8)],
    }
    reference=[];seen={};rng=np.random.default_rng(42)
    for name,frames in schedule.items():
        seconds=4 if name=='historical_replay' else 1
        samples=np.zeros((seconds*RATE,2),np.int8)
        expected=[]
        for at,data in frames:
            packet=iq_packet(data);start=round(at*RATE)
            assert start+len(packet)<=len(samples)
            samples[start:start+len(packet)]=packet
            expected.append(dict(at_sample=start,mpdu_hex=data.hex()))
            if data not in seen:
                stem=f'packet-{len(seen):03d}';seen[data]=stem
                # Mimic receiver channel offset and low noise around the actual
                # signed-8-bit waveform, retaining quantisation exactly.
                p=(packet[:,0].astype(np.float32)+1j*packet[:,1].astype(np.float32))/128
                wave=np.zeros(40_000+len(p),np.complex64);wave[20_000:20_000+len(p)]=p
                wave*=np.exp(-2j*np.pi*1_500_000*np.arange(len(wave))/RATE)
                wave+=1e-5*(rng.normal(size=len(wave))+1j*rng.normal(size=len(wave)))
                raw=wave.astype('<c8').tobytes();(vectors/(stem+'.cf32')).write_bytes(raw)
                meta=dict(schema=1,format='cf32_le',iq_file=stem+'.cf32',samples=len(wave),
                          sample_rate_hz=RATE,channel_hz=2_462_000_000,capture_center_hz=2_463_500_000,
                          overflow=False,fnv1a64=fnv(raw),expected_mpdu_hex=data.hex(),expected_rate_mbps=6,
                          timing_note='Synthetic local software preflight; no measured device time')
                (vectors/(stem+'.json')).write_text(json.dumps(meta,indent=2)+'\n')
        path=directory/(name+'.cs8');samples.tofile(path)
        reference.append(dict(name=name,file=path.name,samples=len(samples),seconds=seconds,
                              sha256=hashlib.sha256(path.read_bytes()).hexdigest(),frames=expected))
    meta=dict(schema='rfmon-hackrf-conducted-validation/v1',format='interleaved_signed_int8_iq',
              sample_rate_hz=RATE,tx_center_hz=2_462_000_000,bssid=BSSID.hex(':'),ssid=SSID.decode(),
              clients=[m.hex(':') for m in CLIENTS],quantised_peak_component=48,
              laboratory_only=True,transmitted=False,phases=reference,
              limits='Use finite sample counts and watchdog; no uncontrolled over-air test. TX output must be independently received before detector acceptance.')
    (directory/'reference.json').write_text(json.dumps(meta,indent=2)+'\n')
    print(json.dumps(dict(directory=str(directory),packet_vectors=len(seen),phases=len(schedule))))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('new_directory',type=Path)
    args=parser.parse_args();prepare(args.new_directory)
