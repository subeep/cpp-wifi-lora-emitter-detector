"""Versioned scoped-corpus inventory; preserve partitions and dataset originals.

Only explicitly admitted Tampere training files are read, for MD5/SHA-256
verification. Other records are metadata-only. No IQ decoding or training.
"""
import argparse
from collections import Counter,defaultdict
import hashlib
from pathlib import Path
import re

from .common import new_output,read_source,sha256,stamp,write_json
from .label_contract import export as export_contract
from .localizer_audit import load
from .recognition_review import validate_frozen_review


def publisher_matches(records,catalog):
    if catalog.get('schema')!='rfmon.drone_ml.publisher_catalog.v1' or catalog.get('version')!='1.0.1':
        raise ValueError('unsupported publisher catalog')
    files={}
    for f in catalog['files']:
        name=f['name']
        if name!=Path(name).name or name in files or not re.fullmatch('[0-9a-f]{32}',f['md5']):
            raise ValueError('invalid publisher filename/checksum')
        files[name]=f
    selected=[]
    for r in records:
        if r['dataset']!='Tampere_DroneRF':continue
        # A publisher listing must never authorize reading a protected source.
        if r['partition']!='train':raise ValueError('Tampere verification requires admitted train partition')
        name=Path(r['source']).name
        if name not in files:raise ValueError('record absent from publisher catalog')
        if r['adapter']!='ci16' or r.get('member'):raise ValueError('unexpected Tampere format')
        rate=120_000_000 if '_2G' in name else 200_000_000
        samples=120_000_000 if '_2G' in name else 100_000_000
        if r['rate_hz']!=rate or r['total_samples']!=samples or r['source_stat'][2]!=samples*4:
            raise ValueError('publisher format disagrees with frozen metadata')
        selected.append((r,files[name]))
    if len({r['source'] for r,_ in selected})!=len(selected):raise ValueError('duplicate Tampere source')
    return selected


def verify_selected(selected,output):
    verified=[]
    for index,(r,published) in enumerate(selected):
        path=Path(r['source']);before=stamp(path)
        if before[2]!=r['total_samples']*4:raise ValueError('source size changed')
        md5=hashlib.md5();strong=hashlib.sha256();count=0
        with read_source(path) as f:
            for block in iter(lambda:f.read(4*1024*1024),b''):
                md5.update(block);strong.update(block);count+=len(block)
        if stamp(path)!=before or count!=before[2]:raise ValueError('source changed during hashing')
        if md5.hexdigest()!=published['md5']:raise ValueError('publisher MD5 mismatch')
        verified.append(dict(path=str(path),source_sha256=strong.hexdigest(),md5=md5.hexdigest(),
            bytes=count,stat=before,stat_preserved=True,partition=r['partition'],recording_id=r['recording_id']))
        write_json(output/'progress.json',dict(phase='source_checksums',completed=index+1,total=len(selected)))
    return verified


