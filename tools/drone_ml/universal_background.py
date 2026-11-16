"""Freeze whole-session background references without copying or relabelling IQ."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
from .universal_inventory import read_json

BANDS = ('2.4 GHz', '5.2 GHz', '5.8 GHz')


def band(center):
    if 2.3e9 <= center < 2.5e9: return BANDS[0]
    if 5.1e9 <= center < 5.4e9: return BANDS[1]
    if 5.7e9 <= center < 5.9e9: return BANDS[2]
    raise ValueError('unexpected background band')


def validate_split(parts):
    owners, paths, digests = {}, {}, {}
    for partition, rows in parts.items():
        for row in rows:
            for key, mapping in ((row['session'], owners), (str(Path(row['source']).resolve()), paths),
                                 (row['sha256'], digests)):
                old = mapping.setdefault(key, partition)
                if old != partition: raise ValueError('background overlap across partitions')
            if row['samples'] <= 0 or row['rate_hz'] <= 0: raise ValueError('invalid capture duration')
    if not all(parts.values()): raise ValueError('empty background partition')
    if {band(r['center_hz']) for r in parts['held_out']} != set(BANDS):
        raise ValueError('held-out background misses a scanned band')


def prepare(repo, sessions, confirmation):
    if confirmation.get('background_absence_confirmed') is not True:
        raise ValueError('explicit background scene confirmation required')
    session_dir = Path(sessions)/'air3s-bg-ambient-A-20261110-083841'
    source = read_json(session_dir/'session.json')
    train, held, excluded = [], [], []
    for row in source['segments']:
        p = session_dir/row['file']
        size = p.stat().st_size
        if (row['status'] != 'ok' or row['overflow_marks'] or row['samples'] != row['requested_samples']
                or size != row['samples']*4):
            excluded.append(dict(source=str(p), reason='short_or_overflow_capture')); continue
        sidecar = read_json(p.with_suffix('.json'))
        if sidecar['sha256'] != row['sha256']: raise ValueError('sidecar/session hash disagreement')
        train.append(dict(source=str(p), session=session_dir.name, format='sc16', rate_hz=source['rate_hz'],
                          center_hz=row['centre_hz'], samples=row['samples'], sha256=row['sha256'],
                          label_basis='existing scene description and user confirmation 2026-11-12',
                          iq_hash_reverified=False))
    directory = Path(repo)/'data/air3s_o4/2026-10-23/background-01'
    validation = read_json(directory.parent/'background-01-validation.json')
    if validation.get('label_source') != 'user confirmation' or not validation.get('pass'):
        raise ValueError('October background lacks recorded confirmation/validation')
    for row in validation['captures']:
        p = directory/row['file']; meta = read_json(p.with_suffix('.json'))
        if not row['integrity_pass'] or p.stat().st_size != row['samples']*8:
            raise ValueError('held-out capture integrity changed')
        held.append(dict(source=str(p), session='Air3S:2026-10-23:background-01', format='cf32',
                         rate_hz=meta['sample_rate_hz'], center_hz=meta['capture_center_hz'],
                         samples=row['samples'], sha256=row['sha256'],
                         label_basis='existing October user-confirmed validation', iq_hash_reverified=False))
    parts = dict(train=train, held_out=held)
    validate_split(parts)
    summary = {k:dict(captures=len(v), sessions=sorted({r['session'] for r in v}),
                     iq_seconds=sum(r['samples']/r['rate_hz'] for r in v),
                     centers_hz=sorted({r['center_hz'] for r in v}),
                     bands=sorted({band(r['center_hz']) for r in v})) for k,v in parts.items()}
    return dict(schema='rfmon.drone_ml.universal_background.v1', partitions=parts, summary=summary,
                excluded=excluded, source_confirmation=confirmation, antenna_position='unknown',
                held_out_use='development_only_not_threshold_setting_or_final_scorecard',
                prior_exposure='Both sessions have been used in earlier diagnostics; no training/split artifact referencing the October session was found in the reviewed manifests.',
                missing_for_C4='A separate threshold-calibration session and an untouched evaluation session with adequate duration.',
                final_false_rows_per_hour_claim_ready=False,
                note='3.25 seconds of held-out IQ covers three bands, not every current tuning centre and not enough exposure to establish a <=1/hour rate. Whole sessions stay disjoint.')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2])
    p.add_argument('--sessions',type=Path,default=Path('/media/sudeep/Storage/x310_sessions'))
    p.add_argument('--confirmation',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args(); result=prepare(a.repo,a.sessions,read_json(a.confirmation))
    a.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result['summary'],indent=2))


if __name__=='__main__': main()
