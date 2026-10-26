"""Prepare unlabelled numerical review snippets, never a training manifest.

Include deterministic uniform samples as well as proposal-selected samples so
review is not limited to the detector's successes. Preserve recording context
separately from per-region truth. No report boxes or reserved IQ are used.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from scipy import fft

from .common import new_output, read_source, sha256, stamp, write_json
from .temporal_audit import check_record, reader


def selections(record, rate, count):
    available = record['analysed_samples']
    if available < count:
        raise ValueError('insufficient observed source for a full review snippet')
    row = record['source']
    key = row['source_sha256']+'\0'+row.get('dataset_key','')+'\0uniform-review-v1'
    uniform = int(hashlib.sha256(key.encode()).hexdigest()[:16],16) % (available-count+1)
    picked = [dict(start_sample=uniform, samples=count, reasons=['deterministic_uniform'], proposal_ids=[])]
    for name, predicate in [
        ('largest_transient', lambda r:any(b.startswith('transient') for b in r.get('branches',[]))),
        ('stable_ambiguous', lambda r:r.get('stable_background_ambiguity',False))]:
        options = [r for r in record['regions'] if predicate(r)]
        if not options:
            continue
        proposal = max(options,key=lambda r:(r['stop_sample']-r['start_sample'])*(r['high_hz']-r['low_hz']))
        start = max(0,min((proposal['start_sample']+proposal['stop_sample']-count)//2,available-count))
        found = next((p for p in picked if p['start_sample']==start),None)
        if found:
            found['reasons'].append(name); found['proposal_ids'].append(proposal['id'])
        else:
            picked.append(dict(start_sample=start,samples=count,reasons=[name],proposal_ids=[proposal['id']]))
    return picked


def build(report_paths, output, seconds=.016):
    if not np.isfinite(seconds) or not .001<=seconds<=.02:
        raise ValueError('review snippet must be 1 to 20 ms')
    paths = list(map(Path,report_paths))
    if not 1<=len(paths)<=8:
        raise ValueError('report count budget')
    before={p:stamp(p) for p in paths}; digests={str(p):sha256(p) for p in paths}; rows=[]
    for path in paths:
        with read_source(path) as f:report=json.load(f)
        if report.get('schema')!='rfmon.drone_ml.localizer_audit.v1' or report.get('algorithm')!='multiscale':
            raise ValueError('requires completed multiscale audit')
        for record in report['records']:
            check_record(record['source'])  # Before source path access or output creation.
            rows.append((path,record))
    if not 1<=len(rows)<=32:
        raise ValueError('source record count budget')
    out=new_output(output,[p.parent for p in paths]+[Path(r['source']['path']).parent for _,r in rows])
    hashes={}; entries=[]
    for index,(report_path,record) in enumerate(rows):
        row=record['source']; path=Path(row['path']).resolve(); rate=int(row['sample_rate_hz'])
        if path not in hashes:
            before[path]=stamp(path); hashes[path]=sha256(path)
        if hashes[path]!=row['source_sha256']:
            raise ValueError('review source checksum mismatch')
        count=round(rate*seconds)
        if count>4_000_000:
            raise ValueError('review read budget')
        picked=selections(record,rate,count)
        with reader(row) as (available,get):
            if available<record['analysed_samples']:
                raise ValueError('audit exceeds available source')
            for pick in picked:
                x=get(pick['start_sample'],count)
                if not np.isfinite(x).all():
                    raise ValueError('nonfinite review IQ')
                iq_hash=hashlib.sha256(np.asarray(x,dtype='<c8').tobytes()).hexdigest()
                # Physical native grid, no resizing/resampling/automatic frequency view.
                frames=np.lib.stride_tricks.sliding_window_view(x.astype(np.complex64),1024)[::512]
                transform=fft.fftshift(fft.fft(frames*np.hanning(1025)[:-1].astype(np.float32),axis=1,workers=1),axes=1)
                power=transform.real**2+transform.imag**2
                image=10*np.log10(np.maximum(power,1e-30))
                name=f'review_{len(entries):03d}.npz'
                with (out/name).open('xb') as f:
                    np.savez_compressed(f,log_power_db=image.T,
                        frequency_offset_hz=fft.fftshift(fft.fftfreq(1024,1/rate)),
                        frame_center_time_seconds=(pick['start_sample']+np.arange(len(frames))*512+512)/rate)
                nearby=[r for r in record['regions'] if r['start_sample']<pick['start_sample']+count and r['stop_sample']>pick['start_sample']]
                entries.append(dict(id=f'review_{len(entries):03d}',source=row,source_report=str(report_path),
                    selection=pick,source_record_index=index,tensor_file=name,tensor_sha256=sha256(out/name),
                    source_iq_sha256=iq_hash,nfft=1024,hop=512,shape=list(image.T.shape),
                    source_sample_stop=pick['start_sample']+count,
                    proposals_for_reference_only=nearby,
                    proposals_truncated_by_snippet=[r['id'] for r in nearby if r['start_sample']<pick['start_sample'] or r['stop_sample']>pick['start_sample']+count],
                    review_status='unreviewed',verified_presence_label=None,verified_identity_label=None,
                    reviewed_parent_regions=[],reviewed_internal_features=[],training_eligible=False,
                    note='Source publisher context is not per-emitter truth. Native STFT is a review view, not a frozen model preprocessing profile.'))
        write_json(out/'progress.json',dict(source_records_completed=index+1,source_records=len(rows),snippets=len(entries)))
    if any(stamp(p)!=s for p,s in before.items()):
        raise ValueError('review inputs changed')
    manifest=dict(schema='rfmon.drone_ml.unlabelled_localizer_review.v1',input_report_sha256=digests,
        records=entries,source_stats_preserved=True,source_hashes_verified=True,final_test_iq_read=False,
        training_eligible=False,labels_created=False,
        review_instructions=[
            'Review a parent transmission separately from any internal feature; do not infer packets or ZC from appearance alone.',
            'Source drone-on context does not identify every region; unknown emitter attribution must remain null.',
            'Uniform selection can contain activity or misses; it is not a negative label.',
            'Keep truncated regions and gaps explicit. Sixteen milliseconds here is a review convenience, not a chosen model context.',
            'Record evidence, reviewer and source scope before any future versioned training admission.'])
    write_json(out/'manifest.json',manifest)
    return manifest


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reports',type=Path,nargs='+',required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--seconds',type=float,default=.016);a=p.parse_args()
    r=build(a.reports,a.output,a.seconds);print(json.dumps(dict(snippets=len(r['records']),training_eligible=False)))


if __name__=='__main__':main()
