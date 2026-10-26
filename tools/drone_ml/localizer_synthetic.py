"""Deterministic synthetic diagnostics with explicit, non-drone signal truth."""
import argparse
import json
from pathlib import Path
import time

import numpy as np

from .common import new_output, write_json
from .localizer_audit import score_regions
from .rf_localizer import LocalizerConfig, localize


def noise(rng, n):
    return (rng.normal(size=n)+1j*rng.normal(size=n))/np.sqrt(2)


def fixture(seed, snr_db=10., rate=20_000_000, seconds=.02):
    rng = np.random.default_rng(seed); n = round(rate*seconds)
    x = noise(rng, n); truth = []
    # Wide and narrow simultaneous signals; the wide burst crosses the 10 ms
    # processing boundary. A third narrowband source spans the whole record.
    specs = [(.007, .013, -.2*rate, .18*rate), (.009, .011, .25*rate, .02*rate),
             (0., seconds, .10*rate, .005*rate)]
    for t0, t1, center, bandwidth in specs:
        a, b = round(t0*rate), round(t1*rate); count = b-a
        bins = np.fft.fftfreq(count, 1/rate)
        band = (bins >= center-bandwidth/2) & (bins < center+bandwidth/2)
        signal = np.fft.ifft(np.fft.fft(noise(rng, count))*band)*10**(snr_db/20)
        x[a:b] += signal
        truth.append(dict(start=a, count=count, low_hz=center-bandwidth/2,
                          high_hz=center+bandwidth/2))
    return x.astype('<c8'), truth


def benchmark(config=LocalizerConfig()):
    records = []; rate = 20_000_000; seconds = .02; n = round(rate*seconds)
    for seed in (11, 23, 47, 83, 131):
        for kind in ('white_noise', 'colored_noise', 'signal_10db', 'signal_0db', 'signal_minus5db'):
            if kind.startswith('signal'):
                snr = {'signal_10db':10., 'signal_0db':0., 'signal_minus5db':-5.}[kind]
                x, truth = fixture(seed, snr)
            else:
                x = noise(np.random.default_rng(seed), n); truth = []
                if kind == 'colored_noise':
                    # Receiver-shaped Gaussian noise, no added emitter. This
                    # deliberately challenges the flat-spectrum assumption.
                    bins = np.fft.fftfreq(n, 1/rate)
                    shape = 1+4*np.exp(-.5*((bins+rate*.2)/(rate*.08))**2)
                    x = np.fft.ifft(np.fft.fft(x)*shape)
                x = x.astype('<c8')
            started = time.perf_counter()
            result = localize(lambda s, count:x[s:s+count], n, rate, seconds, config)
            scores = [score_regions(result['regions'], g, rate) for g in truth]
            # Count proposals outside *every* injected rectangle. Splits or
            # oversized overlapping components are not automatically correct.
            unmatched = sum(not any(r['start_sample'] < g['start']+g['count'] and
                r['stop_sample'] > g['start'] and r['low_hz'] < g['high_hz'] and
                r['high_hz'] > g['low_hz'] for g in truth) for r in result['regions'])
            records.append(dict(seed=seed, kind=kind, observed_seconds=result['analysed_samples']/rate,
                regions=len(result['regions']), dropped_regions=result['dropped_regions'],
                synthetic_reference_boxes=truth, matches=scores, unmatched_regions=unmatched,
                matched_at_80pct_area=sum(s is not None and s['reference_area_coverage']>=.8 for s in scores),
                wall_seconds=time.perf_counter()-started))
    summary = {}
    for kind in sorted({r['kind'] for r in records}):
        rs = [r for r in records if r['kind']==kind]
        summary[kind] = dict(records=len(rs), observed_seconds=sum(r['observed_seconds'] for r in rs),
            proposal_count=sum(r['regions'] for r in rs), unmatched_regions=sum(r['unmatched_regions'] for r in rs),
            reference_count=sum(len(r['matches']) for r in rs),
            matched_at_80pct_area=sum(r['matched_at_80pct_area'] for r in rs))
    return dict(schema='rfmon.drone_ml.localizer_synthetic.v1', config=result['config'], records=records,
        summary=summary, drone_false_alarm_rate=None,
        note='Seeded Gaussian synthetic signals only. SNR is added signal/noise PSD within occupied bins, '
             'not whole-band SNR. Colored noise intentionally tests receiver-response ambiguity. '
             'Tiny observation duration cannot establish deployment false-alarm rates.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True); args = parser.parse_args()
    out = new_output(args.output, [])
    result = benchmark(); write_json(out/'report.json', result)
    print(json.dumps(result['summary'], indent=2))


if __name__ == '__main__':
    main()
