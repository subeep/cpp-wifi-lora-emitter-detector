"""T29/T30: validate controlled X310 recording sessions and their scene manifests.

Checks recorder integrity and operator-supplied scene evidence. It never assigns a
label: a session can at most become a *candidate* for a scoped negative or positive
label, which Sudeep approves in the scene manifest. Sealed final sessions are checked
from sidecar metadata only; their IQ is never opened here.
"""
import argparse
from collections import defaultdict
from datetime import datetime
import json
import math
from pathlib import Path
import re

import numpy as np

from .common import read_source, sha256, write_json

SCENE_SCHEMA = 'rfmon.drone_ml.x310_scene.v1'
REPORT_SCHEMA = 'rfmon.drone_ml.x310_session_validation.v1'
PARTITIONS = ('train', 'calibration', 'development', 'final_sealed')
# Scene types. Negative candidates require every own drone system off and a stated basis.
BACKGROUND = ('quiet', 'router_idle', 'router_load', 'phone_hotspot_upload', 'camera_stream',
              'bluetooth', 'mixed_non_drone')
POSITIVE = ('controller_only', 'linked_ground', 'linked_flight', 'mixed_with_drone')
STATES = ('off', 'on_unlinked', 'on_linked', 'unknown')
MIN_RATE, MAX_RATE = 20_000_000, 25_000_000      # ml admission floor; measured 1 GbE ceiling
PROBE = 1_000_000                                 # samples per bounded quality probe
MAX_BAD_FRACTION = .10                            # faulty segments tolerated (excluded) per session


def _time(value):
    try:
        t = datetime.fromisoformat(value)
    except (TypeError, ValueError):
        return None
    return t if t.tzinfo else None


def template(session_dir):
    """Blank scene manifest for the operator to complete; nothing is pre-filled as fact."""
    session = json.loads((Path(session_dir) / 'session.json').read_text())
    return dict(
        schema=SCENE_SCHEMA, recorder_session=Path(session_dir).name,
        recorder_created=session.get('created'),
        scene_type=None, scene_type_choices=list(BACKGROUND + POSITIVE),
        operator=None, environment_id=None, collection_day=None, collection_group=None,
        partition=None, partition_choices=list(PARTITIONS),
        receiver=dict(antenna_description=None, antenna_position=None, cable_or_filter=None),
        own_drone_systems=[dict(model=None, physical_unit=None, aircraft_state=None, controller_state=None,
                                distance_m=None, state_choices=list(STATES))],
        uncontrolled_drone_activity=dict(assessment=None, choices=['none_observed', 'possible', 'observed'], basis=None),
        nearby_equipment=[dict(kind=None, description=None, state=None, band=None)],
        timeline=[dict(time=None, note=None)],
        label_scope='controlled scene, observed band and time only',
        approval=dict(approved_by=None, approved_at=None, statement=None),
        instructions='Fill every null. Times are ISO-8601 with UTC offset. Powered-off or no decoded DroneID '
                     'alone does not prove absence: say how the scene was controlled. Leave approval empty '
                     'for anything ambiguous; it then stays unlabelled stress data.')


