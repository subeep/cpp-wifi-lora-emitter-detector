#!/usr/bin/env python3
"""Read-only bounded full-capture research replay; no radio, RID or training.

Overlapping windows stay in one development session. Their counts are not
independent encounters. Recorder continuity/state declarations remain context.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time


def window_starts(samples, rate):
    span = round(rate * 64e-6)
    if not 16 <= span <= 64000 or samples < span:
        raise ValueError('unsupported rate or insufficient complete chirp span')
    stride = 65536 - span
    return range(0, samples - span + 1, stride)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--tool', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--workers', type=int, default=2, choices=range(1, 5))
    p.add_argument('--refine', action='store_true')
    args = p.parse_args()
    output, tool = args.output.resolve(), args.tool.resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest = json.loads(args.manifest.read_text())
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    summary = {'scope':'full-file bounded overlapping development replay, not independent family validation',
               'refinement_requested':args.refine, 'radio_opened':False, 'model_trained':False,
               'remote_id_decoded':False, 'classification_enabled':False, 'recordings':[]}
    started = time.monotonic()
    for record in manifest['captures']:
        path, rate = Path(record['path']), record['sample_rate_hz']
        before = path.stat()
        if record['format'] != 'cf32_le' or before.st_size % 8:
            raise ValueError('expected aligned cf32_le recording')
        total = before.st_size // 8
        starts = window_starts(total, rate)
        def run(offset):
            count = min(65536, total-offset)
            command = [str(tool), 'analyze', '--input', str(path), '--format', record['format'],
                '--sample-rate', str(rate), '--center', str(record['center_frequency_hz']),
                '--offset', str(offset), '--samples', str(count), '--frames', '256',
                '--alpha-bins', '1', '--assess-evidence']
            if args.refine:
                command += ['--refine-chirps']
            measured = json.loads(subprocess.run(command, check=True, capture_output=True, text=True, timeout=15).stdout)
            assert measured['classification']['link_family'] == 'unknown'
            with path.open('rb') as f:
                f.seek(offset*8)
                digest = hashlib.sha256(f.read(count*8)).hexdigest()
            r = {'offset_samples':offset, 'samples':count, 'sha256_iq_window':digest,
                 'source_group':record['source_group'], 'features':measured['features'],
                 'measurement':measured['measurement'], 'classification':measured['classification'],
                 'dsp_evidence':measured['dsp_evidence'], 'chirp_structure':measured['chirp_structure'],
                 'ofdm_structure':measured['ofdm_structure']}
            if args.refine:
                r['chirp_refinement'] = measured['chirp_refinement']
            return r
        statuses, patterns = Counter(), Counter()
        best, refinements = {}, {}
        file = output/(record['id']+'_windows.ndjson')
        with ThreadPoolExecutor(max_workers=args.workers) as pool, file.open('w') as f:
            for index, r in enumerate(pool.map(run, starts), 1):
                f.write(json.dumps(r, allow_nan=False)+'\n')
                for c in r['dsp_evidence']['candidates']:
                    statuses[c['kind']+':'+c['status']] += 1
                    if c['pattern_consistent']:
                        patterns[c['kind']] += 1
                for c in r['chirp_structure']['candidates']:
                    if c['status']=='measured' and (c['label'] not in best or c['peak_coherence_squared'] > best[c['label']]['peak_coherence_squared']):
                        best[c['label']] = c
                if args.refine:
                    for c in r['chirp_refinement']['candidates']:
                        if c['status']=='measured' and (c['label'] not in refinements or c['dechirped_band_power_fraction'] > refinements[c['label']]['dechirped_band_power_fraction']):
                            refinements[c['label']] = c
                if index % 100 == 0:
                    print(record['id'], index, '/', len(starts), flush=True)
        after = path.stat()
        h = hashlib.sha256()
        with path.open('rb') as f:
            while data := f.read(1048576):
                h.update(data)
        assert h.hexdigest() == record['sha256'], 'full-file checksum changed'
        assert (before.st_size,before.st_mtime_ns,before.st_ino)==(after.st_size,after.st_mtime_ns,after.st_ino)
        result = {'id':record['id'], 'operating_state':record['operating_state'], 'source_group':record['source_group'],
                  'samples':total, 'duration_seconds':total/rate, 'windows':len(starts),
                  'window_samples_limit':65536, 'window_stride':starts.step, 'overlap_samples':round(rate*64e-6),
                  'whole_file_finite_selected_windows':True, 'full_file_sha256':h.hexdigest(),
                  'original_size_mtime_inode_unchanged':True, 'originals_modified':False,
                  'statuses':dict(statuses), 'patterns':dict(patterns), 'best_legacy_chirps':best,
                  'best_refinements':refinements, 'passband_verified':False,
                  'independent_test':False, 'family_precision_recall':None, 'window_output':file.name}
        summary['recordings'].append(result)
        (output/'summary.json').write_text(json.dumps(summary, indent=2, allow_nan=False)+'\n')
        print(record['id'], 'patterns:',dict(patterns),'chirp maxima:',{k:v['peak_coherence_squared'] for k,v in best.items()}, flush=True)
    summary['elapsed_seconds'] = time.monotonic()-started
    (output/'summary.json').write_text(json.dumps(summary, indent=2, allow_nan=False)+'\n')
    print('Finished',sum(r['windows'] for r in summary['recordings']),'windows',flush=True)


if __name__ == '__main__':
    main()
