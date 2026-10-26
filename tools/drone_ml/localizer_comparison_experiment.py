"""Paired synthetic comparison; no data-derived labels or live promotion."""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import time

import numpy as np

from .common import new_output, sha256, write_json
from .localizer_audit import score_regions
from .localizer_synthetic import fixture, noise
from .rf_localizer import LocalizerConfig, localize as baseline
from .rf_localizer_multiscale import MultiscaleConfig, localize as multiscale


def cases(seed):
    rate = 20_000_000; n = 400000
    for kind in ('white_noise', 'colored_noise', 'noise_gain_step', 'signal_10db', 'signal_0db', 'signal_minus5db'):
        truth = []
        if kind.startswith('signal'):
            snr = {'signal_10db':10., 'signal_0db':0., 'signal_minus5db':-5.}[kind]
            x, truth = fixture(seed, snr)
        else:
            x = noise(np.random.default_rng(seed), n)
            if kind == 'colored_noise':
                bins = np.fft.fftfreq(n, 1/rate)
                x = np.fft.ifft(np.fft.fft(x)*(1+4*np.exp(-.5*((bins+rate*.2)/(rate*.08))**2)))
            elif kind == 'noise_gain_step':
                x[n//3:2*n//3] *= 3
            x = x.astype('<c8')
        yield kind, x, rate, truth
    # Entire-record wide signal: a mandatory temporal-change gate would miss
    # it. It remains ambiguous with shaped noise even if a region is proposed.
    rng = np.random.default_rng(seed); x = noise(rng, n)
    bins = np.fft.fftfreq(n, 1/rate); low, high = -7e6, -3e6
    x += np.fft.ifft(np.fft.fft(noise(rng, n))*((bins>=low)&(bins<high)))*10**(.5)
    yield 'continuous_wide_10db', x.astype('<c8'), rate, [dict(start=0, count=n, low_hz=low, high_hz=high)]
    # Short 31 us activity at a 100 MSps native rate, not a drone/ZC simulation.
    rate = 100_000_000; n = 2_000_000; rng = np.random.default_rng(seed)
    x = noise(rng, n); start = 512000; count = 3100
    bins = np.fft.fftfreq(count, 1/rate); low, high = 20e6, 21e6
    x[start:start+count] += np.fft.ifft(np.fft.fft(noise(rng, count))*((bins>=low)&(bins<high)))*10
    yield 'short_31us_20db', x.astype('<c8'), rate, [dict(start=start, count=count, low_hz=low, high_hz=high)]


def experiment(output):
    out = new_output(output, [])
    recipes = dict(baseline=asdict(LocalizerConfig()), multiscale=asdict(MultiscaleConfig()))
    paths = [Path(__file__), Path(__file__).with_name('rf_localizer.py'),
             Path(__file__).with_name('rf_localizer_multiscale.py')]
    code_hashes = {str(p):sha256(p) for p in paths}
    write_json(out/'frozen_settings.json', dict(configurations=recipes, code_sha256=code_hashes,
        development_seed=7, previous_evaluation_seeds=[11,23,47,83,131],
        fresh_evaluation_seeds=[201,311,419,523,631],
        note='Frozen before this run. Fresh seeds are new noise draws from the same synthetic families, not independent field evidence.'))
    rows = []
    for group, seeds in [('previous',[11,23,47,83,131]), ('fresh',[201,311,419,523,631])]:
        for seed in seeds:
            for kind, x, rate, truth in cases(seed):
                for name, runner in [('baseline',baseline),('multiscale',multiscale)]:
                    started = time.perf_counter()
                    result = runner(lambda s,k:x[s:s+k], len(x), rate, len(x)/rate)
                    elapsed = time.perf_counter()-started
                    scores = [score_regions(result['regions'], g, rate) for g in truth]
                    unmatched = sum(not any(r['start_sample']<g['start']+g['count'] and
                        r['stop_sample']>g['start'] and r['low_hz']<g['high_hz'] and
                        r['high_hz']>g['low_hz'] for g in truth) for r in result['regions'])
                    rows.append(dict(seed_group=group, seed=seed, kind=kind, algorithm=name,
                        sample_rate_hz=rate, observed_seconds=result['analysed_samples']/rate,
                        regions=len(result['regions']), unmatched_regions=unmatched,
                        stable_ambiguous_regions=sum(r.get('stable_background_ambiguity',False) for r in result['regions']),
                        dropped_regions=result['dropped_regions'], matches=scores, synthetic_reference_boxes=truth,
                        matched_at_80pct_area=sum(s is not None and s['reference_area_coverage']>=.8 for s in scores),
                        matched_at_50pct_iou=sum(s is not None and s['rectangle_iou']>=.5 for s in scores),
                        wall_seconds=elapsed))
            write_json(out/'progress.json', dict(completed_seed_group=group, completed_seed=seed, rows=len(rows)))
    summary = {}
    for group in ('previous','fresh'):
        summary[group] = {}
        for kind in sorted({r['kind'] for r in rows}):
            summary[group][kind] = {}
            for algorithm in ('baseline','multiscale'):
                selected = [r for r in rows if r['seed_group']==group and r['kind']==kind and r['algorithm']==algorithm]
                summary[group][kind][algorithm] = dict(
                    reference_count=sum(len(r['matches']) for r in selected),
                    matched_at_80pct_area=sum(r['matched_at_80pct_area'] for r in selected),
                    matched_at_50pct_iou=sum(r['matched_at_50pct_iou'] for r in selected),
                    proposals=sum(r['regions'] for r in selected),
                    unmatched=sum(r['unmatched_regions'] for r in selected),
                    stable_ambiguous=sum(r['stable_ambiguous_regions'] for r in selected),
                    wall_seconds=sum(r['wall_seconds'] for r in selected),
                    observed_seconds=sum(r['observed_seconds'] for r in selected))
    if any(sha256(p)!=code_hashes[str(p)] for p in paths):
        raise ValueError('experiment implementation changed during execution')
    report = dict(schema='rfmon.drone_ml.localizer_paired_synthetic.v1', rows=rows, summary=summary,
        configurations=recipes, code_sha256=code_hashes,
        drone_false_alarm_rate=None, field_validation=False, training_performed=False,
        note='Gaussian synthetic activity only; shape/noise/gain changes are ambiguous to an energy localizer. '
             'SNR describes added signal/noise PSD within occupied bins. Both algorithms see identical IQ. '
             'Overlap matching is many-to-one; oversized proposals can cover several true regions.')
    write_json(out/'report.json', report); write_json(out/'summary.json', summary)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--output',type=Path,required=True)
    a = p.parse_args(); r = experiment(a.output); print(json.dumps(r['summary'],indent=2))


if __name__ == '__main__':
    main()
