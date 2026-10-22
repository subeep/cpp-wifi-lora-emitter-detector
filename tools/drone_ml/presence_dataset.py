"""Review-gated binary presence corpus from exact live audit intervals.

Neither an operator context nor a model prediction is a training label. Final-test
rows are described in the review, but their IQ is never read by preparation.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path

import numpy as np

from .common import new_output, sha256
from .input_audit import read_interval, read_json
from .live_audit import write_new_json
from .preprocess import STFTConfig, spectrogram

CONFIG = STFTConfig(duration_seconds=.008, hop=256, auto_frequency_view=True)
PARTITIONS = ('train', 'calibration', 'development')
LABELS = ('background', 'drone')


def template(root):
    rows=[]
    sessions=sorted(Path(root).glob('*/session.json'))
    if len(sessions)>1000:
        raise ValueError('inventory exceeds 1000 sessions; select a smaller collection')
    for session_path in sessions:
        session=read_json(session_path)
        if session.get('schema')!='rfmon.drone_ml.live_audit.v1':
            raise ValueError('unsupported session')
        for metadata in sorted(session_path.parent.glob('[0-9][0-9][0-9][0-9].json')):
            item=read_json(metadata)
            rows.append(dict(metadata=str(metadata.resolve()), metadata_sha256=sha256(metadata),
                iq_sha256=item['iq_sha256'], recorder_session=session['session_id'],
                operator_context=session['operator_context'], source_run=item['request'].get('run_id'),
                suggested_view_offset_hz=item['result'].get('selected_offset_hz'),
                partition='excluded', label=None, reviewed=False, collection_group=None,
                label_evidence='', independence_evidence='', drone_model=None,
                verified_view_offset_hz=None))
    return dict(schema='rfmon.drone_ml.presence_review.v1', records=rows,
                instructions='Review the exact selected view. Merge same-collection on/off sessions into one collection_group. '
                'Choose background/drone only with evidence; uncertain rows stay excluded. '
                'Reserve final_test by metadata without opening its IQ. Need >=2 independent groups per label '
                'in each of train, calibration and development. Context labels are not window truth.')


def readiness(review):
    if review.get('schema')!='rfmon.drone_ml.presence_review.v1':
        raise ValueError('unsupported presence review')
    rows=review.get('records',[])
    if not isinstance(rows,list) or len(rows)>10000:
        raise ValueError('invalid or oversized review')
    blockers=[]; groups=defaultdict(set); partitions_by_key={}; collection_by_source={}
    for i,row in enumerate(rows):
        partition=row.get('partition')
        if partition=='excluded':continue
        if partition not in (*PARTITIONS,'final_test'):
            blockers.append(f'row {i}: unsupported partition');continue
        if (row.get('reviewed') is not True or row.get('label') not in LABELS
                or not row.get('label_evidence') or not row.get('independence_evidence')
                or not row.get('collection_group') or not row.get('recorder_session')
                or not row.get('source_run') or not row.get('metadata_sha256') or not row.get('iq_sha256')
                or row.get('verified_view_offset_hz') is None
                or (row.get('label')=='drone' and not row.get('drone_model'))):
            blockers.append(f'row {i}: missing verified label, view, grouping or evidence');continue
        if not isinstance(row['verified_view_offset_hz'],(int,float)) or not np.isfinite(row['verified_view_offset_hz']):
            blockers.append(f'row {i}: invalid frequency view');continue
        for field in ('collection_group','recorder_session','source_run','metadata','iq_sha256'):
            key=(field,row.get(field))
            if key in partitions_by_key and partitions_by_key[key]!=partition:
                blockers.append(f'row {i}: {field} crosses partitions')
            partitions_by_key[key]=partition
        for field in ('recorder_session','source_run'):
            key=(field,row[field])
            if key in collection_by_source and collection_by_source[key]!=row['collection_group']:
                blockers.append(f'row {i}: {field} was split into falsely independent collection groups')
            collection_by_source[key]=row['collection_group']
        groups[(partition,row['label'])].add(row['collection_group'])
    for partition in PARTITIONS:
        for label in LABELS:
            if len(groups[partition,label])<2:
                blockers.append(f'{partition}/{label}: fewer than two reviewed independent collection groups')
    return dict(schema='rfmon.drone_ml.presence_readiness.v1',ready=not blockers,
                blockers=blockers, rows=len(rows),
                groups={f'{p}/{label}':len(groups[p,label]) for p in (*PARTITIONS,'final_test') for label in LABELS},
                final_test_iq_read=False, live_qualified=False)


def prepare(review_path,output):
    review_digest=sha256(review_path)
    review=read_json(review_path)
    status=readiness(review)
    if not status['ready']:
        raise ValueError('presence training blocked: '+'; '.join(status['blockers']))
    selected=[row for row in review['records'] if row['partition'] in PARTITIONS]
    sources=[Path(row['metadata']).resolve().parent for row in review['records'] if row.get('metadata')]
    out=new_output(output,sources)
    records=[]
    seen=set()
    for row in selected:
        metadata=Path(row['metadata']).resolve()
        if sha256(metadata)!=row['metadata_sha256']:
            raise ValueError('metadata changed since review')
        item=read_json(metadata)
        session=read_json(metadata.parent/'session.json')
        request=item['request']
        if (item.get('schema')!='rfmon.drone_ml.audit_interval.v1'
                or session.get('schema')!='rfmon.drone_ml.live_audit.v1'
                or session['session_id']!=row['recorder_session']
                or request.get('run_id')!=row['source_run']
                or request.get('source')!='live'
                or request.get('contiguous') not in (True,'true','1')):
            raise ValueError('requires reviewed live contiguous capture provenance')
        iq_path=(metadata.parent/item['iq_file']).resolve()
        if iq_path.parent!=metadata.parent or iq_path.name!=metadata.stem+'.cf32':
            raise ValueError('invalid IQ path')
        rate=float(request['sample_rate_hz'])
        iq,digest=read_interval(iq_path,rate,0)
        if digest!=item['iq_sha256'] or digest!=row['iq_sha256'] or iq_path.stat().st_size!=iq.nbytes:
            raise ValueError('IQ integrity mismatch')
        if digest in seen:
            raise ValueError('duplicate IQ intervals must not overweight training/evaluation')
        seen.add(digest)
        image,meta=spectrogram(iq,rate,CONFIG,return_metadata=True)
        if abs(meta['selected_offset_hz']-row['verified_view_offset_hz'])>1e-6:
            raise ValueError('prepared frequency view differs from reviewed view')
        filename=f'{len(records):06d}.npy'
        with (out/filename).open('xb') as f:np.save(f,image,allow_pickle=False)
        records.append(dict(row,tensor=filename,tensor_sha256=sha256(out/filename)))
    if sha256(review_path)!=review_digest:
        raise ValueError('review changed during preparation')
    corpus=dict(schema='rfmon.drone_ml.presence_corpus.v1',preprocessing=CONFIG.metadata(),
                review_sha256=review_digest,records=records,readiness=status,
                final_test_iq_read=False,live_qualified=False)
    write_new_json(out/'corpus.json',corpus)
    return corpus


def main():
    p=argparse.ArgumentParser(description=__doc__)
    commands=p.add_subparsers(dest='command',required=True)
    t=commands.add_parser('template');t.add_argument('--sessions',type=Path,required=True);t.add_argument('--output',type=Path,required=True)
    r=commands.add_parser('check');r.add_argument('--review',type=Path,required=True);r.add_argument('--output',type=Path,required=True)
    b=commands.add_parser('prepare');b.add_argument('--review',type=Path,required=True);b.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    if args.command=='template':
        result=template(args.sessions);write_new_json(args.output,result)
        print(f"Review template: {len(result['records'])} intervals; none automatically labelled.")
    elif args.command=='check':
        result=readiness(read_json(args.review));write_new_json(args.output,result)
        print(json.dumps(result,indent=2))
    else:
        result=prepare(args.review,args.output);print(f"Prepared {len(result['records'])} reviewed intervals.")


if __name__=='__main__':main()
