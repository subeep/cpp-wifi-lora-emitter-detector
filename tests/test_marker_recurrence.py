import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/cyclostationary'))
from marker_recurrence import assess_recurrence
from replay_contiguous_link_markers import offsets

def point(t,frequency=2440e6,profile='up_18mhz'):
    return {'sample':round(t*100e6),'frequency_hz':frequency,'profile':profile,'coherence':.95}

r=assess_recurrence([point(.001+i*.02) for i in range(5)],100e6)
assert r['apparent_20ms_recurrence'] and not r['recurrence_qualified'] and not r['family_accepted']
r=assess_recurrence([point(.001),point(.00101),point(.021),point(.041)],100e6)
assert r['distinct_points']==3 and r['deduplicated_points']==1
assert not assess_recurrence([point(.001),point(.041),point(.081)],100e6)['apparent_20ms_recurrence']
assert not assess_recurrence([point(.001),point(.021,2450e6),point(.041)],100e6)['apparent_20ms_recurrence']
assert not assess_recurrence([point(.001+i*.02,profile='down_9mhz') for i in range(4)],100e6)['apparent_20ms_recurrence']
assert not assess_recurrence([point(.001),point(.0212),point(.0414)],100e6)['apparent_20ms_recurrence']
assert not assess_recurrence([],100e6)['apparent_20ms_recurrence']
for points,rate in [([point(.001)]*513,100e6),([point(.001,float('nan'))],100e6),([],0)]:
    try:assess_recurrence(points,rate)
    except ValueError:pass
    else:raise AssertionError('invalid input accepted')
for rate in [20e6,100e6,120e6,200e6]:
    total=int(rate*.1);starts=offsets(total,rate);support=int(rate*64e-6)
    assert starts[0]==0 and starts[-1]+65536==total
    for a,b in zip(starts,starts[1:]):assert a+65536-b>=support
    for position in range(0,total-support,10007):
        assert any(a<=position and position+support<=a+65536 for a in starts)
print('recurrence deduplication, missing periods, frequency conflicts, insufficient support and overlap coverage passed')
import math
for rate in [20e6,25e6,100e6,120e6,200e6]:
    support=math.ceil(rate*(1/15000+12e-6));total=round(rate*.1)
    starts=offsets(total,rate,1/15000+12e-6)
    assert all(a+65536-b>=support for a,b in zip(starts,starts[1:]))
    assert starts[-1]+65536==total
for seconds in [0,-1,float('nan'),.1]:
    try:offsets(100000,100e6,seconds)
    except ValueError:pass
    else:raise AssertionError('invalid/budget-exceeding overlap accepted')
print('full-symbol plus boundary-context overlap coverage passed')
from marker_recurrence import assess_segmented_recurrence
pts=[point(.001+i*.02) for i in range(5)]
for p in pts:p.update(support_start=p['sample']-800,support_end=p['sample']+7100)
s=[dict(id='a',start=0,end=10000000,continuity_verified=False)]
r=assess_segmented_recurrence(pts,1e8,s)
assert r['segments'][0]['recurrence']['apparent_20ms_recurrence'] and not r['any_qualified_recurrence']
# Split the five otherwise periodic markers across three acquisition segments.
s=[dict(id=str(i),start=a,end=b,continuity_verified=True,audit_reference='test fixture')
   for i,(a,b) in enumerate([(0,3000000),(3000000,7000000),(7000000,10000000)])]
r=assess_segmented_recurrence(pts,1e8,s)
assert not r['any_qualified_recurrence'] and not r['cross_segment_association']
s=[dict(id='a',start=0,end=pts[0]['sample']+1,continuity_verified=False)]
assert assess_segmented_recurrence(pts,1e8,s)['excluded_support_count']==5
for bad in [[dict(id='x',start=0,end=100,continuity_verified=True)],
 [dict(id='x',start=0,end=100,continuity_verified='false')],
 [dict(id='x',start=0,end=100,continuity_verified=False),dict(id='y',start=90,end=200,continuity_verified=False)]]:
 try:assess_segmented_recurrence(pts,1e8,bad)
 except ValueError:pass
 else:raise AssertionError('invalid continuity accepted')
print('continuity segments, gaps, complete support and unverified assertions passed')
