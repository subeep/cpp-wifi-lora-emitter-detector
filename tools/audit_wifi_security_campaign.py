#!/usr/bin/env python3
"""Read-only regression audit of a saved conducted Wi-Fi security campaign.

No radio access. Results are development regression evidence, not independent
attack recall, transmitter recall, false-positive calibration or client impact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

PHASES = [('baseline', None),
          ('historical_replay', 'historical_beacon_after_newer'),
          ('target_distinct', 'distinct_management_frames'),
          ('target_repeated', 'repeated_management_frames'),
          ('ap_distinct', 'ap_wide_distinct_management_frames'),
          ('ap_repeated', 'ap_wide_repeated_management_frames'),
          ('retry_negative', None)]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def semantic(snapshot):
    # UI-tail capacity and recorder accounting differ between live and offline.
    return {k: v for k, v in snapshot.items() if k not in ('recent', 'recorded_bytes')}


def audit(tool, campaign):
    cases = []
    hashes = {}
    previous = {}
    for phase, expected_path in PHASES + [('final', None)]:
        recording = campaign / ('receiver/events.ndjson' if phase == 'final' else phase+'-events.ndjson')
        reference = campaign / ('receiver/snapshot.json' if phase == 'final' else phase+'-snapshot.json')
        hashes[str(recording)] = digest(recording)
        hashes[str(reference)] = digest(reference)
        result = subprocess.run([str(tool), str(recording)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(f'{phase}: replay failed: {result.stderr}')
        current = json.loads(result.stdout)
        recorded = json.loads(reference.read_text())
        projected = semantic(current)
        added = {}
        # An older snapshot has no transaction diagnostics. Compare every old
        # semantic field, including all incidents; retain the additive field.
        if 'transactions' not in recorded and 'transactions' in projected:
            added['transactions'] = projected.pop('transactions')
        diff = sorted(k for k in projected.keys() | semantic(recorded).keys()
                      if projected.get(k) != semantic(recorded).get(k))
        observations = {str(i['id']): i['observations'] for i in current['incidents']}
        paths = {i['path'] for i in current['incidents']} | {t['path'] for i in current['incidents'] for t in i['timeline']}
        # A path must gain a new observation in its own phase, not merely remain
        # in an earlier incident's retained timeline.
        path_advanced = expected_path is None or any(
            i['observations'] > previous.get(str(i['id']), 0) and
            i['path'] == expected_path
            for i in current['incidents'])
        negative_ok = (current['incidents_total'] == 0 if phase == 'baseline' else
                       observations == previous if phase == 'retry_negative' else True)
        clean = (not current['input_lines_rejected'] and not current['queue_events_dropped'] and
                 not current['queue_captures_dropped'] and all(
                     not c['overflow_captures'] and not c['failed_captures'] and not c['burst_cap_captures']
                     for c in current['coverage']))
        cases.append(dict(phase=phase,passed=not diff and path_advanced and negative_ok and clean,
                          semantic_difference_keys=diff,added_snapshot_fields=added,expected_path=expected_path,
                          path_advanced=path_advanced,negative_control_passed=negative_ok,
                          clean_coverage=clean,frames_accepted=current['frames_accepted'],
                          incidents=current['incidents_total'],observations=observations,paths=sorted(paths)))
        previous = observations
    # Detect accidental input mutation during this run.
    unchanged = all(digest(Path(p)) == h for p, h in hashes.items())
    return dict(schema='wifi_security_campaign_audit.v1',passed=unchanged and all(c['passed'] for c in cases),
                executable_sha256=digest(tool),input_sha256=hashes,inputs_unchanged=unchanged,
                hardware_operated=False,independent_validation=False,thresholds_calibrated=False,
                cases=cases)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool',type=Path,required=True)
    p.add_argument('--campaign',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a = p.parse_args()
    if a.output.resolve().is_relative_to(a.campaign.resolve()):
        p.error('output must be outside the original campaign')
    if a.output.exists():p.error('output already exists; preserve previous audits')
    report = audit(a.tool.resolve(), a.campaign.resolve())
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(report,indent=2)+'\n')
    for c in report['cases']:print(c['phase'], 'PASS' if c['passed'] else 'FAIL', c['semantic_difference_keys'])
    if not report['passed']:raise SystemExit(1)

if __name__ == '__main__':main()
