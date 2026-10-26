"""Offline scoped-supervision contract. Never infer labels from RF proposals.

Evidence statements are curated inputs, not facts this validator authenticates.
This exporter is separate from existing training code and reads no IQ.
"""
import argparse
import json
import math
from pathlib import Path
import re

from .common import new_output, read_source, sha256, stamp, write_json


def load(path):
    with read_source(path) as f:return json.load(f)


def _hash(value):
    return isinstance(value,str) and re.fullmatch('[0-9a-f]{64}',value) is not None


def validate(manifest, evidence_loader):
    if manifest.get('schema')!='rfmon.drone_ml.supervision_contract.v1':raise ValueError('invalid contract schema')
    rows=manifest.get('records',[])
    if not isinstance(rows,list) or not 1<=len(rows)<=10000:raise ValueError('record budget')
    groups={};sources={};ids=set()
    # All split checks precede even evidence-file access. No IQ access exists.
    for row in rows:
        s=row['source'];partition=s.get('partition')
        if partition not in ('train','development','development_only'):raise ValueError('reserved or unknown partition')
        split='train' if partition=='train' else 'development'
        if not row.get('id') or row['id'] in ids:raise ValueError('missing or duplicate id')
        ids.add(row['id'])
        if not _hash(s.get('source_sha256')) or not s.get('collection_group') or not s.get('path'):
            raise ValueError('source provenance required')
        # Conservative: even distinct captures in one container cannot straddle splits.
        for mapping,key in ((groups,s['collection_group']),(sources,s['source_sha256'])):
            if key in mapping and mapping[key]!=split:raise ValueError('source or collection-group split leakage')
            mapping[key]=split
    decisions=[]
    for row in rows:
        scope=row.get('scope');target=row.get('target');s=row['source'];reason=None
        if scope=='review_only' and target is None:
            reason='context_only_no_supervision'
        elif scope not in ('recording_bag','region','controlled_session'):
            reason='unsupported_scope'
        elif not isinstance(target,dict) or set(target)!= {'presence','identity'}:
            reason='explicit_presence_and_identity_required'
        elif target['presence'] not in ('drone_system_present','no_drone'):
            reason='unsupported_presence_target'
        elif target['identity'] is not None and (not isinstance(target['identity'],str) or not target['identity'].strip()):
            reason='invalid_identity'
        elif target['presence']=='no_drone' and target['identity'] is not None:
            reason='absence_cannot_have_drone_identity'
        elif scope=='recording_bag' and (row.get('region') is not None or target['presence']!='drone_system_present'):
            reason='bag_label_cannot_be_crop_or_absence'
        elif scope=='controlled_session' and (target['presence']!='no_drone' or row.get('region') is not None):
            reason='controlled_session_requires_whole_scope_absence'
        elif scope=='region':
            r=row.get('region',{})
            if (set(r)!={'start_sample','stop_sample','low_hz','high_hz'} or
                    any(type(r.get(k)) is not int for k in ('start_sample','stop_sample')) or
                    not 0<=r['start_sample']<r['stop_sample'] or
                    not all(isinstance(r.get(k),(int,float)) and math.isfinite(r[k]) for k in ('low_hz','high_hz')) or
                    not r['low_hz']<r['high_hz'] or row.get('boundary_reviewed') is not True):
                reason='adjudicated_geometry_required'
        if reason is None:
            evidence=row.get('evidence',{})
            if not evidence.get('path') or not _hash(evidence.get('sha256')):
                reason='reviewed_evidence_required'
            else:
                doc=evidence_loader(evidence)
                expected_kind={'recording_bag':'publisher_controlled_recording','region':'adjudicated_region',
                               'controlled_session':'controlled_no_drone_session'}[scope]
                if (doc.get('schema')!='rfmon.drone_ml.label_evidence.v1' or
                        doc.get('kind')!=expected_kind or doc.get('reviewed') is not True or
                        not doc.get('reviewer') or not doc.get('provenance') or
                        doc.get('source_sha256')!=s['source_sha256'] or doc.get('dataset_key')!=s.get('dataset_key') or
                        doc.get('scope')!=scope or doc.get('target')!=target or
                        doc.get('region')!=row.get('region')):
                    reason='evidence_does_not_support_exact_scope_and_target'
                elif scope=='controlled_session' and doc.get('all_drone_absence_confirmed') is not True:
                    reason='target_off_is_not_verified_drone_absence'
        decisions.append(dict(id=row['id'],accepted=reason is None,reason=reason or 'supported_scoped_target',
                              scope=scope,training_unit=scope if reason is None else None))
    return dict(schema='rfmon.drone_ml.supervision_decisions.v1',records=decisions,
        accepted=sum(d['accepted'] for d in decisions),iq_read=False,
        note='Checks consistency with curated evidence; not independent truth verification. Bag labels never become per-crop labels.')


def export(manifest_path,output):
    manifest_path=Path(manifest_path);before={manifest_path:stamp(manifest_path)}
    manifest=load(manifest_path);hashes={str(manifest_path):sha256(manifest_path)}
    def evidence_loader(ref):
        path=Path(ref['path']);before[path]=stamp(path);actual=sha256(path)
        if actual!=ref['sha256']:raise ValueError('evidence checksum mismatch')
        hashes[str(path)]=actual;return load(path)
    result=validate(manifest,evidence_loader)
    if any(stamp(p)!=s for p,s in before.items()):raise ValueError('input changed during validation')
    out=new_output(output,[p.parent for p in before]+[Path(r['source']['path']).parent for r in manifest['records']])
    accepted={d['id'] for d in result['records'] if d['accepted']}
    # This format is intentionally not the legacy per-window training format.
    approved=dict(schema='rfmon.drone_ml.scoped_training_units.v1',
        records=[r for r in manifest['records'] if r['id'] in accepted],input_sha256=hashes,
        requires_scope_aware_trainer=True,legacy_window_label_conversion_allowed=False,
        source_checksums_must_be_reverified_before_iq_read=True,training_started=False)
    write_json(out/'decisions.json',result);write_json(out/'scoped_units.json',approved)
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--manifest',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    print(json.dumps(export(a.manifest,a.output),indent=2))


if __name__=='__main__':main()
