#!/usr/bin/env python3
"""Report independent encounter metrics without enabling runtime identities.

Exactly one predeclared evaluation unit per independence group is required.
This rejects correlated crops and development-exposed references. Metadata
assertions still require human/source audit; a report is never a certificate.
"""
import argparse
import json
import math
from collections import Counter
from pathlib import Path


def wilson_lower(successes,total):
    if not 0<=successes<=total:raise ValueError('invalid count')
    if total==0:return None
    z=1.6448536269514722 # One-sided 95%, frozen before a future held evaluation.
    p=successes/total
    return (p+z*z/(2*total)-z*math.sqrt(p*(1-p)/total+z*z/(4*total*total)))/(1+z*z/total)


def evaluate(document):
    rows=document['encounters'];families=document['advertised_families']
    if len(rows)>100000 or not families or len(families)>32 or len(set(families))!=len(families):raise ValueError('evaluation bounds')
    if any(not isinstance(f,str) or not f for f in families):raise ValueError('invalid family scope')
    seen=set();excluded=Counter();eligible=[]
    pipeline=document['pipeline_sha256']
    if len(pipeline)!=64 or any(c not in '0123456789abcdef' for c in pipeline):raise ValueError('invalid pipeline digest')
    for r in rows:
        if not isinstance(r['id'],str) or not r['id'] or r['id'] in seen:raise ValueError('duplicate/invalid encounter')
        seen.add(r['id'])
        for b in ['truth_verified','independence_verified','inside_supported_envelope']:
            if type(r[b]) is not bool:raise ValueError('evaluation assertions must be boolean')
        if r['outcome'] not in ['accepted','unknown','ambiguous','unavailable']:raise ValueError('invalid outcome')
        prediction=r['predicted_family']
        if (r['outcome']=='accepted')!=(prediction is not None):raise ValueError('inconsistent accepted label')
        if prediction is not None and prediction not in families:raise ValueError('prediction outside advertised scope')
        if r['partition']!='sealed_test':excluded['not_sealed_test']+=1;continue
        if not r['truth_verified'] or r['true_family'] is None:excluded['unverified_truth']+=1;continue
        if not r['independence_verified'] or not r.get('independence_group'):excluded['unverified_independence']+=1;continue
        if r.get('pipeline_sha256')!=pipeline:excluded['pipeline_mismatch']+=1;continue
        if not r.get('truth_audit_reference'):excluded['missing_truth_audit']+=1;continue
        eligible.append(r)
    groups=[r['independence_group'] for r in eligible]
    if len(groups)!=len(set(groups)):raise ValueError('correlated rows require predeclared group-level reduction')
    metrics=[]
    for family in families:
        accepted=[r for r in eligible if r['predicted_family']==family]
        positives=[r for r in eligible if r['true_family']==family]
        conditional=[r for r in positives if r['inside_supported_envelope']]
        correct=sum(r['true_family']==family for r in accepted)
        detected=sum(r['predicted_family']==family for r in conditional)
        precision=correct/len(accepted) if accepted else None
        lower=wilson_lower(correct,len(accepted))
        recall=detected/len(conditional) if conditional else None
        units={r.get('physical_unit_id') for r in positives}-{None,''}
        sessions={r.get('session_id') for r in positives}-{None,''}
        row=dict(family=family,accepted=len(accepted),correct=correct,precision=precision,
            precision_one_sided_95_lower=lower,conditional_positives=len(conditional),
            conditional_recall=recall,conditional_recall_one_sided_95_lower=wilson_lower(detected,len(conditional)),
            unconditional_recall=sum(r['predicted_family']==family for r in positives)/len(positives) if positives else None,
            physical_units=len(units),sessions=len(sessions))
        row['proposed_accuracy_and_diversity_targets_passed']=bool(precision is not None and precision>=.99 and lower>=.95 and
            recall is not None and recall>=.95 and len(units)>=3 and len(sessions)>=3)
        metrics.append(row)
    nonadvertised=[r for r in eligible if r['true_family'] not in families]
    outcomes=dict(Counter(r['outcome'] for r in eligible))
    return dict(schema='independent_link_evaluation.v1',pipeline_sha256=pipeline,
        input_encounters=len(rows),eligible_encounters=len(eligible),excluded=dict(excluded),families=metrics,
        outcome_counts=outcomes,acceptance_coverage=outcomes.get('accepted',0)/len(eligible) if eligible else None,
        nonadvertised_encounters=len(nonadvertised),nonadvertised_wrong_known_labels=sum(r['predicted_family'] is not None for r in nonadvertised),
        metrics_unit='one_predeclared_unit_per_verified_independence_group',
        confidence_method='one_sided_95_percent_Wilson_binomial_conditional_on_audited_independent_groups',
        runtime_acceptance_enabled=False,release_complete=False,
        remaining_release_gates=['audit_truth_and_independence_assertions','certified_background_exposure_and_false_alert_episodes',
            'receiver_and_continuity_qualification','live_performance_and_endurance','review_supported_operating_envelope'])

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--input',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=evaluate(json.loads(a.input.read_text()));a.output.write_text(json.dumps(r,indent=2)+'\n')