def compile_inventory(review,plan,audit,catalog,verified,local_contract):
    validate_frozen_review(review,plan)
    if audit['frozen_records']!=review['records'] or audit['frozen_groups']!=plan['groups']:
        raise ValueError('audit does not match frozen review and splits')
    selected=publisher_matches(review['records'],catalog)
    verified_by_path={r['path']:r for r in verified}
    if len(verified_by_path)!=len(verified):raise ValueError('duplicate verification record')
    if set(verified_by_path)!={r['source'] for r,_ in selected}:raise ValueError('verification set incomplete')
    for row,pub in selected:
        v=verified_by_path[row['source']]
        if (v['md5']!=pub['md5'] or v['partition']!='train' or v['recording_id']!=row['recording_id'] or
                not re.fullmatch('[0-9a-f]{64}',v['source_sha256'])):
            raise ValueError('verification metadata mismatch')
    records=[];frozen_lookup={};class_groups=defaultdict(lambda:defaultdict(set))
    for r in review['records']:
        status='frozen_test_metadata_only' if 'test' in r['partition'] else (
            'frozen_calibration_metadata_only' if 'calibration' in r['partition'] else 'publisher_context_pending_scope_review')
        v=verified_by_path.get(r['source'])
        if v:status='verified_recording_bag'
        entry=dict(id='frozen:'+r['recording_id'],dataset=r['dataset'],
            source=dict(path=r['source'],member=r.get('member'),source_sha256=v['source_sha256'] if v else None),
            partition=r['partition'],collection_group=r['split_group'],context_label=r['label'],role=r['role'],
            status=status,supervision_scope='recording_bag' if v else 'review_only',
            target=dict(presence='drone_system_present',identity=r['label']) if v else None,
            training_eligible=bool(v),negative_eligible=False,per_crop_label_inheritance=False,
            sample_rate_hz=r['rate_hz'],center_hz=r['center_hz'],total_samples=r['total_samples'],
            source_checksum_reverified=bool(v),physical_unit=r['physical_unit'],
            raw_iq_decoded=False,existing_partition_preserved=True)
        records.append(entry);frozen_lookup[(r['source'],r.get('member'))]=entry
        class_groups[r['label']][r['partition']].add(r['split_group'])
    lineage=[]
    for i,d in enumerate(audit['derivative_lineage']):
        parent=frozen_lookup.get((d['source'],d.get('member')))
        row=dict(id=f'derivative:{i}',dataset='diagnostic_derivative',source=dict(path=d['path']),
            partition=d['derivative_partition'],collection_group=parent['collection_group'] if parent else 'unresolved_derivative:'+d['source'],
            status='diagnostic_only_never_independent_evaluation',supervision_scope='review_only',target=None,
            training_eligible=False,negative_eligible=False,source_checksum_reverified=False,
            parent_source=d['source'],parent_member=d.get('member'),parent_id=parent['id'] if parent else None,
            independent_evaluation_eligible=False,per_crop_label_inheritance=False)
        records.append(row);lineage.append(dict(derivative_id=row['id'],parent_id=row['parent_id'],
            original_partitions=d['original_partitions'],diagnostic_partition=d['derivative_partition'],
            independent_evaluation_eligible=False))
    for r in audit['nist_records']:
        records.append(dict(id='nist:'+r['dataset_key'],dataset='NIST_WiFi',source=dict(path=r['path'],dataset_key=r['dataset_key'],source_sha256=r['source_sha256']),
            partition=r['partition'],collection_group=r['collection_group'],status='wifi_context_not_verified_drone_absence',
            supervision_scope='review_only',target=None,training_eligible=False,negative_eligible=False,
            source_checksum_reverified=False,sample_rate_hz=r['sample_rate_hz'],center_hz=r['center_hz'],
            independent_drone_absence_evaluation=False,per_crop_label_inheritance=False))
    for r in audit['fpv_sessions']:
        records.append(dict(id='fpv:'+r['session'],dataset='FPV_Analog_Video',source=dict(path=r['archive'],member=r['member']),
            partition=r['partition'],collection_group=r['conservative_collection_group'],status='format_and_label_scope_unresolved',
            supervision_scope='review_only',target=None,training_eligible=False,negative_eligible=False,
            source_checksum_reverified=False,context=r['context'],format_issue=r['issue'],
            first_probe_frequency_hz=r['first_block_frequency_hz'],per_crop_label_inheritance=False))
    known_paths={r['source']['path'] for r in records}
    for r in local_contract['records']:
        s=r['source']
        if s['path'] in known_paths:continue
        records.append(dict(id='local:'+r['id'],dataset='local_context',source=s,partition=s['partition'],
            collection_group=s['collection_group'],status='local_context_no_scene_wide_truth',supervision_scope='review_only',
            target=None,training_eligible=False,negative_eligible=False,source_checksum_reverified=False,
            per_crop_label_inheritance=False))
    if len({r['id'] for r in records})!=len(records):raise ValueError('duplicate inventory id')
    coverage={label:{part:len(groups) for part,groups in parts.items()} for label,parts in sorted(class_groups.items())}
    admitted_names={Path(r['source']).name for r,_ in selected}
    excluded_by_name={Path(r['source']).name:r for r in review['source_decisions'] if r['dataset']=='Tampere_DroneRF'}
    extras=[dict(**f,status='not_admitted_preserve_exclusion',prior_decision=excluded_by_name.get(f['name']))
            for f in catalog['files'] if f['name'] not in admitted_names]
    return dict(schema='rfmon.drone_ml.corpus_inventory.v1',records=records,class_group_coverage=coverage,
        derivative_lineage=lineage,publisher_files_not_admitted=extras,
        background_evaluation=dict(verified_negative_records=0,ready=False,
            nist_use='unlabelled Wi-Fi stress review only; not specificity/FAR scoring',
            fpv_use='target-absent context does not confirm all-drone absence; format remains unresolved'),
        summary=dict(records=len(records),frozen_recordings=len(review['records']),
            frozen_groups=len(plan['groups']),dataset_counts=dict(Counter(r['dataset'] for r in records)),
            original_partition_counts=dict(Counter(r['partition'] for r in review['records'])),
            verified_bags=len(verified),verified_bag_classes=len({r['target']['identity'] for r in records if r['training_eligible']}),
            verified_negative_records=0,adjudicated_regions=0,diagnostic_derivatives=len(lineage)),
        training_ready=False,existing_partitions_changed=False,final_test_iq_read=False,
        note='Metadata inventory plus checksum-verified weak bags. No per-window labels, decoded IQ, tensors or trained model.')


