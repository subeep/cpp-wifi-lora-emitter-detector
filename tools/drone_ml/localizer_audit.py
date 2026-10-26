"""Run the offline IQ-only localizer, then separately score weak references.

The run command has no annotation input. The compare command reads completed
proposals and previously audited report geometry, never IQ or model weights.
"""
import argparse
import json
from pathlib import Path
import time

from .common import new_output, read_source, sha256, stamp, write_json
from .rf_localizer import LocalizerConfig, localize
from .temporal_audit import check_record, reader


def load(path):
    with read_source(path) as f:
        return json.load(f)


def audit(manifest_path, output, seconds=.1, config=None, algorithm='baseline'):
    if algorithm == 'baseline':
        run_localizer = localize
        config = config if config is not None else LocalizerConfig()
        if type(config) is not LocalizerConfig:
            raise ValueError('baseline requires LocalizerConfig')
    elif algorithm in ('multiscale','multiscale_lower_tail'):
        from .rf_localizer_multiscale import MultiscaleConfig, localize as run_localizer
        config = config if config is not None else (MultiscaleConfig() if algorithm=='multiscale' else
            MultiscaleConfig(reference_method='lower_tail',fast_high_db=10.,fast_low_db=6.))
        if type(config) is not MultiscaleConfig:
            raise ValueError('multiscale requires MultiscaleConfig')
        if config.reference_method != ('median' if algorithm=='multiscale' else 'lower_tail'):
            raise ValueError('algorithm and reference method mismatch')
    else:
        raise ValueError('unknown localizer algorithm')
    manifest_path = Path(manifest_path)
    before = {manifest_path: stamp(manifest_path)}
    manifest_hash = sha256(manifest_path)
    manifest = load(manifest_path)
    if manifest.get('schema') != 'rfmon.drone_ml.temporal_sources.v1':
        raise ValueError('invalid explicit source manifest')
    rows = manifest.get('records', [])
    if not 1 <= len(rows) <= 32:
        raise ValueError('source count budget')
    config.validate()
    for row in rows:
        check_record(row)  # All partition checks before any IQ path access.
    out = new_output(output, [manifest_path.parent, *[Path(r['path']).parent for r in rows]])
    hashes = {}; results = []
    for index, row in enumerate(rows):
        path = Path(row['path']).resolve()
        if path not in hashes:
            before[path] = stamp(path); hashes[path] = sha256(path)
        if hashes[path] != row['source_sha256']:
            raise ValueError('source checksum mismatch')
        started = time.perf_counter()
        with reader(row) as (available, get):
            result = run_localizer(get, available, row['sample_rate_hz'], seconds, config)
        result['source'] = row
        elapsed = time.perf_counter()-started
        result['timing'] = dict(wall_seconds=elapsed,
            wall_seconds_per_observed_second=elapsed/(result['analysed_samples']/row['sample_rate_hz']),
            includes='source open/read, FFT, CPU proposal extraction and boundary merging',
            excludes='full source checksum, model inference, acquisition, plotting; not live throughput')
        results.append(result)
        write_json(out/'progress.json', dict(completed_records=index+1, total_records=len(rows)))
    if any(stamp(p) != s for p, s in before.items()):
        raise ValueError('source or manifest changed during audit')
    report = dict(schema='rfmon.drone_ml.localizer_audit.v1', source_manifest_sha256=manifest_hash,
        algorithm=algorithm, records=results, source_stats_preserved=True, source_hashes_verified=True,
        final_test_iq_read=False, reference_coordinates_used=False, labels_created=False,
        model_inference_performed=False, live_changes=False,
        limitations=[
            'RF energy regions are not packets, emitters, drones or training labels.',
            'Relative quantile contrasts are not calibrated physical SNR or drone probabilities.',
            'Uniform continuous full-band energy can be indistinguishable from noise.',
            'Colored receiver response, interference and DC can produce or hide regions.',
            'Thresholds, smoothing, finite FFT resolution and chunk boundaries can miss weak/short signals.',
            'Intersecting components can merge emitters; proposal truncation is reported explicitly.',
            'Retaining a full-band fallback is a requirement, not a completed fallback classifier.',
            'Source continuity and label limitations are inherited; empty proposals never certify absence.'])
    write_json(out/'report.json', report)
    return report


