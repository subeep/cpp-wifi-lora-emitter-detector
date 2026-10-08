#!/usr/bin/env python3
"""Grouped DSP breadth/control benchmark. No training, radios or drone truth.

Synthetic derivatives share groups. Received Wi-Fi crops share one session.
Patterns in ordinary traffic/tones are confusables, not drone identifications.
"""
import argparse
import cmath
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
RATE, N = 20_000_000, 32768


def noise(seed, n=N):
    rng = random.Random(seed)
    return [complex(rng.gauss(0, 1), rng.gauss(0, 1)) for _ in range(n)]


def states(seed, kind, period=64, carrier=3e6, deviation=.5e6):
    rng = random.Random(seed)
    phase, bit, x = 0, 0, []
    for j in range(N):
        if kind == 'dsss' and j % (period*11) == 0:
            bit = rng.randrange(2)
        elif kind != 'dsss' and j % period == 0:
            bit = rng.randrange(2 if kind in ('fsk', 'ook', 'bpsk', 'dsss') else 4)
        if kind == 'fsk':
            phase += 2*math.pi*(carrier + (-deviation if bit == 0 else deviation))/RATE
            z = cmath.exp(1j*phase)
        else:
            z = cmath.exp(2j*math.pi*carrier*j/RATE)
            if kind == 'ook':
                z *= bit
            elif kind == 'dsss':
                barker11 = (1, 1, 1, -1, -1, -1, 1, -1, -1, 1, -1)
                z *= (1 if bit else -1)*barker11[(j//period)%11]
            else:
                z *= cmath.exp(1j*math.pi*bit/(2 if kind == 'qpsk' else 1))
        x.append(z)
    return x


def cases():
    for seed in range(40):
        yield f'noise_{seed}', 'white_noise', f'noise:{seed}', noise(440021+seed), 19e6
    for seed in range(8):
        x, previous = [], 0j
        for z in noise(840021+seed):
            previous = .95*previous + z
            x.append(previous)
        yield f'colored_{seed}', 'colored_noise', f'colored:{seed}', x, 19e6
    for carrier in (-9.4e6, -3e6, 0, 3e6, 9.4e6):
        x = [cmath.exp(2j*math.pi*carrier*j/RATE) for j in range(N)]
        yield f'tone_{carrier}', 'tone_confusable', f'tone:{carrier}', x, 19e6
    for gap in (.2e6, 2e6):
        x = [cmath.exp(2j*math.pi*1e6*j/RATE)+.7*cmath.exp(2j*math.pi*(1e6+gap)*j/RATE) for j in range(N)]
        yield f'beat_{gap}', 'tone_beat_confusable', f'beat:{gap}', x, 19e6
    for kind in ('fsk', 'ook', 'bpsk', 'qpsk', 'dsss'):
        for seed in range(3):
            group = f'analytic:{kind}:{seed}'
            x = states(97331+seed, kind, period=8 if kind=='dsss' else 64)
            yield f'{kind}_{seed}', kind+'_control', group, x, 16e6
            added = noise(58317+seed)
            yield f'{kind}_{seed}_noise10', kind+'_noise10', group, [z+.2236068*n for z,n in zip(x,added)], 16e6
        # Impairments stay with their base source group, never a held-out unit.
        x = states(97331, kind, period=8 if kind=='dsss' else 64)
        yield kind+'_echo', kind+'_echo', f'analytic:{kind}:0', [z+(.5*cmath.exp(.7j)*x[j-17] if j>=17 else 0) for j,z in enumerate(x)], 16e6
    for seed in range(6):
        alpha = (73.25+seed*7.5)*RATE/8192
        x = [z*(1+.8*math.cos(2*math.pi*alpha*j/RATE)) for j,z in enumerate(noise(619331+seed))]
        yield f'periodic_envelope_{seed}', 'periodic_envelope_control', f'envelope:{seed}', x, 19e6
        changed = x[:N//2] + noise(819331+seed, N//2)
        yield f'discovery_only_{seed}', 'discovery_only_control', f'envelope:{seed}', changed, 19e6
    x = states(97331, 'fsk')
    tone = [cmath.exp(2j*math.pi*2e6*j/RATE) for j in range(N)]
    for gain in (.3, 2):
        yield f'fsk_tone_{gain}', 'unresolved_mixture', 'analytic:fsk:0', [z+gain*t for z,t in zip(x,tone)], 16e6


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output, tool = args.output.resolve(), args.tool.resolve()
    output.mkdir(parents=True, exist_ok=False)
    hashes = {str(p.relative_to(ROOT)): digest(p) for p in (
        Path(__file__), ROOT/'src/cyclostationary/waveform_features.cpp', ROOT/'src/cyclostationary/candidate_bands.cpp')}
    records, timings = [], []

    def evaluate(name, category, group, path, width, samples, provenance):
        checksum = digest(path)
        base = [str(tool), 'analyze', '--input', str(path), '--format', 'cf32_le', '--sample-rate', str(RATE),
                '--samples', str(samples), '--frames', '256']
        old = json.loads(subprocess.run(base, capture_output=True, text=True, check=True, timeout=20).stdout)
        command = base + ['--expand-waveforms', '--prepare-candidates']
        if width:
            command += ['--usable-bandwidth', str(width)]
        start = time.monotonic()
        expanded = json.loads(subprocess.run(command, capture_output=True, text=True, check=True, timeout=20).stdout)
        elapsed = (time.monotonic()-start)*1000
        # Compare every old field, including quality warnings and classification.
        preserved = all(expanded[k] == v for k,v in old.items())
        if not preserved or digest(path) != checksum:
            raise RuntimeError('legacy output/source preservation failed: '+name)
        w, prep = expanded['waveform_features'], expanded['candidate_preparation']
        assertions = {
            'source_preserved': True, 'all_legacy_json_fields_equal': preserved,
            'classification_unknown': expanded['classification']['link_family']=='unknown',
            'bounded_search': w['fft_calls']<=8 and w['refinement_evaluations']<=40 and len(w['cyclic_peaks'])<=8,
            'bounded_preparation': len(prep['candidates'])<=2 and prep['fir_operations']<=64_000_000,
        }
        if not all(assertions.values()):
            raise RuntimeError('scope/bound assertion failed: '+name)
        records.append({'id': name, 'category': category, 'source_group': group, 'provenance': provenance,
                        'input_sha256': checksum, 'sample_rate_hz': RATE, 'samples': expanded['input']['samples_read'],
                        'declared_width_hz': width, 'roundtrip_ms': elapsed, 'checks': assertions,
                        'waveform_features': w, 'candidate_preparation': prep, 'classification': expanded['classification']})
        timings.append(elapsed)

    with tempfile.TemporaryDirectory(prefix='rfmon-waveform-benchmark-') as directory:
        path = Path(directory)/'control.cf32'
        for name, category, group, x, width in cases():
            path.write_bytes(b''.join(struct.pack('<ff',z.real,z.imag) for z in x))
            evaluate(name, category, group, path, width, len(x), 'analytic_software_control_not_drone_truth')
    for path in sorted((ROOT/'tests/fixtures/wifi_ofdm').glob('*.cf32')):
        evaluate(path.stem, 'received_wifi_confusable', 'x310:2026-09-19:received-wifi', path, None,
                 min(65536,path.stat().st_size//8), 'received_wifi_crop_not_certified_drone_free')

    totals = defaultdict(Counter)
    for record in records:
        w = record['waveform_features']
        counts = totals[record['category']]
        counts['cases'] += 1
        counts['ordinary_persistent_cases'] += any(p['kind']=='ordinary' and p['persistent_pattern'] for p in w['cyclic_peaks'])
        counts['conjugate_persistent_cases'] += any(p['kind']=='conjugate' and p['persistent_pattern'] for p in w['cyclic_peaks'])
        counts['two_frequency_cases'] += w['morphology']['two_frequency_pattern']
        counts['two_level_envelope_cases'] += w['morphology']['two_level_envelope_pattern']
        counts['prepared_candidates'] += sum(c['status']=='prepared_declared_passband' for c in record['candidate_preparation']['candidates'])
        counts['prepared_two_frequency_cases'] += any(c.get('waveform_features',{}).get('morphology',{}).get('two_frequency_pattern',False) for c in record['candidate_preparation']['candidates'])
        counts['prepared_two_level_envelope_cases'] += any(c.get('waveform_features',{}).get('morphology',{}).get('two_level_envelope_pattern',False) for c in record['candidate_preparation']['candidates'])
    timings.sort()
    summary = {'schema': 'rfmon.cyclo.waveform_benchmark.v1', 'method': 'bounded_caf_morphology_v1',
               'partition': 'software_development_controls', 'thresholds_frozen_before_run': True,
               'independent_real_unit_session_test': False, 'radio_opened': False, 'model_trained': False,
               'remote_id_decoded': False, 'named_family_acceptance_enabled': False, 'family_precision_recall': None,
               'source_code_sha256': hashes, 'tool_sha256': digest(tool), 'cases': len(records),
               'source_groups': len({r['source_group'] for r in records}), 'all_preservation_checks_pass': True,
               'timing_scope': 'offline_process_start_json_existing_and_new_dsp_candidate_preparation_not_live_adapter',
               'roundtrip_ms': {'median': timings[len(timings)//2], 'p95': timings[min(len(timings)-1, math.ceil(.95*len(timings))-1)], 'max': max(timings)},
               'category_counts': {k:dict(v) for k,v in totals.items()}, 'records': records}
    (output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k not in ('records','source_code_sha256')},indent=2))


if __name__ == '__main__':
    main()
