"""Prepare sharded STFT data and physical IQ augmentation, without training."""
import argparse
from collections import Counter
import hashlib
import io
import json
import os
from pathlib import Path
import zipfile
import h5py
import numpy as np
from .activity import POLICY, assess
from .common import new_output, read_source, sha256, stamp, write_json
from .corpus_sources import rar_bytes
from .drff import interval, vector
from .preprocess import STFTConfig, select_offset, spectrogram

CONFIG = STFTConfig(duration_seconds=.008, hop=256, auto_frequency_view=True)
DEVELOPMENT = {'train','calibration','unknown_calibration'}


def augment_iq(iq, rate, seed, partition):
    if partition != 'train': raise ValueError('augmentation is training-only')
    rng=np.random.default_rng(seed)
    offset=float(rng.uniform(-100000,100000))
    delay=max(1,round(rate*float(rng.uniform(30e-9,150e-9))))
    echo=float(rng.uniform(.1,.35))*np.exp(1j*rng.uniform(-np.pi,np.pi))
    x=np.asarray(iq,dtype=np.complex128).copy()
    x[delay:]+=echo*np.asarray(iq[:-delay])  # zero-padded echo, no circular wrap
    x*=np.exp(2j*np.pi*offset*np.arange(len(x))/rate)
    ratio_db=float(rng.uniform(10,30))
    noise_rms=np.sqrt(np.mean(np.abs(x)**2))*10**(-ratio_db/20)
    x+=noise_rms/np.sqrt(2)*(rng.normal(size=len(x))+1j*rng.normal(size=len(x)))
    return x,dict(seed=seed,cfo_hz=offset,echo_delay_samples=delay,
                  echo_real=float(echo.real),echo_imag=float(echo.imag),
                  observed_power_to_added_noise_db=ratio_db,ground_truth_snr=None)