def score_regions(regions, geometry, rate):
    """Best rectangular overlap, not a semantic detection correctness metric."""
    a, b = geometry['start'], geometry['start']+geometry['count']
    low, high = geometry['low_hz'], geometry['high_hz']
    reference_area = (b-a)*(high-low)
    scored = []
    for r in regions:
        time_overlap = max(0, min(b, r['stop_sample'])-max(a, r['start_sample']))
        freq_overlap = max(0, min(high, r['high_hz'])-max(low, r['low_hz']))
        if time_overlap <= 0 or freq_overlap <= 0:
            continue
        area = time_overlap*freq_overlap
        candidate_area = (r['stop_sample']-r['start_sample'])*(r['high_hz']-r['low_hz'])
        view = r['proposed_view']; center = view['offset_hz']; width = view['rate_hz']
        view_overlap = max(0, min(high, center+width/2)-max(low, center-width/2))
        scored.append(dict(proposal_id=r['id'], reference_area_coverage=area/reference_area,
            rectangle_iou=area/(reference_area+candidate_area-area),
            reference_time_coverage=time_overlap/(b-a), reference_frequency_coverage=freq_overlap/(high-low),
            proposed_view_reference_frequency_coverage=view_overlap/(high-low),
            proposed_view=view))
    return max(scored, key=lambda s: (s['reference_area_coverage'], s['rectangle_iou'])) if scored else None


def compare(localizer_path, references_path, output):
    paths = list(map(Path, (localizer_path, references_path)))
    before = {p: stamp(p) for p in paths}; hashes = {str(p): sha256(p) for p in paths}
    proposals, references = map(load, paths)
    if (proposals.get('schema') != 'rfmon.drone_ml.localizer_audit.v1'
            or references.get('schema') != 'rfmon.drone_ml.frequency_view_audit.v1'):
        raise ValueError('incompatible completed audit schemas')
    if proposals.get('reference_coordinates_used') is not False:
        raise ValueError('requires IQ-only proposals')
    rows = []
    for ref in references['records']:
        source = ref['source']; check_record(source)
        matches = [r for r in proposals['records'] if r['source']['path'] == source['path']
                   and r['source']['source_sha256'] == source['source_sha256']
                   and r['source']['sample_rate_hz'] == source['sample_rate_hz']]
        if len(matches) != 1:
            raise ValueError('reference source does not match exactly one proposal source')
        result = matches[0]
        for region in ref['regions']:
            g = region['geometry']
            if g['start']+g['count'] > result['analysed_samples']:
                raise ValueError('reference outside analyzed interval')
            rows.append(dict(source=source['path'], reference=region['reference'],
                best=score_regions(result['regions'], g, source['sample_rate_hz'])))
    report = dict(schema='rfmon.drone_ml.localizer_reference_comparison.v1', input_sha256=hashes,
        regions=rows, reference_count=len(rows),
        any_rectangle_overlap=sum(r['best'] is not None for r in rows),
        area_coverage_at_least_80pct=sum(r['best'] is not None and
                                       r['best']['reference_area_coverage'] >= .8 for r in rows),
        view_covers_frequency_and_80pct_time=sum(r['best'] is not None and
            r['best']['proposed_view_reference_frequency_coverage'] >= .999999 and
            r['best']['reference_time_coverage'] >= .8 for r in rows),
        detection_recall=None, false_positive_rate=None, independent_evaluation=False,
        note='Weak, selectively reported energy boxes; unmatched proposals are not false positives. '
             'Rectangle overlap and proposed-view containment do not establish drone identity or recall.')
    if any(stamp(p) != s for p, s in before.items()):
        raise ValueError('audit changed during comparison')
    out = new_output(output, [p.parent for p in paths])
    write_json(out/'comparison.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    run = sub.add_parser('run'); run.add_argument('--manifest', type=Path, required=True)
    run.add_argument('--output', type=Path, required=True); run.add_argument('--seconds', type=float, default=.1)
    run.add_argument('--algorithm', choices=['baseline','multiscale','multiscale_lower_tail'], default='baseline')
    comp = sub.add_parser('compare'); comp.add_argument('--localizer', type=Path, required=True)
    comp.add_argument('--references', type=Path, required=True); comp.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'run':
        result = audit(args.manifest, args.output, args.seconds, algorithm=args.algorithm)
        print(json.dumps([dict(path=r['source']['path'], regions=len(r['regions']),
                               dropped=r['dropped_regions'], seconds=r['timing']['wall_seconds'])
                          for r in result['records']], indent=2))
    else:
        result = compare(args.localizer, args.references, args.output)
        print(json.dumps({k:v for k,v in result.items() if k != 'regions'}, indent=2))


if __name__ == '__main__':
    main()
