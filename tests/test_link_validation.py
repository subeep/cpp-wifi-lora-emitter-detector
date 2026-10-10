import copy,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/cyclostationary'))
from evaluate_link_validation import evaluate,wilson_lower

def row(i,family='OcuSync',prediction='OcuSync',outcome='accepted'):
 return dict(id=str(i),true_family=family,predicted_family=prediction,outcome=outcome,
  truth_verified=True,independence_verified=True,inside_supported_envelope=True,
  partition='sealed_test',independence_group=str(i),pipeline_sha256='a'*64,
  truth_audit_reference='synthetic test only',physical_unit_id=str(i%3),session_id=str(i))
d=dict(encounters=[],advertised_families=['OcuSync'],pipeline_sha256='a'*64)
r=evaluate(d);assert r['acceptance_coverage'] is None and r['families'][0]['precision'] is None
assert wilson_lower(3,3)<.95 and wilson_lower(100,100)>.95
d['encounters']=[row(i) for i in range(100)]
r=evaluate(d);assert r['families'][0]['proposed_accuracy_and_diversity_targets_passed'] and not r['release_complete']
d['encounters'] += [row(100+i,prediction=None,outcome='unavailable') for i in range(10)]
r=evaluate(d);assert r['families'][0]['conditional_recall']==100/110
assert not r['families'][0]['proposed_accuracy_and_diversity_targets_passed']
d['encounters'].append(row(111,'ordinary_wifi'))
r=evaluate(d);assert r['nonadvertised_wrong_known_labels']==1 and r['families'][0]['correct']==100
bad=copy.deepcopy(d);bad['encounters'][1]['independence_group']='0'
try:evaluate(bad)
except ValueError:pass
else:raise AssertionError('correlated crops counted independently')
for field,value in [('partition','development_only'),('truth_verified',False),('independence_verified',False),('truth_audit_reference',''),('pipeline_sha256','b'*64)]:
 sample=row(0);sample[field]=value
 assert evaluate(dict(d,encounters=[sample]))['eligible_encounters']==0
sample=row(0);sample['truth_verified']='true'
try:evaluate(dict(d,encounters=[sample]))
except ValueError:pass
else:raise AssertionError('truthy string accepted')
print('validation: no-data, confidence, misses, competing labels, independence and provenance passed')