def window_starts(total,n,windows):
    if type(total) is not int or type(n) is not int or type(windows) is not int or n<=0 or windows<1:
        raise ValueError('invalid window geometry')
    if total<3*n: raise ValueError('recording too short')
    count=min(windows,(total-3*n)//n+1)
    a=np.unique(np.linspace(n,total-2*n,count,dtype=np.int64))
    if len(a)>1 and np.min(np.diff(a))<n: raise ValueError('requested windows overlap')
    return [int(x) for x in a]


def source_windows(row, windows=12):
    """Yield bounded, spaced windows. No joining records or sweep dwell intervals."""
    path=Path(row['source']);n=round(row['rate_hz']*CONFIG.duration_seconds)
    if not 1<=n<=4_000_000 or not os.statvfs(path).f_flag & os.ST_RDONLY or stamp(path)!=row['source_stat']:
        raise ValueError('source changed, writable or interval exceeds budget')
    def starts(total):
        return window_starts(total,n,windows)
    adapter=row['adapter']; extra={}
    if adapter=='drff':
        with read_source(path) as f,h5py.File(f,'r') as h:
            for start in starts(row['total_samples']):
                yield start,interval(h,start,n),extra
    elif adapter=='ci16':
        with read_source(path) as f:
            for start in starts(row['total_samples']):
                f.seek(start*4);raw=f.read(n*4)
                if len(raw)!=n*4: raise ValueError('short IQ read')
                v=np.frombuffer(raw,dtype='<i2').reshape(-1,2)
                clip=float(np.mean((v==32767)|(v==-32768)))
                yield start,v[:,0].astype(np.float64)+1j*v[:,1],dict(adc_rail_fraction=clip)
    elif adapter=='leuven':
        with read_source(path) as f,zipfile.ZipFile(f) as z:
            info=z.getinfo(row['member'])
            if info.file_size!=row['member_bytes'] or info.CRC!=row['member_crc32'] or info.file_size>512*1024*1024:
                raise ValueError('archive member changed')
            raw=z.read(info) # CRC verified for the complete selected member
        extra=dict(member_sha256=hashlib.sha256(raw).hexdigest(),member_crc_verified=True)
        with h5py.File(io.BytesIO(raw),'r') as h:
            d=vector(h,'uhd_samps')
            if d.dtype.names!=('real','imag') or len(d.shape)!=2 or d.shape[0]!=1:
                raise ValueError('unsupported Leuven complex layout')
            for start in starts(d.shape[1]):
                v=d[0,start:start+n]
                yield start,v['real'].astype(np.float64)+1j*v['imag'],extra
    elif adapter=='rfuav':
        # Read a bounded prefix once, with enough spacing for twelve 8 ms intervals.
        count=min(row['total_samples'],round(row['rate_hz']*.16))
        raw=rar_bytes(path,row['member'],limit=count*8,prefix=True)
        iq=np.frombuffer(raw,dtype='<c8')
        extra=dict(prefix_sha256=hashlib.sha256(raw).hexdigest(),prefix_samples=count,member_crc_verified=False)
        for start in starts(count): yield start,iq[start:start+n],extra
    else: raise ValueError('unknown source adapter')
    if stamp(path)!=row['source_stat']: raise ValueError('source changed during extraction')


class ShardWriter:
    def __init__(self,root):
        self.root=root;self.images=[];self.rows=[];self.shards=[];self.pending=[]
    def append(self,image,row):
        row=dict(row,index=len(self.rows),shard=f'shard_{len(self.shards):04d}.npy',shard_index=len(self.images))
        self.images.append(image);self.rows.append(row)
        if len(self.images)==32: self.flush()
    def flush(self):
        if not self.images:return
        name=f'shard_{len(self.shards):04d}.npy';path=self.root/name
        np.save(path,np.stack(self.images))
        self.shards.append(dict(path=name,sha256=sha256(path),shape=[len(self.images),CONFIG.nfft,625],bytes=path.stat().st_size))
        self.images=[]


def prepare(review_path,output,windows=12):
    if not 4<=windows<=24: raise ValueError('windows must be 4..24')
    review_path=Path(review_path).resolve();review=json.loads(review_path.read_text())
    if review['schema']!='rfmon.drone_ml.expanded_review.v1': raise ValueError('invalid review')
    split_path=review_path.parent/'split_plan.json'
    if sha256(split_path)!=review['split_sha256']: raise ValueError('split changed')
    plan=json.loads(split_path.read_text())
    rows=[r for r in review['records'] if r['partition'] in DEVELOPMENT]
    if any(plan['groups'][r['split_group']]['partition']!=r['partition'] for r in review['records']):
        raise ValueError('review partition mismatch')
    out=new_output(output,[review['source_root'],review_path.parent]);writer=ShardWriter(out)
    rejected=[];seen={};sums=[0.,0.,0];source_counts=Counter()
    def add(image,row):
        writer.append(image,row)
        if row['partition']=='train' and row['model_eligible']:
            sums[0]+=float(np.sum(image,dtype=np.float64));sums[1]+=float(np.sum(image.astype(np.float64)**2));sums[2]+=image.size
    for position,r in enumerate(rows):
        for start,iq,extra in source_windows(r,windows):
            if not np.isfinite(iq).all():
                rejected.append(dict(recording_id=r['recording_id'],start=start,reason='nonfinite_IQ'));continue
            rawhash=hashlib.sha256(np.asarray(iq,dtype='<c8').tobytes()).hexdigest()
            if rawhash in seen:
                prev=seen[rawhash]
                if (prev['partition'],prev['label'])!=(r['partition'],r['label']):
                    raise ValueError('duplicate interval crosses partitions or labels')
                rejected.append(dict(recording_id=r['recording_id'],start=start,reason='duplicate_selected_interval'));continue
            seen[rawhash]=r
            if extra.get('adc_rail_fraction',0)>.001:
                rejected.append(dict(recording_id=r['recording_id'],start=start,reason='ADC_rail_fraction_above_0.001'));continue
            try: image=spectrogram(iq,r['rate_hz'],CONFIG)
            except ValueError as e:
                rejected.append(dict(recording_id=r['recording_id'],start=start,reason=str(e)));continue
            quality=assess(image)
            rec=dict(recording_id=r['recording_id'],source=r['source'],member=r.get('member'),dataset=r['dataset'],
                split_group=r['split_group'],label=r['label'],partition=r['partition'],
                offset_samples=start,samples=len(iq),source_rate_hz=r['rate_hz'],source_center_hz=r['center_hz'],
                selected_offset_hz=select_offset(iq,r['rate_hz'],CONFIG),interval_sha256=rawhash,
                label_scope=r['label_scope'],role=r['role'],validation_support=r['validation_support'],
                quality=quality,model_eligible=quality['eligible'],augmentation=None,source_integrity=extra,
                clipping_status='ADC_rails_checked' if r['adapter']=='ci16' else 'float_scale_unknown_not_certified_unclipped')
            add(image,rec);source_counts[r['dataset']]+=1
            if r['partition']=='train' and quality['eligible']:
                seed=int(hashlib.sha256((r['recording_id']+':'+str(start)+':v1').encode()).hexdigest()[:8],16)
                augmented,params=augment_iq(iq,r['rate_hz'],seed,'train')
                augimage=spectrogram(augmented,r['rate_hz'],CONFIG);q=assess(augimage)
                add(augimage,dict(rec,augmentation=params,quality=q,model_eligible=q['eligible'],
                    selected_offset_hz=select_offset(augmented,r['rate_hz'],CONFIG)))
        if (position+1)%10==0: print(json.dumps(dict(sources=position+1,total_sources=len(rows),tensors=len(writer.rows))),flush=True)
    writer.flush()
    if sums[2]==0:raise ValueError('no eligible training examples')
    if any(stamp(r['source'])!=r['source_stat'] for r in rows):raise ValueError('source statistics changed')
    mean=sums[0]/sums[2];scale=np.sqrt(max(0,sums[1]/sums[2]-mean**2))
    if not np.isfinite(scale) or scale<1e-6:raise ValueError('training variance invalid')
    classes=sorted({r['label'] for r in writer.rows if r['partition']=='train' and r['model_eligible']})
    expected=sorted({r['label'] for r in rows if r['partition']=='train'})
    if classes!=expected:raise ValueError('activity filter removed an entire training class')
    manifest=dict(schema='rfmon.drone_ml.sharded_corpus.v1',records=writer.rows,shards=writer.shards,
        classes=classes,preprocessing=CONFIG.metadata(),activity_policy=POLICY,
        normalization=dict(mean=mean,scale=float(scale),scope='eligible_train_original_and_augmented_only'),
        review_sha256=sha256(review_path),split_sha256=sha256(split_path),
        source_stats_preserved=True,held_test_iq_read=False,release_validation_eligible=False,
        negative_truth_available=False,rejected=rejected,
        original_by_dataset=dict(source_counts),windows_per_source=windows)
    write_json(out/'corpus.json',manifest)
    return manifest


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--review',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--windows-per-source',type=int,default=12)
    a=p.parse_args();r=prepare(a.review,a.output,a.windows_per_source)
    print(json.dumps(dict(classes=r['classes'],tensors=len(r['records']),shards=len(r['shards']))))
if __name__=='__main__':main()
