#!/usr/bin/env python3
"""Read-only, bounded DRFF/RFUAV development import. No source code execution.

Only explicit selections below are read. Source filesystem must be read-only.
Dataset/device labels are context, never link truth or certified negatives.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import selectors
import subprocess
import time
import xml.etree.ElementTree as ET
import numpy as np
from prepare_local_link_references import open_source, stamp

DRFF = (
 'dataset7-environment/indoor_environment.mat',
 'dataset7-environment/outdoor_environment.mat',
 'dataset6-wifi_mixed/mavic3C_1_c1.mat',
 'dataset6-wifi_mixed/mini4PRO_1_c1.mat',
 'dataset1-single_drone_states/mavic3C_1_Ascend_c17_u1_d2.mat',
 'dataset1-single_drone_states/mini4PRO_1_Ascend_c17_u1_d2.mat',
)
RFUAV = (
 ('DJI MINI4 PRO','VTSBW=20/pack2'),
 ('DJI AVATA2','VTSBW=20/pack2'),
 ('FRSKY X14','pack1'),
)
FORMAT_SOURCE='https://github.com/kitoweeknd/RFUAV/blob/df72beaef99928ed79b8f49163a8be569741c417/graphic/RawDataProcessor.py'


def scalar(value):
    a=np.asarray(value)
    if a.size!=1:raise ValueError('metadata must contain a scalar')
    n=float(a.reshape(-1)[0])
    if not math.isfinite(n) or n<=0:raise ValueError('invalid positive metadata')
    return n


def matlab_text(value):
    a=np.asarray(value)
    if a.size>256 or a.dtype.kind not in 'ui':raise ValueError('unsupported MATLAB text')
    return ''.join(chr(int(x)) for x in a.reshape(-1)).rstrip('\0')


def label_conflicts(dataset,filename,metadata_target):
    normalize=lambda s:''.join(c.lower() for c in s if c.isalnum())
    if dataset=='DRFF-R2':
        if 'dataset7-environment/' in filename:
            return ['environment_folder_has_drone_target_metadata'] if metadata_target else ['environment_absence_unverified']
        claimed=Path(filename).stem.split('_')[0]
        target=metadata_target.split('_')[0]
    else:claimed=filename;target=metadata_target
    return [] if normalize(claimed)==normalize(target) else ['filename_and_target_metadata_disagree']


def complex_prefix(i,q,count):
    if i.shape!=q.shape or len(i.shape)!=2 or 1 not in i.shape or count<=0 or count>20000000:
        raise ValueError('unsupported IQ shape or sample budget')
    if max(i.shape)<count:raise ValueError('requested interval exceeds recording')
    sl=(slice(0,count),0) if i.shape[1]==1 else (0,slice(0,count))
    x=np.asarray(i[sl],dtype=np.float32)+1j*np.asarray(q[sl],dtype=np.float32)
    if not np.isfinite(x).all():raise ValueError('nonfinite IQ')
    return x.astype('<c8').tobytes()


def command_bytes(command,limit,exact_prefix=False,timeout=120):
    """Bound subprocess output and time; deliberately stop IQ decompression early."""
    if not 0<limit<=160000000:raise ValueError('output byte budget')
    p=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
    output=bytearray();deadline=time.monotonic()+timeout
    try:
        with selectors.DefaultSelector() as sel:
            sel.register(p.stdout,selectors.EVENT_READ)
            while len(output)<limit:
                left=deadline-time.monotonic()
                if left<=0:raise TimeoutError('bounded archive read timed out')
                if not sel.select(min(left,1)):continue
                block=os.read(p.stdout.fileno(),min(65536,limit-len(output)))
                if not block:break
                output.extend(block)
        if exact_prefix:
            if len(output)!=limit:raise ValueError('short archive IQ prefix')
        else:
            if len(output)==limit:raise ValueError('metadata exceeds budget')
            if p.wait(timeout=max(.01,deadline-time.monotonic()))!=0:raise ValueError('archive metadata read failed')
        return bytes(output)
    finally:
        if p.poll() is None:
            p.terminate()
            try:p.wait(timeout=3)
            except subprocess.TimeoutExpired:p.kill();p.wait()
        p.stdout.close()


def xml_metadata(payload):
    if b'<!DOCTYPE' in payload.upper() or b'<!ENTITY' in payload.upper():raise ValueError('XML entities forbidden')
    root=ET.fromstring(payload)
    if root.tag!='SignalHoundIQFile' or root.findtext('DataType')!='Complex Float':raise ValueError('unsupported RFUAV format')
    fields={k:root.findtext(k) for k in ['Drone','DeviceType','SerialNumber','IQFileName','DataType']}
    for key in ['SampleRate','CenterFrequency','SampleCount','IFBandwidth']:fields[key]=scalar(root.findtext(key))
    if not fields['Drone']:raise ValueError('missing recording context')
    return fields


def checked_source(root,rel):
    p=root/rel
    if p.is_symlink() or root not in p.resolve().parents or not p.is_file():raise ValueError('source escapes root or is not regular')
    if not os.statvfs(p).f_flag & os.ST_RDONLY:raise ValueError('extended import requires read-only source filesystem')
    return p


def prepare(root,output):
    root=root.resolve();output=output.resolve()
    if output==root or root in output.parents:raise ValueError('outputs must be outside source root')
    output.mkdir(parents=True,exist_ok=False)
    import h5py
    records=[]
    def save(path,before,payload,rate,center,metadata,conflicts,dataset,member=None):
        if not 0<rate<=200e6 or not 0<center<=10e9:raise ValueError('capture metadata outside import budget')
        if stamp(path)!=before:raise ValueError('source changed during import')
        count=len(payload)//8
        if len(payload)%8 or count!=round(rate*.1) or not np.isfinite(np.frombuffer(payload,dtype='<c8')).all():raise ValueError('invalid derived IQ interval')
        name=f'{len(records):02d}_{dataset}_{path.stem.replace(" ","_")}.cf32'
        dest=output/name;dest.write_bytes(payload)
        records.append(dict(source=str(path),source_stat_before=before,source_stat_after=stamp(path),
            path=str(dest),sha256=hashlib.sha256(payload).hexdigest(),format='cf32_le',samples=count,
            sample_rate_hz=rate,capture_center_hz=center,source_sample_offset=0,duration_seconds=count/rate,
            partition='development_only',split_group=dataset+':'+('unit_session_unverified' if dataset=='DRFF-R2' else path.stem),
            unit_and_session_independence_verified=False,link_family_truth=None,
            continuity='publisher_recording_not_independently_verified',metadata=metadata,
            label_conflicts=conflicts,negative_ground_truth_eligible=False,release_validation_eligible=False,
            archive_member=member,full_source_checksum_verified=False,archive_iq_crc_verified=False))
        (output/'manifest.partial.json').write_text(json.dumps(dict(schema='local_link_references.v1',complete=False,records=records),indent=2)+'\n')
        print(name,'prepared',conflicts,flush=True)
    for rel in DRFF:
        path=checked_source(root,'DRFF-R2/DRFF-R2/'+rel);before=stamp(path)
        with open_source(path) as stream,h5py.File(stream,'r') as h:
            rate=scalar(h['Fs'][()])
            center_keys=[k for k in ['CenterFrequence','CenterFreq'] if k in h]
            if not center_keys:raise ValueError('missing centre frequency')
            centers=[scalar(h[k][()]) for k in center_keys]
            if len(set(centers))!=1:raise ValueError('conflicting centre metadata')
            center=centers[0]
            if rate>200e6:raise ValueError('rate exceeds pilot budget')
            metadata=dict(target=matlab_text(h['TD'][()]) if 'TD' in h else '',state=matlab_text(h['State'][()]) if 'State' in h else '',
                rate_source='embedded_Fs',center_source='embedded_'+center_keys[0],
                total_samples=max(h['RF0_I'].shape),filename_context=rel)
            payload=complex_prefix(h['RF0_I'],h['RF0_Q'],round(rate*.1))
        save(path,before,payload,rate,center,metadata,label_conflicts('DRFF-R2',rel,metadata['target']),'DRFF-R2')
    for name,pack in RFUAV:
        path=checked_source(root,'RFUAV/'+name+'.rar');before=stamp(path)
        xml_member=name+'/'+pack+'.xml';iq_member=name+'/'+pack+'_0-1s.iq'
        xml=command_bytes(['unrar','p','-inul',str(path),xml_member],65536)
        metadata=xml_metadata(xml);rate=metadata['SampleRate'];center=metadata['CenterFrequency']
        if rate>200e6 or metadata['SampleCount']<round(rate*.1):raise ValueError('unsupported RFUAV sample count/rate')
        metadata.update(xml_member=xml_member,xml_sha256=hashlib.sha256(xml).hexdigest(),format_source=FORMAT_SOURCE,
            byte_order_scope='little_endian_interpretation_not_explicit_in_XML',scale_factor_applied=False)
        payload=command_bytes(['unrar','p','-inul',str(path),iq_member],round(rate*.1)*8,exact_prefix=True)
        save(path,before,payload,rate,center,metadata,label_conflicts('RFUAV',name,metadata['Drone']),'RFUAV',iq_member)
    for r in records:
        if stamp(Path(r['source']))!=r['source_stat_before']:raise ValueError('source changed before completion')
    (output/'manifest.json').write_text(json.dumps(dict(schema='local_link_references.v1',records=records,
        source_filesystem_read_only=True,reader_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()),indent=2)+'\n')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();prepare(a.root,a.output)