def _probe(path, samples, clip_limit):
    """Three bounded sc16 probes (start, middle, end). Reads at most 3 x PROBE samples."""
    out = []
    with read_source(path) as f:
        for start in sorted({0, max(0, samples // 2 - PROBE // 2), max(0, samples - PROBE)}):
            f.seek(start * 4)
            raw = np.frombuffer(f.read(min(PROBE, samples - start) * 4), dtype='<i2')
            if raw.size < 2:
                continue
            v = raw.astype(np.float64) / 32768
            out.append(dict(offset_samples=int(start), rms_dbfs=float(10 * np.log10(max(np.mean(v * v) * 2, 1e-30))),
                            clipped_fraction=float(np.mean(np.abs(raw) >= 32767)), all_zero=bool(not raw.any()),
                            dc_i=float(v[0::2].mean()), dc_q=float(v[1::2].mean())))
    problems = []
    if not out or any(p['all_zero'] for p in out):
        problems.append('empty or all-zero probe')
    if any(p['clipped_fraction'] > clip_limit for p in out):
        problems.append('clipping above limit')
    return out, problems


def check(session_dir, verify_hashes=False, clip_limit=1e-3):
    d = Path(session_dir)
    blockers, warnings, segments = [], [], []
    session = json.loads((d / 'session.json').read_text())
    scene_path = d / 'scene.json'
    scene = json.loads(scene_path.read_text()) if scene_path.exists() else None
    sealed = bool(scene) and scene.get('partition') == 'final_sealed'
    rate = session.get('rate_hz')
    if session.get('schema') != 1 or session.get('receive_only') is not True:
        blockers.append('unsupported recorder session')
    if not isinstance(rate, (int, float)) or rate != int(rate) or not MIN_RATE <= rate <= MAX_RATE:
        blockers.append(f'sample rate must be an integer within {MIN_RATE}-{MAX_RATE} Hz')
    if not str(session.get('format', '')).startswith('sc16_le'):
        blockers.append('unsupported sample format')
    rows = session.get('segments') or []
    if not rows:
        blockers.append('no segments')
    starts, seconds = [], 0.0
    for s in rows:
        name = s.get('file', '')
        path = d / name
        problems = []
        if not re.fullmatch(r'[A-Za-z0-9_.-]+\.sc16', name) or path.is_symlink() or not path.is_file():
            problems.append('missing or invalid segment file')
        else:
            size = path.stat().st_size
            if size != s.get('bytes') or size != 4 * s.get('samples', -1) or s.get('samples') != s.get('requested_samples'):
                problems.append('size differs from requested samples')
            side = path.with_suffix('.json')
            if not side.is_file() or json.loads(side.read_text()).get('sha256') != s.get('sha256'):
                problems.append('sidecar missing or disagrees with session')
            if not sealed and not problems:
                probes, bad = _probe(path, s['samples'], clip_limit)
                problems += bad
                s = dict(s, probes=probes)
                if verify_hashes and sha256(path) != s.get('sha256'):
                    problems.append('sha256 mismatch')
        if s.get('overflow_marks') != 0 or s.get('status') != 'ok':
            problems.append('overflow or recorder error')
        if not re.fullmatch('[0-9a-f]{64}', str(s.get('sha256'))):
            problems.append('invalid sha256')
        t = _time(s.get('host_start'))
        if t is None:
            problems.append('invalid host_start')
        elif isinstance(rate, (int, float)) and rate > 0:
            starts.append(t); seconds += s.get('samples', 0) / rate
        segments.append(dict(file=name, centre_hz=s.get('centre_hz'), samples=s.get('samples'),
                             host_start=s.get('host_start'), probes=s.get('probes'), problems=problems,
                             usable=not problems))
        if problems:
            # A faulty segment is excluded, never repaired or trimmed; too many fail the session.
            warnings.append(f'{name} excluded: ' + '; '.join(problems))
            if t is not None and isinstance(rate, (int, float)) and rate > 0:
                starts.pop(); seconds -= s.get('samples', 0) / rate
    bad = sum(not s['usable'] for s in segments)
    if rows and bad > MAX_BAD_FRACTION * len(rows):
        blockers.append(f'{bad} of {len(rows)} segments faulty (limit {MAX_BAD_FRACTION:.0%})')
    span = (max(starts) - min(starts)).total_seconds() if len(starts) > 1 else 0.0
    candidate, scene_report = None, None
    if scene is None:
        blockers.append('scene.json missing')
    else:
        sb = _scene(scene, d.name, starts)
        blockers += sb['blockers']; warnings += sb['warnings']
        candidate, scene_report = sb['candidate'], sb['summary']
    if session.get('target'):
        warnings.append('recorder "target" field is a tool default and is ignored; the scene manifest decides')
    return dict(schema=REPORT_SCHEMA, session=d.name, passed=not blockers, blockers=blockers, warnings=warnings,
                rate_hz=rate, gain_db=session.get('gain_db'), antenna=session.get('antenna'),
                device_args=session.get('device_args'), segments=segments, iq_seconds=seconds, wall_span_seconds=span,
                centres_hz=sorted({s['centre_hz'] for s in segments if s['centre_hz']}),
                segments_usable=len(segments) - bad, segments_excluded=bad,
                sealed_iq_opened=False if sealed else None, hashes_verified=bool(verify_hashes and not sealed),
                device_timestamps=False, scene=scene_report,
                label_candidate=candidate if not blockers else None, label_assigned=False,
                scene_sha256=sha256(scene_path) if scene else None)


def _scene(scene, name, starts):
    b, w = [], []
    kind = scene.get('scene_type')
    if scene.get('schema') != SCENE_SCHEMA or scene.get('recorder_session') != name:
        b.append('scene manifest does not belong to this session')
    if kind not in BACKGROUND + POSITIVE:
        b.append('scene_type missing or unsupported')
    if scene.get('partition') not in PARTITIONS:
        b.append('partition missing or unsupported')
    for key in ('operator', 'environment_id', 'collection_day', 'collection_group'):
        if not isinstance(scene.get(key), str) or not scene[key].strip():
            b.append(f'{key} missing')
    rx = scene.get('receiver') or {}
    if not all(isinstance(rx.get(k), str) and rx[k].strip() for k in ('antenna_description', 'antenna_position')):
        b.append('receiver antenna description/position missing')
    systems = scene.get('own_drone_systems')
    if not isinstance(systems, list):
        b.append('own_drone_systems must be a list (empty if none exist on site)'); systems = []
    for s in systems:
        if (not s.get('model') or not s.get('physical_unit') or s.get('aircraft_state') not in STATES
                or s.get('controller_state') not in STATES):
            b.append('own drone system needs model, physical_unit and both states')
    equipment = scene.get('nearby_equipment')
    if not isinstance(equipment, list) or any(not e.get('kind') or not e.get('state') for e in equipment or []):
        b.append('nearby_equipment entries need kind and state (use an empty list only for none)')
    notes = scene.get('timeline') or []
    times = [_time(n.get('time')) for n in notes]
    if not notes or any(t is None or not n.get('note') for t, n in zip(times, notes)):
        b.append('timeline needs at least one timestamped note with UTC offset')
    elif starts and not any(min(starts).timestamp() - 600 <= t.timestamp() <= max(starts).timestamp() + 600 for t in times):
        b.append('no timeline note falls within ten minutes of the recording')
    unc = scene.get('uncontrolled_drone_activity') or {}
    if unc.get('assessment') not in ('none_observed', 'possible', 'observed') or not unc.get('basis'):
        b.append('uncontrolled_drone_activity needs an assessment and its basis')
    approval = scene.get('approval') or {}
    approved = bool(approval.get('approved_by') and _time(approval.get('approved_at')) and approval.get('statement'))
    all_off = all(s.get('aircraft_state') == 'off' and s.get('controller_state') == 'off' for s in systems)
    any_on = any('on' in str(s.get('aircraft_state')) or 'on' in str(s.get('controller_state')) for s in systems)
    candidate = 'unlabelled_stress'
    if kind in BACKGROUND:
        if not all_off:
            b.append('background scene lists a drone system that is not off')
        elif unc.get('assessment') == 'none_observed' and approved:
            candidate = 'scoped_negative_candidate'
        else:
            w.append('not a negative candidate: needs none_observed with basis and approval')
    elif kind in POSITIVE:
        if not any_on:
            b.append('positive scene lists no active drone system')
        elif kind == 'controller_only' and any(s.get('aircraft_state') != 'off' for s in systems):
            b.append('controller_only scene requires every aircraft off')
        elif approved:
            candidate = 'controller_only_activity_candidate' if kind == 'controller_only' else 'drone_system_activity_candidate'
        else:
            w.append('not a positive candidate: approval missing')
    return dict(blockers=b, warnings=w, candidate=candidate,
                summary=dict(scene_type=kind, partition=scene.get('partition'), environment_id=scene.get('environment_id'),
                             collection_day=scene.get('collection_day'), collection_group=scene.get('collection_group'),
                             approved=approved, models=sorted({str(s.get('model')) for s in systems}),
                             units=sorted({str(s.get('physical_unit')) for s in systems})))


def collection(reports):
    """Cross-session split and exposure checks over passing validation reports."""
    blockers, seen = [], {}
    hours = defaultdict(lambda: dict(iq_hours=0.0, wall_hours=0.0, sessions=0, groups=set(), days=set()))
    settings = defaultdict(set)
    for r in reports:
        if not r['passed']:
            blockers.append(f"{r['session']}: failed validation"); continue
        sc = r['scene']
        for key in ('collection_group', 'collection_day', 'environment_id'):
            # Sessions that share a group, day or environment are not independent.
            k = (key, sc[key])
            if seen.setdefault(k, sc['partition']) != sc['partition']:
                blockers.append(f"{r['session']}: {key} {sc[key]!r} crosses partitions")
        h = hours[(sc['partition'], r['label_candidate'])]
        h['iq_hours'] += r['iq_seconds'] / 3600; h['wall_hours'] += r['wall_span_seconds'] / 3600
        h['sessions'] += 1; h['groups'].add(sc['collection_group']); h['days'].add(sc['collection_day'])
        polarity = 'negative' if r['label_candidate'] == 'scoped_negative_candidate' else \
                   'positive' if str(r['label_candidate']).endswith('activity_candidate') else None
        if polarity:
            settings[(r['rate_hz'], r['gain_db'], r['antenna'], tuple(r['centres_hz']))].add(polarity)
    unpaired = [dict(rate_hz=k[0], gain_db=k[1], antenna=k[2], centres=len(k[3]), only=sorted(v)[0])
                for k, v in settings.items() if len(v) == 1]
    table = {f'{p}/{c}': dict(iq_hours=round(v['iq_hours'], 4), wall_hours=round(v['wall_hours'], 4),
                              sessions=v['sessions'], independent_groups=len(v['groups']), days=len(v['days']))
             for (p, c), v in sorted(hours.items(), key=str)}
    if unpaired:
        blockers.append('receiver settings seen with only one label polarity (receiver-to-label shortcut risk)')
    return dict(schema='rfmon.drone_ml.x310_collection.v1', passed=not blockers, blockers=blockers,
                exposure=table, unpaired_receiver_settings=unpaired,
                note='wall_hours is first-to-last segment start per session, a lower bound on monitored time')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    t = sub.add_parser('template'); t.add_argument('session', type=Path)
    c = sub.add_parser('check'); c.add_argument('session', type=Path)
    c.add_argument('--verify-hashes', action='store_true'); c.add_argument('--output', type=Path)
    k = sub.add_parser('collection'); k.add_argument('root', type=Path); k.add_argument('--output', type=Path)
    a = p.parse_args()
    if a.command == 'template':
        target = a.session / 'scene.json'
        if target.exists():
            raise SystemExit('scene.json already exists; not overwritten')
        write_json(target, template(a.session)); print(f'wrote {target}; complete it before running check')
        return
    if a.command == 'check':
        result = check(a.session, a.verify_hashes)
    else:
        dirs = sorted(x.parent for x in a.root.glob('*/session.json'))
        if not 1 <= len(dirs) <= 2000:
            raise SystemExit('expected 1-2000 sessions under the root')
        result = collection([check(x) for x in dirs])
    if a.output:
        if a.output.exists():
            raise SystemExit('output exists; not overwritten')
        write_json(a.output, result)
    print(json.dumps(dict(passed=result['passed'], blockers=result['blockers'],
                          **({'label_candidate': result['label_candidate']} if a.command == 'check' else
                             {'exposure': result['exposure']})), indent=2))
    raise SystemExit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