def build(review_path,plan_path,audit_path,catalog_path,local_path,output):
    paths=list(map(Path,(review_path,plan_path,audit_path,catalog_path,local_path)))
    before={p:stamp(p) for p in paths};hashes={str(p):sha256(p) for p in paths}
    review,plan,audit,catalog,local=map(load,paths)
    validate_frozen_review(review,plan)
    if audit['frozen_records']!=review['records'] or audit['frozen_groups']!=plan['groups']:
        raise ValueError('frozen metadata mismatch')
    selected=publisher_matches(review['records'],catalog) # all guards before source access
    out=new_output(output,[p.parent for p in paths]+[Path(r['source']).parent for r in review['records']])
    code={str(Path(__file__)):sha256(__file__),str(Path(__file__).with_name('label_contract.py')):sha256(Path(__file__).with_name('label_contract.py'))}
    write_json(out/'inputs.json',dict(input_sha256=hashes,code_sha256=code))
    verified=verify_selected(selected,out);write_json(out/'source_verification.json',dict(records=verified,iq_decoded=False))
    inventory=compile_inventory(review,plan,audit,catalog,verified,local)
    evidence_dir=out/'evidence';evidence_dir.mkdir();contract=[]
    for r in inventory['records']:
        if not r['training_eligible']:continue
        source=dict(path=r['source']['path'],source_sha256=r['source']['source_sha256'],partition=r['partition'],
            collection_group=r['collection_group'],adapter='ci16_le',sample_rate_hz=r['sample_rate_hz'])
        evidence=dict(schema='rfmon.drone_ml.label_evidence.v1',kind='publisher_controlled_recording',reviewed=True,
            reviewer='publisher_catalog_and_checksum_audit',source_sha256=source['source_sha256'],scope='recording_bag',target=r['target'],
            provenance=dict(publisher_url=catalog['url'],publisher_version=catalog['version'],catalog_sha256=hashes[str(paths[3])],
                            source_verification=str((out/'source_verification.json').resolve())),
            limitations=['whole recording drone/controller system association','not per-crop identity','physical unit independence unresolved','do not join split recordings'])
        ep=evidence_dir/(r['id'].replace(':','_')+'.json');write_json(ep,evidence)
        contract.append(dict(id=r['id'],source=source,scope='recording_bag',target=r['target'],whole_recording_samples=r['total_samples'],
            evidence=dict(path=str(ep.resolve()),sha256=sha256(ep))))
    cp=out/'contract.json';write_json(cp,dict(schema='rfmon.drone_ml.supervision_contract.v1',records=contract))
    # Export beside the inventory: the contract exporter forbids outputs inside
    # its evidence/input directories and all dataset roots.
    exported=out.with_name(out.name+'_scoped_units')
    decisions=export_contract(cp,exported)
    inventory['scoped_export']=str(exported.resolve());inventory['accepted_scoped_units']=decisions['accepted']
    write_json(out/'inventory.json',inventory);write_json(out/'summary.json',inventory['summary'])
    write_json(out/'extraction_plan.json',dict(schema='rfmon.drone_ml.corpus_extraction_plan.v1',
        status='not_training_ready',scoped_units=str((exported/'scoped_units.json').resolve()),
        mandatory_rules=['reverify source hashes before extraction','preserve collection groups and frozen splits',
            'bag labels stay at recording level','retain full-band and parent context alongside optional cores',
            'missing continuity is masked, never fabricated','retain selector and preprocessing provenance'],
        next_gates=['verified negative supervision','adequate independent positive calibration groups',
            'freeze numeric preprocessing and scope-aware tensor contract'],tensors_created=False))
    if any(stamp(p)!=s or sha256(p)!=hashes[str(p)] for p,s in before.items()):raise ValueError('input changed')
    if any(sha256(p)!=h for p,h in code.items()):raise ValueError('code changed')
    write_json(out/'progress.json',dict(phase='complete',records=len(inventory['records']),accepted_scoped_units=decisions['accepted']))
    return inventory


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('review','plan','audit','catalog','local','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();r=build(a.review,a.plan,a.audit,a.catalog,a.local,a.output);print(r['summary'])


if __name__=='__main__':main()
