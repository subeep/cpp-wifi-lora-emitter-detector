"""B1: bounded deployment-rate IQ views with locked-source guards and resume support.

No presence/model labels are asserted: metadata labels are recording context only.
Three spaced 8 ms intervals per admitted recording, with guarded overlapping RF views.
RFUAV uses the first 160 ms of each member to bound archive streaming, recorded explicitly.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import inspect
from fractions import Fraction
import hashlib
import io
import json
import math
import os
from pathlib import Path
import zipfile
import h5py
import numpy as np
from scipy.signal import resample_poly
from .common import read_source
from .drff import vector
from .expanded_corpus import rar_intervals, window_starts
from .universal_inventory import SourceGuard, locked_name, read_json
from .universal_classes import EXCLUDE

RATE, LENGTH = 20_000_000, 160_000
VERSION = 'universal_views_v2_kaiser8.6_100us_support'   # v2: no in-place shift of the source window


def offsets(rate):
    if not 20e6 <= rate <= 200e6 or int(rate) != rate: raise ValueError('unsupported source rate')
    if rate == RATE: return [0.]
    extent = .45*rate - RATE/2
    count = math.ceil(2*extent/16e6)+1
    if count % 2 == 0: count += 1
    return np.linspace(-extent, extent, count).tolist()


def deployment_view(iq, rate, offset):
    """Same Kaiser-8.6 polyphase resampler as preprocess.py; preserve physical amplitude."""
    allowed = offsets(rate)
    if not math.isfinite(offset) or abs(offset)>max(abs(x) for x in allowed)+.01:
        raise ValueError('frequency view outside guarded source band')
    pad = round(rate*.0001); expected = round(rate*.008)+2*pad
    # A copy: the shift below must never touch the caller's array. (v1 used np.asarray, which
    # returned the caller's complex128 window itself, so each offset of one window was applied on top
    # of the previous ones: views came in duplicate pairs at wrong centres for DRFF-R2, DroneRFb-DIR,
    # KU Leuven and Tampere.)
    x = np.array(iq,dtype=np.complex128)
    if x.ndim!=1 or len(x)!=expected or not np.isfinite(x).all(): raise ValueError('short/nonfinite window')
    if offset: x *= np.exp(-2j*np.pi*offset*np.arange(len(x))/rate)
    ratio = Fraction(RATE,int(rate))
    result = resample_poly(x,ratio.numerator,ratio.denominator,window=('kaiser',8.6))
    trim = round(RATE*.0001)
    result = np.asarray(result[trim:trim+LENGTH],dtype='<c8')
    if len(result)!=LENGTH: raise ValueError('short output view')
    return result


def eligible(inventory, classes, background):
    if classes.get('status')!='reviewed': raise ValueError('A3 class review required')
    known={r['model'] for r in classes['classes']}
    bg={str(Path(r['source']).resolve()):(part,r) for part,rows in background['partitions'].items() for r in rows}
    rows=[]
    for original in inventory['records']:
        if not original['readable'] or EXCLUDE.intersection(original['flags']) or original['model'] not in known: continue
        row=dict(original)
        if row['dataset']=='Own_X310' and row['model']=='not_a_drone':
            item=bg.get(str(Path(row['source']).resolve()))
            if item is None: continue
            part,meta=item; row.update(background_partition=part,format=meta['format'],samples=meta['samples'])
        if row['model']=='not_a_drone' and row['dataset']=='DRFF-R2':
            # Presence/scene review still needed; do not turn environment metadata into negatives.
            row['negative_label_approved']=False
        rows.append(row)
    return rows


class Reader:
    def __init__(self, root, rows):
        self.guard=SourceGuard(root)
        self.own={str(Path(r['source']).resolve()) for r in rows if r['dataset']=='Own_X310'}

    def check(self,row):
        path=Path(row['source']); resolved=path.resolve(strict=True)
        if locked_name(path) or locked_name(row.get('member') or '') or not row['readable']:
            raise PermissionError('locked recording')
        if row['dataset']=='Own_X310':
            if str(resolved) not in self.own: raise PermissionError('unlisted own capture')
            s=resolved.stat()
            if (s.st_dev,s.st_ino) in self.guard.locked_inodes: raise PermissionError('locked alias')
        else: self.guard.check(path,row.get('member'))
        return resolved

    def intervals(self,row):
        path=self.check(row);rate=int(row['rate_hz']);n=round(rate*.008);pad=round(rate*.0001)
        size=n+2*pad
        def yield_reader(total,read):
            for start in window_starts(int(total),n,3):
                yield start,read(start-pad,size)
        dataset=row['dataset']
        if dataset in ('DRFF-R2','DroneRFb-DIR'):
            with self.guard.open(path) as f,h5py.File(f,'r') as h:
                keys=('RF0_I','RF0_Q') if dataset=='DRFF-R2' else ('I','Q')
                i,q=(vector(h,k) for k in keys)
                if i.shape!=q.shape or len(i.shape)!=2 or min(i.shape)!=1: raise ValueError('unsupported HDF5 vectors')
                def read(start,count):
                    cut=lambda d:np.asarray(d[start:start+count,0] if d.shape[1]==1 else d[0,start:start+count],dtype=np.float64)
                    return cut(i)+1j*cut(q)
                yield from yield_reader(i.size,read)
        elif dataset=='KULeuven_DroneRF':
            with self.guard.open(path) as f,zipfile.ZipFile(f) as z:
                member=z.getinfo(row['member'])
                if member.file_size!=row['member_bytes'] or member.CRC!=row['member_crc32'] or member.file_size>512*1024**2:
                    raise ValueError('archive member changed or too large')
                raw=z.read(member)
            with h5py.File(io.BytesIO(raw),'r') as h:
                d=vector(h,'uhd_samps')
                if d.dtype.names!=('real','imag') or len(d.shape)!=2 or d.shape[0]!=1: raise ValueError('unsupported Leuven layout')
                def read(start,count):
                    v=d[0,start:start+count]
                    return v['real'].astype(np.float64)+1j*v['imag']
                yield from yield_reader(d.size,read)
        elif dataset=='RFUAV':
            total=round(rate*.16)
            starts=window_starts(total,n,3)
            for start,iq in rar_intervals(path,row['member'],[s-pad for s in starts],size):
                yield start+pad,iq
        elif dataset in ('Tampere_DroneRF','Own_X310'):
            sc16=dataset=='Tampere_DroneRF' or path.suffix=='.sc16'
            width=4 if sc16 else 8
            if path.stat().st_size%width: raise ValueError('partial raw IQ sample')
            with read_source(path) as f:
                def read(start,count):
                    f.seek(start*width);raw=f.read(count*width)
                    if len(raw)!=count*width: raise ValueError('short raw capture')
                    if not sc16:return np.frombuffer(raw,dtype='<c8')
                    x=np.frombuffer(raw,dtype='<i2').reshape(-1,2).astype(np.float64)
                    return (x[:,0]+1j*x[:,1])/32768.
                yield from yield_reader(path.stat().st_size//width,read)
        else: raise ValueError('unsupported source dataset')


def prepare(inventory_path,classes_path,background_path,root,output,smoke=False,workers=4,resume_code=None):
    if not 1<=workers<=8: raise ValueError('workers must be 1..8')
    inventory,classes,background=map(read_json,(inventory_path,classes_path,background_path))
    rows=eligible(inventory,classes,background)
    if smoke:
        selected={}
        for row in rows: selected.setdefault(row['dataset'],row)
        # Explicitly cover both native sc16 and float own-capture layouts.
        for row in rows:
            if row['dataset']=='Own_X310': selected.setdefault('Own_X310:'+Path(row['source']).suffix,row)
        rows=list({r['recording_id']:r for r in selected.values()}.values())
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    config=dict(version=VERSION,inputs={str(p):hashlib.sha256(Path(p).read_bytes()).hexdigest()
                                      for p in (inventory_path,classes_path,background_path)},smoke=smoke,
                selected_recordings=len(rows),intervals_per_recording=3,source_IQ_labels='recording_context_only',
                rfuav_sampling='three intervals within first 160 ms per IQ member',
                code_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                transform_sha256=hashlib.sha256((''.join(inspect.getsource(x) for x in (offsets,deployment_view,Reader))).encode()).hexdigest())
    config_path=output/'config.json'
    if config_path.exists():
        previous=read_json(config_path)
        if previous!=config:
            old_code=previous.pop('code_sha256',None)
            old_transform=previous.pop('transform_sha256',None)
            comparable={k:v for k,v in config.items() if k not in ('code_sha256','transform_sha256')}
            if previous!=comparable or (old_transform!=config['transform_sha256'] and old_code!=resume_code):
                raise ValueError('resume inputs/transform changed; explicit legacy scheduling migration required')
            (output/'scheduling_migration.json').write_text(json.dumps(dict(previous_code_sha256=old_code,
                new_code_sha256=config['code_sha256'],transform_sha256=config['transform_sha256'],workers=workers),indent=2)+'\n')
    config_path.write_text(json.dumps(config,indent=2)+'\n')
    reader=Reader(root,rows);done=[];errors=[];counts=Counter()
    def process_record(row):
        try:
            path=reader.check(row);s=path.stat();signature=[s.st_size,s.st_mtime_ns]
            metadata_path=output/(row['recording_id']+'.json');iq_path=metadata_path.with_suffix('.npy')
            if metadata_path.exists():
                meta=read_json(metadata_path)
                if meta['source_signature']!=signature or not iq_path.exists() or iq_path.stat().st_size!=meta['output_bytes']:
                    raise ValueError('resume source/output changed')
            else:
                views=[];windows=[]
                for start,iq in reader.intervals(row):
                    for offset in offsets(int(row['rate_hz'])):
                        views.append(deployment_view(iq,int(row['rate_hz']),offset))
                        windows.append(dict(source_start_sample=start,offset_hz=offset,center_hz=row['center_hz']+offset))
                if not views:raise ValueError('no views')
                stack=np.stack(views)
                with iq_path.with_suffix('.tmp').open('wb') as f:np.save(f,stack,allow_pickle=False)
                iq_path.with_suffix('.tmp').replace(iq_path)
                after=path.stat()
                if [after.st_size,after.st_mtime_ns]!=signature:raise ValueError('source changed during read')
                meta=dict(source_record=row,source_signature=signature,windows=windows,rate_hz=RATE,
                          samples_per_window=LENGTH,view_version=VERSION,output_bytes=iq_path.stat().st_size,
                          iq_sha256=hashlib.sha256(stack.tobytes()).hexdigest(),presence_labels_assigned=False)
                metadata_path.write_text(json.dumps(meta,indent=2)+'\n')
            return row,meta,None
        except (ValueError,OSError,TimeoutError,KeyError) as error:
            return row,None,dict(recording_id=row['recording_id'],source=row['source'],member=row.get('member'),error=str(error))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for index,(row,meta,error) in enumerate(pool.map(process_record,rows)):
            if error:errors.append(error)
            else:
                counts[row['model']]+=len(meta['windows']);done.append(row['recording_id'])
            if (index+1)%25==0 or index+1==len(rows):
                progress=dict(completed=len(done),attempted=index+1,total=len(rows),errors=len(errors),windows=sum(counts.values()))
                (output/'progress.json').write_text(json.dumps(progress,indent=2)+'\n');print(json.dumps(progress),flush=True)
    report=dict(schema='rfmon.drone_ml.universal_views.v1',config=config,complete=len(done)==len(rows),
                completed_recordings=len(done),errors=errors,windows_per_class=dict(sorted(counts.items())),
                total_windows=sum(counts.values()),window_store=str(output.resolve()))
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    if errors: raise RuntimeError(f'{len(errors)} source failures; see report.json')
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inputs',type=Path,required=True)
    p.add_argument('--root',type=Path,default=Path('/media/sudeep/Storage/datasets'))
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--smoke',action='store_true')
    p.add_argument('--workers',type=int,default=4)
    p.add_argument('--resume-code',help='explicit old code SHA-256 for a verified scheduling-only migration')
    a=p.parse_args()
    prepare(a.inputs/'inventory.json',a.inputs/'classes.json',a.inputs/'background_split.json',a.root,a.output,a.smoke,a.workers,a.resume_code)


if __name__=='__main__': main()
