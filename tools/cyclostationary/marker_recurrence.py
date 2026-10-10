"""Bounded apparent recurrence of compatible markers in one recording.

No cross-file association, inferred gap filling, identity or probabilities.
All matching tolerances are development policy, not calibrated confidence.
"""
import math

def assess_recurrence(points,rate,continuity_verified=False):
    if type(continuity_verified) is not bool or not math.isfinite(rate) or not 0<rate<=1e9 or len(points)>512:raise ValueError('recurrence bounds')
    for p in points:
        if (type(p['sample']) is not int or p['sample']<0 or
            p['profile'] not in ('up_9mhz','up_18mhz','down_9mhz') or
            not math.isfinite(p['frequency_hz']) or not math.isfinite(p['coherence']) or
            not 0<=p['coherence']<=1):raise ValueError('invalid marker point')
    retained=[]
    for p in sorted(points,key=lambda p:p['coherence'],reverse=True):
        if not any(p['profile']==q['profile'] and abs(p['sample']-q['sample'])<=rate*66.7e-6 and
                   abs(p['frequency_hz']-q['frequency_hz'])<=250000 for q in retained):retained.append(p)
    retained.sort(key=lambda p:p['sample'])
    chains=[]
    for i,p in enumerate(retained):
        # The source reports regular up-chirps; down-chirps are not assigned this cadence.
        chain=[i]
        if p['profile'].startswith('up_'):
            options=[chains[j]+[i] for j,q in enumerate(retained[:i])
                     if q['profile']==p['profile'] and
                     abs((p['sample']-q['sample'])/rate-.02)<=.0001 and
                     abs(p['frequency_hz']-q['frequency_hz'])<=250000 and
                     abs(p['frequency_hz']-retained[chains[j][0]]['frequency_hz'])<=250000]
            if options:chain=max(options,key=len)
        chains.append(chain)
    best=max(chains,key=len,default=[])
    sequence=[retained[i] for i in best]
    return {'policy':'apparent_marker_recurrence_v1','input_points':len(points),'distinct_points':len(retained),
        'deduplicated_points':len(points)-len(retained),'longest_sequence':sequence,
        'intervals_seconds':[(b['sample']-a['sample'])/rate for a,b in zip(sequence,sequence[1:])],
        'apparent_20ms_recurrence':len(best)>=3,'continuity_verified':continuity_verified,
        'recurrence_qualified':len(best)>=3 and continuity_verified,'family_accepted':False,
        'period_seconds':.02,'period_tolerance_seconds':.0001,'frequency_tolerance_hz':250000,
        'missing_periods_bridged':False,'transmitter_identity_established':False}


def assess_segmented_recurrence(points,rate,segments):
    """Never associate across declared gaps, retunes or continuity segments.

    Verified continuity is supplied by a separate capture audit, not inferred
    from matching IQ or timestamps. Unknown continuity permits only an apparent
    sequence. Complete marker support must fit one segment.
    """
    if len(segments)>64:raise ValueError('continuity segment budget')
    assess_recurrence(points,rate,False) # Validate points even when no segments.
    previous=0;ids=set()
    for s in segments:
        if (not isinstance(s['start'],int) or isinstance(s['start'],bool) or
            not isinstance(s['end'],int) or isinstance(s['end'],bool) or
            s['start']<previous or s['end']<=s['start'] or
            not isinstance(s['id'],str) or not s['id'] or s['id'] in ids or
            type(s['continuity_verified']) is not bool or
            (s['continuity_verified'] and not s.get('audit_reference'))):
            raise ValueError('invalid/overlapping/unqualified continuity segments')
        previous=s['end'];ids.add(s['id'])
    buckets=[[] for _ in segments];excluded=[]
    for p in points:
        begin=p.get('support_start');end=p.get('support_end')
        if (type(begin) is not int or type(end) is not int or
            not 0<=begin<=p['sample']<end):raise ValueError('invalid complete marker support')
        owner=next((i for i,s in enumerate(segments) if s['start']<=begin and end<=s['end']),None)
        if owner is None:excluded.append(p)
        else:buckets[owner].append(p)
    reports=[]
    for s,bucket in zip(segments,buckets):
        reports.append(dict(segment=s,recurrence=assess_recurrence(bucket,rate,s['continuity_verified'])))
    return dict(policy='segmented_marker_recurrence_v1',segments=reports,
        input_points=len(points),excluded_support_count=len(excluded),
        excluded_points=excluded,cross_segment_association=False,family_accepted=False,
        any_qualified_recurrence=any(r['recurrence']['recurrence_qualified'] for r in reports))
