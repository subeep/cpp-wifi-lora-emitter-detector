#!/usr/bin/env python3
"""Exact payload/CRC regression on labelled local IQ, with optional derived stress cases.

No radio, training, or source writes. Perturbations are development diagnostics,
not independent received packets or deployment accuracy estimates.
"""
import argparse, hashlib, json, pathlib, subprocess, tempfile

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def fnv(data):
    value=14695981039346656037
    for byte in data:value=((value^byte)*1099511628211)&((1<<64)-1)
    return str(value)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--fixtures',type=pathlib.Path,required=True)
    ap.add_argument('--tool',type=pathlib.Path,required=True)
    ap.add_argument('--output',type=pathlib.Path)
    ap.add_argument('--stress',action='store_true')
    a=ap.parse_args();root=a.fixtures.resolve();tool=a.tool.resolve()
    if a.output and a.output.exists():raise ValueError('Output must be new')
    expected=json.loads((root/'expected.json').read_text());results=[];hashes={}
    if a.stress:import numpy as np
    with tempfile.TemporaryDirectory(prefix='labelled-lora-validation-') as tmp:
        for index,e in enumerate(expected):
            src=(root/e['directory']).resolve()
            if not src.is_relative_to(root):raise ValueError('Fixture path escapes root')
            for name in ['iq.cf32_le','manifest.json']:hashes[str(src/name)]=digest(src/name)
            metadata=json.loads((src/'manifest.json').read_text())
            variants=[('received',src)]
            if a.stress:
                iq=np.fromfile(src/'iq.cf32_le',dtype='<c8');rate=metadata['sample_rate_hz']
                if not np.isfinite(iq).all():raise ValueError('Nonfinite source IQ')
                rng=np.random.default_rng(20261016+index);power=float(np.mean(abs(iq)**2))
                for name,shift,snr in [('cfo_plus_1khz',1000,None),('added_noise_20db',0,20),('added_noise_10db',0,10)]:
                    samples=iq*np.exp(2j*np.pi*shift*np.arange(iq.size)/rate)
                    if snr is not None:samples+=np.sqrt(power/(2*10**(snr/10)))*(rng.normal(size=iq.size)+1j*rng.normal(size=iq.size))
                    payload=samples.astype('<c8').tobytes();dest=pathlib.Path(tmp)/f'{index}-{name}';dest.mkdir()
                    (dest/'iq.cf32_le').write_bytes(payload);m=dict(metadata);m['source']='synthetic_impairment_of:'+hashes[str(src/'iq.cf32_le')];m['iq_fnv1a64']=fnv(payload)
                    (dest/'manifest.json').write_text(json.dumps(m));variants.append((name,dest))
            for name,directory in variants:
                p=subprocess.run([str(tool),str(directory),'--json'],capture_output=True,text=True,timeout=30,check=True)
                d=json.loads(p.stdout);rows=d['rows'];want=e['payload_hex'].lower()
                valid=[r for r in rows if r['crc']=='Valid' and r['header_valid']]
                matches=[r for r in valid if ''.join(r['payload_hex'].split()).lower()==want and r['sf']==e['sf'] and r['bandwidth_khz']*1000==e['bandwidth_hz']]
                wrong=[r for r in valid if ''.join(r['payload_hex'].split()).lower()!=want]
                results.append({'recording':e['directory'],'variant':name,'exact_payload_crc_and_phy':bool(matches),'valid_crc_wrong_payload_rows':len(wrong),'hypothesis_rows':len(rows)})
    unchanged=all(digest(pathlib.Path(p))==h for p,h in hashes.items())
    baseline=[r for r in results if r['variant']=='received']
    passed=unchanged and bool(baseline) and all(r['exact_payload_crc_and_phy'] for r in baseline) and not any(r['valid_crc_wrong_payload_rows'] for r in results)
    report={'schema':'rfmon.labelled_lora_validation.v1','passed':passed,'originals_unchanged':unchanged,'source_sha256':hashes,'tool_sha256':digest(tool),'scope':'development regression; same-source variants are not independent encounters','noise_definition':'added noise power relative to whole-recording mean signal power; not measured packet SNR','cases':results}
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'passed':passed,'received_exact':sum(r['exact_payload_crc_and_phy'] for r in baseline),'received_cases':len(baseline),'stress_exact':sum(r['exact_payload_crc_and_phy'] for r in results if r['variant']!='received'),'stress_cases':len(results)-len(baseline),'originals_unchanged':unchanged}))
    if not passed:raise SystemExit(1)
if __name__=='__main__':main()
