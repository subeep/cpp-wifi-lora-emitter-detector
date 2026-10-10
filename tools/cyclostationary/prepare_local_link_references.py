#!/usr/bin/env python3
"""Read only selected Leuven/Tampere sources; write bounded derived IQ elsewhere.

Requires numpy and h5py. No publisher code is executed, no source is extracted
in place, and no other dataset directory is traversed.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import zipfile
import numpy as np

LEUVEN=('mini2vid.zip','mini2RC.zip','matricevid.zip','inspire2Vid.zip')
TAMPERE=('DJI_mavic_pro_2G.bin','DJI_inspire_2_2G.bin','Parrot_disco_2G.bin','Parrot_mambo_control_2G.bin')

def stamp(path):
    s=path.stat()
    return [s.st_dev,s.st_ino,s.st_size,s.st_atime_ns,s.st_mtime_ns,s.st_ctime_ns]

def open_source(path):
    # Fail closed if no-atime read cannot be granted; do not silently touch sources.
    return os.fdopen(os.open(path,os.O_RDONLY|os.O_NOATIME),'rb')

def read_complex_slice(d,offset,count):
    if (d.dtype.names != ('real','imag') or len(d.shape)!=2 or
        d.shape[0]!=1 or offset<0 or count<=0 or d.shape[1]<offset+count):
        raise ValueError('unsupported MATLAB complex vector layout or interval')
    v=d[0,offset:offset+count]
    if not np.isfinite(v['real']).all() or not np.isfinite(v['imag']).all():
        raise ValueError('nonfinite source IQ')
    iq=(v['real']+1j*v['imag']).astype('<c8')
    if not np.isfinite(iq).all():raise ValueError('nonfinite converted IQ')
    return iq.tobytes()


def read_ci16_slice(src,offset,count):
    if offset<0 or count<=0:raise ValueError('invalid sample interval')
    src.seek(offset*4)
    payload=src.read(count*4)
    if len(payload)!=count*4:raise ValueError('short source read')
    return payload


def prepare(root,output,start_seconds=0.0,selected_names=None):
    if not np.isfinite(start_seconds) or not 0 <= start_seconds <= .1:
        raise ValueError("start_seconds must be finite and within [0, 0.1]")
    if selected_names is not None and (not selected_names or not set(selected_names).issubset(LEUVEN+TAMPERE)):
        raise ValueError('source selection must use explicit Leuven/Tampere allowlist')
    root,output=root.resolve(),output.resolve()
    if output==root or root in output.parents:raise ValueError('outputs must be outside dataset root')
    output.mkdir(parents=True,exist_ok=False)
    records=[];skipped=[]
    for dataset,names in [('KULeuven_DroneRF',LEUVEN),('Tampere_DroneRF',TAMPERE)]:
        for name in names:
            if selected_names is not None and name not in selected_names:continue
            path=root/dataset/name
            if path.is_symlink() or path.resolve().parent!=(root/dataset).resolve():raise ValueError('unexpected source path')
            before=stamp(path);member=None;member_sha=None
            if dataset=='KULeuven_DroneRF':
                import h5py
                rate=100000000;fmt='cf32_le';count=10000000;offset=round(start_seconds*rate)
                with open_source(path) as src,zipfile.ZipFile(src) as z:
                    entries=sorted((x for x in z.infolist() if x.filename.endswith('.mat')),key=lambda x:x.filename)
                    if not entries:raise ValueError('no MAT members')
                    entry=entries[0];member=entry.filename
                    if entry.file_size>512*1024*1024:raise ValueError('MAT member exceeds budget')
                    raw=z.read(entry) # Full selected member read verifies ZIP CRC.
                member_sha=hashlib.sha256(raw).hexdigest()
                with h5py.File(io.BytesIO(raw),'r') as h:
                    d=h['uhd_samps']
                    if d.dtype.names!=('real','imag') or len(d.shape)!=2 or d.shape[0]!=1:
                        raise ValueError('unsupported MATLAB complex vector layout')
                    if d.shape[1]<offset+count:
                        if stamp(path)!=before:raise RuntimeError('source changed during short-interval inspection')
                        skipped.append(dict(source=str(path),source_stat_before=before,source_stat_after=stamp(path),
                            archive_member=member,archive_member_sha256=member_sha,archive_member_crc_verified=True,
                            requested_sample_offset=offset,requested_samples=count,available_samples=d.shape[1],
                            reason='requested_interval_exceeds_selected_member'))
                        print(name,'skipped: requested interval exceeds',d.shape[1],'samples',flush=True)
                        continue
                    payload=read_complex_slice(d,offset,count)
                del raw
            else:
                rate=120000000;fmt='ci16_le';count=12000000;offset=round(start_seconds*rate)
                if before[2]!=480000000:raise ValueError('unexpected/incomplete Tampere source size')
                with open_source(path) as src:payload=read_ci16_slice(src,offset,count)
            if stamp(path)!=before:raise RuntimeError('source changed during read: '+str(path))
            dest=output/(dataset+'_'+Path(name).stem+('.cf32' if fmt=='cf32_le' else '.ci16'))
            dest.write_bytes(payload)
            records.append({'source':str(path),'source_stat_before':before,'source_stat_after':stamp(path),
                'archive_member':member,'archive_member_sha256':member_sha,'archive_member_crc_verified':member is not None,
                'full_source_checksum_verified':False,'path':str(dest),'sha256':hashlib.sha256(payload).hexdigest(),
                'format':fmt,'samples':count,'sample_rate_hz':rate,'capture_center_hz':2440000000,
                'source_sample_offset':offset,'duration_seconds':.1,'partition':'development_only',
                'split_group':('kuleuven:mini2_unit_session_unknown' if name.startswith('mini2') else dataset+':'+Path(name).stem),'unit_and_session_independence_verified':False,
                'link_family_truth':None,'continuity':'publisher_recording_not_independently_verified',
                'role_label':name,'label_scope':'publisher_filename_context_not_signal_classification'})
            del payload
            (output/'manifest.partial.json').write_text(json.dumps({'schema':'local_link_references.v1','complete':False,'records':records,'skipped':skipped},indent=2)+'\n')
            print(name,'prepared',flush=True)
    for record in records+skipped:
        if stamp(Path(record['source']))!=record['source_stat_before']:raise RuntimeError('source changed before import completion')
    (output/'manifest.json').write_text(json.dumps({'schema':'local_link_references.v1','records':records,'skipped':skipped},indent=2)+'\n')
    return records

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--start-seconds',type=float,default=0.0,help='start of the 100 ms derived interval, from 0 to 0.1 seconds')
    p.add_argument('--sources',nargs='+',choices=LEUVEN+TAMPERE,help='explicit subset of permitted source files')
    a=p.parse_args();prepare(a.root,a.output,a.start_seconds,a.sources)
