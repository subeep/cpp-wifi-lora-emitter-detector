#!/usr/bin/env python3
"""Summarise data/drone_offsets.jsonl (carrier-offset measurements written by the app).

DroneID, per serial: packets, mean ppm and spread on each band, 5 GHz minus 2.4 GHz, drift over
the session (straight-line fit, ppm per minute), and the mean on each DroneID channel.
Link grids, per capture centre and spacing: captures, circular mean and spread (Hz), and how far
each 15 kHz grid sits from "a multiple of 5 kHz + centre x the serial's ppm" (the readable serial
with most packets, or --serial; small: the link fits that drone's crystal).

  tools/drone_offset_summary.py [LOG] [--minutes M] [--min-coherence C] [--serial S]
Standard library only. Measurement review; it changes nothing.
"""
import argparse
import cmath
import json
import math
import statistics
import time
from collections import defaultdict
from pathlib import Path


def load(path, minutes):
    lines = []
    for p in (Path(str(path) + '.1'), Path(path)):
        if not p.exists():
            continue
        for raw in p.open():
            try:
                lines.append(json.loads(raw))
            except ValueError:
                continue
    if minutes:
        cutoff = time.time() * 1000 - minutes * 60e3
        lines = [l for l in lines if l.get('unix_ms', 0) >= cutoff]
    return lines


def fit_slope(points):
    """Least-squares slope of (t seconds, ppm); None for fewer than 3 points or no time span."""
    if len(points) < 3:
        return None
    t0 = points[0][0]
    xs = [(t - t0) / 60 for t, _ in points]
    ys = [v for _, v in points]
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    return None if sxx == 0 else sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx


def circular(values, period):
    s = sum(cmath.exp(2j * math.pi * v / period) for v in values)
    mean = (cmath.phase(s) / (2 * math.pi) * period) % period
    r = min(1.0, abs(s) / len(values))
    spread = math.sqrt(-2 * math.log(r)) * period / (2 * math.pi) if r > 0 else math.inf
    return mean, spread


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', nargs='?', default=str(Path(__file__).resolve().parents[1] / 'data/drone_offsets.jsonl'))
    ap.add_argument('--minutes', type=float, default=0, help='only the last M minutes')
    ap.add_argument('--min-coherence', type=float, default=0.9)
    ap.add_argument('--serial', help='fit link grids to this serial (default: the readable serial with most packets)')
    a = ap.parse_args()
    lines = load(a.log, a.minutes)
    packets = [l for l in lines if l.get('kind') == 'droneid']
    links = [l for l in lines if l.get('kind') == 'link']
    print(f'{a.log}: {len(packets)} DroneID packets, {len(links)} link captures')

    by_serial = defaultdict(list)
    unmeasured = 0
    for p in packets:
        if p.get('offset_ppm') is None:
            unmeasured += 1
            continue
        by_serial[p.get('serial') or '(unreadable)'].append(p)
    if unmeasured:
        print(f'  {unmeasured} packets without ppm (found by automatic centring, or no offset)')
    serial_ppm = {}
    print('\nDroneID offset by serial')
    for serial, ps in sorted(by_serial.items(), key=lambda kv: -len(kv[1])):
        ps.sort(key=lambda p: p['unix_ms'])
        model = next((p['model'] for p in ps if p.get('model')), '')
        values = [p['offset_ppm'] for p in ps]
        serial_ppm[serial] = statistics.fmean(values)
        span = (ps[-1]['unix_ms'] - ps[0]['unix_ms']) / 60e3
        print(f'  {serial} {model}: {len(ps)} packets over {span:.1f} min, mean {serial_ppm[serial]:+.4f} ppm'
              f', spread {statistics.stdev(values) if len(values) > 1 else 0:.4f}')
        bands = {}
        for name, hi in (('2.4 GHz', False), ('5 GHz', True)):
            b = [p['offset_ppm'] for p in ps if (p['channel_hz'] >= 3e9) == hi]
            if b:
                bands[name] = statistics.fmean(b)
                print(f'    {name}: {len(b)} packets, mean {bands[name]:+.4f}, spread '
                      f'{statistics.stdev(b) if len(b) > 1 else 0:.4f} ppm')
        if len(bands) == 2:
            print(f'    5 GHz minus 2.4 GHz: {bands["5 GHz"] - bands["2.4 GHz"]:+.4f} ppm')
        slope = fit_slope([(p['unix_ms'] / 1e3, p['offset_ppm']) for p in ps])
        if slope is not None:
            print(f'    drift: {slope:+.4f} ppm per minute (first {values[0]:+.4f}, last {values[-1]:+.4f})')
        channels = defaultdict(list)
        for p in ps:
            channels[p['channel_hz']].append(p['offset_ppm'])
        print('    by channel: ' + ', '.join(f'{c / 1e6:.1f} MHz {statistics.fmean(v):+.4f} ({len(v)})'
                                             for c, v in sorted(channels.items())))

    grids = defaultdict(list)
    for l in links:
        for g in l.get('grid', []):
            if g['coherence'] >= a.min_coherence and (l.get('group') or '') != '':
                grids[(l['center_hz'], g['hypothesis'], g['spacing_hz'])].append(g['hz'])
    print(f'\nLink grid by capture centre (drone timing groups only, coherence >= {a.min_coherence})')
    readable = {k: v for k, v in serial_ppm.items() if k != '(unreadable)'}
    fit_serial = a.serial or (max(readable, key=lambda k: len(by_serial[k])) if readable else None)
    only = serial_ppm.get(fit_serial)
    if only is not None:
        print(f'  fit: distance from (k x 5 kHz + centre x {only:+.4f} ppm of {fit_serial}); '
              f'unrelated signals land anywhere up to 2500 Hz (median about 1250)')
    for (centre, hyp, spacing), values in sorted(grids.items()):
        if hyp.startswith('wifi'):
            continue
        mean, spread = circular(values, spacing)
        line = f'  {centre / 1e6:9.3f} MHz {hyp:18s} {len(values):4d} captures  mean {mean:8.1f} Hz  spread {spread:7.1f} Hz'
        if only is not None and spacing == 15e3:
            # Distance from the nearest (k x 5 kHz + centre x ppm), in ppm of the centre.
            res = [((v - centre * only * 1e-6 + 2500) % 5000) - 2500 for v in values]
            med = statistics.median(abs(r) for r in res)
            line += f'  fit: median {med:6.0f} Hz = {med / centre * 1e6:.3f} ppm'
        print(line)
    if only is None:
        print('  (no readable DroneID serial with ppm to fit link grids to)')


if __name__ == '__main__':
    main()
