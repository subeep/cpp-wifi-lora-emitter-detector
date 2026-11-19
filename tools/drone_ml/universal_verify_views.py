"""Verify final B1 coverage/shape/counts and one persisted-IQ checksum per class."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import numpy as np
from .universal_views import eligible,offsets,RATE,LENGTH,VERSION
from .universal_inventory import read_json,locked_name


def verify(inputs,store):
    inputs,store=Path(inputs),Path(store)
    inventory,classes,background=[read_json(inputs/n) for n in ('inventory.json','classes.json','background_split.json')]
    expected={r['recording_id']:r for r in eligible(inventory,classes,background)}
    config=read_json(store/'config.json');report=read_json(store/'report.json')
    if config['smoke'] or not report['complete'] or report['errors']:raise ValueError('B1 build incomplete')
    for name in ('inventory.json','classes.json','background_split.json'):
        matching=[digest for path,digest in config['inputs'].items() if Path(path).name==name]
        if matching!=[hashlib.sha256((inputs/name).read_bytes()).hexdigest()]:
            raise ValueError('input manifest changed: '+name)
    paths={p.stem:p for p in store.glob('*.json') if len(p.stem)==24}
    if set(paths)!=set(expected):raise ValueError('record coverage differs from eligible inventory')
    if {p.stem for p in store.glob('*.npy')}!=set(expected):raise ValueError('unexpected or missing IQ output')
    counts=Counter();samples={};total_bytes=0
    for ident,path in sorted(paths.items()):
        meta=read_json(path);row=meta['source_record'];source=Path(row['source'])
        if row!=expected[ident] or locked_name(source) or locked_name(row.get('member') or '') or not row['readable']:
            raise ValueError('unexpected or locked source record')
        signature=source.stat()
        if [signature.st_size,signature.st_mtime_ns]!=meta['source_signature']:raise ValueError('source changed')
        windows=3*len(offsets(int(row['rate_hz'])))
        array_path=path.with_suffix('.npy');data=np.load(array_path,mmap_mode='r',allow_pickle=False)
        if (data.shape!=(windows,LENGTH) or data.dtype!=np.dtype('<c8') or meta['rate_hz']!=RATE
                or meta['samples_per_window']!=LENGTH or meta['view_version']!=VERSION
                or len(meta['windows'])!=windows or array_path.stat().st_size!=meta['output_bytes']):
            raise ValueError('bad view geometry or output size: '+ident)
        for view in meta['windows']:
            if view['offset_hz'] not in offsets(int(row['rate_hz'])) or view['center_hz']!=row['center_hz']+view['offset_hz']:
                raise ValueError('bad frequency metadata')
        total_bytes+=meta['output_bytes'];counts[row['model']]+=windows
        if row['model'] not in samples:
            if not np.isfinite(data).all():raise ValueError('nonfinite persisted sample')
            if hashlib.sha256(data.tobytes()).hexdigest()!=meta['iq_sha256']:raise ValueError('persisted IQ checksum mismatch')
            samples[row['model']]=ident
        del data
    if dict(counts)!=report['windows_per_class'] or sum(counts.values())!=report['total_windows']:
        raise ValueError('reported class counts disagree')
    if report['completed_recordings']!=len(expected):raise ValueError('reported record count disagrees')
    return dict(schema='rfmon.drone_ml.universal_views_verification.v1',passed=True,
                records=len(expected),windows=sum(counts.values()),bytes=total_bytes,
                counts_per_class=dict(sorted(counts.items())),checksummed_record_per_class=samples,
                original_iq_opened=False,locked_iq_opened=False,
                verification_scope='All metadata, source size/mtime and output array headers; one complete persisted output record per class checked for finite samples and checksum. Not a full re-hash of every output.')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--inputs',type=Path,required=True)
    p.add_argument('--store',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=verify(a.inputs,a.store);a.output.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps({k:v for k,v in r.items() if k not in ('counts_per_class','checksummed_record_per_class')}))
if __name__=='__main__':main()
