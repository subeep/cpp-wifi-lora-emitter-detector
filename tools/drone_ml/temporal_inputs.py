"""Prepare one bounded, fixed-view temporal STFT preview for offline experiments.

Preserves local 8 ms preprocessing, original offsets and source grouping. It
does not create reviewed training labels or join separate recordings/gaps.
"""
import argparse
import json
import math
from pathlib import Path

import numpy as np

from .common import read_source, sha256, stamp, new_output
from .live_audit import write_new_json
from .preprocess import STFTConfig, spectrogram
from .temporal_audit import check_record, reader


def tiles_from_reader(get,available,rate,start,count,offset_hz):
    if (type(start) is not int or start<0 or type(count) is not int or not 1<=count<=64
            or not math.isfinite(offset_hz)):
        raise ValueError('invalid temporal extent or frequency view')
    config=STFTConfig(duration_seconds=.008,hop=256,offset_hz=offset_hz)
    n=round(rate*.008)
    if start+count*n>available:raise ValueError('short recording; no padding or concatenation permitted')
    # Validate physical passband before opening sample payloads.
    if rate!=20e6 or offset_hz!=0:
        if abs(offset_hz)+10e6>.45*rate:raise ValueError('view outside source guard band')
    images=[];offsets=[]
    for j in range(count):
        at=start+j*n
        image,meta=spectrogram(get(at,n),rate,config,return_metadata=True)
        if image.shape!=(256,625) or meta['selected_offset_hz']!=offset_hz:
            raise ValueError('unexpected temporal tile geometry or view')
        images.append(image);offsets.append(at)
    return np.stack(images),offsets,config.metadata()


def prepare(manifest_path,index,output,start=0,count=16,offset_hz=0.):
    manifest_path=Path(manifest_path)
    before_manifest=stamp(manifest_path);manifest_digest=sha256(manifest_path)
    with read_source(manifest_path) as f:manifest=json.load(f)
    if manifest.get('schema')!='rfmon.drone_ml.temporal_sources.v1':raise ValueError('invalid source manifest')
    rows=manifest['records']
    if type(index) is not int or not 0<=index<len(rows):raise ValueError('invalid record index')
    row=rows[index];check_record(row)
    path=Path(row['path']);before=stamp(path)
    if sha256(path)!=row['source_sha256']:raise ValueError('source hash mismatch')
    with reader(row) as (available,get):
        images,offsets,config=tiles_from_reader(get,available,int(row['sample_rate_hz']),start,count,offset_hz)
    if stamp(path)!=before or stamp(manifest_path)!=before_manifest:raise ValueError('inputs changed during preparation')
    out=new_output(output,[path.parent,manifest_path.parent])
    with (out/'tiles.npy').open('xb') as f:np.save(f,images,allow_pickle=False)
    meta=dict(schema='rfmon.drone_ml.temporal_preview.v1',source=row,
        source_manifest_sha256=manifest_digest,source_record_index=index,
        preprocessing=config,tensor_shape=list(images.shape),tensor_sha256=sha256(out/'tiles.npy'),
        native_start_sample=start,native_tile_offsets=offsets,native_samples_per_tile=round(row['sample_rate_hz']*.008),
        context_seconds=count*.008,fixed_offset_hz=offset_hz,source_stats_preserved=True,
        verified_presence_label=None,verified_identity_label=None,training_eligible=False,
        final_test_iq_read=False,live_qualified=False,
        continuity_scope='Offsets adjacent within this file; physical continuity inherits source provenance.',
        normalization='Per-tile source mean removal and unit RMS, matching existing preprocessing; no training normalization fitted.',
        limitations=['Preview only. Publisher context and energy activity do not establish window labels.',
                    'Fixed-view selection must be reviewed; stronger emitters can obscure a drone.'])
    write_new_json(out/'preview.json',meta)
    return meta


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest',type=Path,required=True);p.add_argument('--record',type=int,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--start-sample',type=int,default=0)
    p.add_argument('--tiles',type=int,default=16);p.add_argument('--offset-hz',type=float,default=0.)
    a=p.parse_args();r=prepare(a.manifest,a.record,a.output,a.start_sample,a.tiles,a.offset_hz)
    print(json.dumps(dict(context_seconds=r['context_seconds'],training_eligible=False,tensor_shape=r['tensor_shape'])))


if __name__=='__main__':main()
