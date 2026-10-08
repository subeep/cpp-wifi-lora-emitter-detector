#!/usr/bin/env python3
"""Frozen DSP policy software benchmark. No training, radios or drone truth.

Analytic waveform controls and their impairments share source groups. Received
Wi-Fi crops are confusable waveform controls, not verified drone-free units.
Observations are retained without tuning thresholds to these outcomes.
"""
import argparse
from collections import Counter
import cmath
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
RATE, N = 20_000_000, 8192
POLICY = 'experimental_dsp_v1'


def noise(seed, amplitude=1, n=N):
    r = random.Random(seed)
    return [complex(r.gauss(0, amplitude), r.gauss(0, amplitude)) for _ in range(n)]


def chirp(slope, sweep, carrier=0, clock=1, bandwidth=None):
    x = noise(17039, .001)
    for i in range(1334):
        t = i / RATE * clock
        frequency = carrier - math.copysign(sweep / 2, slope) + slope * t
        if bandwidth and abs(frequency) > bandwidth / 2:
            continue
        x[2000+i] += cmath.exp(2j * math.pi * ((carrier-math.copysign(sweep/2, slope))*t+slope*t*t/2))
    return x


def ofdm():
    r = random.Random(900139)
    x = noise(900140, .001, 37)
    for _ in range(100):
        carriers = [(k, complex(r.choice([-1, 1]), r.choice([-1, 1]))) for k in range(-26, 27) if k]
        symbol = [sum(z*cmath.exp(2j*math.pi*k*n/64) for k, z in carriers)/math.sqrt(52) for n in range(64)]
        x.extend(symbol[-16:] + symbol)
    return x


def cases():
    for seed in range(20):
        yield f'gaussian_{seed}', 'stationary_noise', f'noise:{seed}', noise(30000+seed), 16e6, 'cf32_le'
    for frequency in (0, 1e6, 9.8e6):
        yield f'tone_{frequency}', 'tone', f'tone:{frequency}', [cmath.exp(2j*math.pi*frequency*n/RATE) for n in range(N)], 16e6, 'cf32_le'
    shift = 135.2e9 * 32e-6
    yield 'two_tone', 'two_tone', 'two_tone', [cmath.exp(-1j*math.pi*shift*n/RATE)+cmath.exp(1j*math.pi*shift*n/RATE) for n in range(N)], 16e6, 'cf32_le'
    for mode in ('fsk', 'hop'):
        r, phase, x = random.Random(93017), 0, []
        for n in range(N):
            if n % (20 if mode == 'fsk' else 400) == 0:
                f = r.choice([-2e6, 2e6]) if mode == 'fsk' else r.uniform(-6e6, 6e6)
            phase += 2*math.pi*f/RATE
            x.append(cmath.exp(1j*phase))
        yield mode, mode, mode, x, 16e6, 'cf32_le'
    impulse = noise(7773, .001)
    for n in range(0, N, 157):
        impulse[n] += complex(100, -70)
    yield 'impulse_train', 'impulses', 'impulses', impulse, 16e6, 'cf32_le'
    nonlinear = noise(700139, .001)
    for i in range(1334):
        t = i / RATE
        nonlinear[2000+i] += cmath.exp(2j*math.pi*(-4.5e6*t + 135.2e9*t*t/2 + 2e15*t*t*t/3))
    yield 'nonlinear_chirp', 'nonlinear_chirp', 'nonlinear', nonlinear, 16e6, 'cf32_le'
    for label, slope, sweep in [('up9', 135.2e9, 9e6), ('up18', 270.2e9, 18e6), ('down9', -135.3e9, 9e6)]:
        group = 'analytic:' + label
        width = 19e6 if sweep == 18e6 else 16e6
        base = chirp(slope, sweep)
        yield label+'_clean', 'analytic_chirp', group, base, width, 'cf32_le'
        yield label+'_unknown_passband', 'unknown_passband', group, base, None, 'cf32_le'
        yield label+'_narrow_profile', 'narrow_profile', group, base, 8e6, 'cf32_le'
        for carrier in (-.3e6, .3e6):
            yield f'{label}_cfo_{carrier}', 'cfo', group, chirp(slope, sweep, carrier), width, 'cf32_le'
        for scale in (1e-25, 1e25):
            yield f'{label}_gain_{scale}', 'gain', group, [z*scale for z in base], width, 'cf32_le'
        for ppm in (-500, -50, 50, 500):
            yield f'{label}_clock_{ppm}', 'clock', group, chirp(slope, sweep, clock=1+ppm/1e6), width, 'cf32_le'
        for snr in (-5, 0, 10, 20):
            # SNR at active unit-power chirp, not a calibrated receiver SNR.
            added = noise(80135, math.sqrt(10**(-snr/10)/2))
            yield f'{label}_noise_{snr}', 'added_noise', group, [z+n for z, n in zip(base, added)], width, 'cf32_le'
        yield label+'_dc', 'source_dc', group, [z+complex(1, 2) for z in base], width, 'cf32_le'
        mixed = [z+.5*cmath.exp(2j*math.pi*2e6*n/RATE) for n, z in enumerate(base)]
        yield label+'_tone_mixture', 'tone_mixture', group, mixed, width, 'cf32_le'
        for delay in (3, 20):
            multipath = [z+(.5*cmath.exp(.7j)*base[n-delay] if n >= delay else 0) for n, z in enumerate(base)]
            yield f'{label}_multipath_{delay}', 'multipath', group, multipath, width, 'cf32_le'
        yield label+'_sweep_truncated', 'time_frequency_mask', group, chirp(slope, sweep, bandwidth=sweep*.6), width, 'cf32_le'
        rails = [complex(max(-1, min(1, 2*z.real)), max(-1, min(1, 2*z.imag))) for z in base]
        yield label+'_int16_rails', 'int16_rails', group, rails, width, 'ci16_le'
    yield 'ordinary_ofdm_analytic', 'ordinary_ofdm', 'analytic_ofdm', ofdm(), 19e6, 'cf32_le'
    # Repeated random symbol is periodic, but outside-prefix correlation
    # should prevent the CP matcher from treating it as valid OFDM evidence.
    r = random.Random(63139)
    symbol = [complex(r.gauss(0, 1), r.gauss(0, 1)) for _ in range(80)]
    yield 'repeated_symbol', 'periodic_non_ofdm', 'periodic', (symbol*103)[:N], 19e6, 'cf32_le'


def fresh_cases():
    for seed in range(40):
        yield f'fresh_noise_{seed}', 'fresh_stationary_noise', f'fresh_noise:{seed}', noise(991037+seed), 16e6, 'cf32_le'
    for seed in range(8):
        rng = random.Random(819037+seed)
        f = rng.uniform(-8e6,8e6)
        x = [cmath.exp(2j*math.pi*f*n/RATE)+.01*z for n,z in enumerate(noise(739037+seed))]
        yield f'fresh_tone_{seed}', 'fresh_tone', f'fresh_tone:{seed}', x, 16e6, 'cf32_le'
    for which,(slope,sweep) in enumerate(((135.2e9,9e6),(270.2e9,18e6),(-135.3e9,9e6))):
        width = 19.5e6 if sweep==18e6 else 16e6
        seed = 591037+which
        group = f'fresh_analytic:{which}'
        def generate(clock=1, carrier=0, factor=1):
            x = noise(seed,.001)
            for n in range(1334):
                t = n/RATE*clock
                x[1777+n] += cmath.exp(2j*math.pi*((carrier-math.copysign(sweep/2,slope))*t+factor*slope*t*t/2))
            return x
        base = generate()
        for snr in (5,10,15):
            added = noise(seed+300,math.sqrt(10**(-snr/10)/2))
            yield f'fresh_{which}_noise_{snr}', 'fresh_added_noise', group, [z+n for z,n in zip(base,added)], width, 'cf32_le'
        for ppm in (-123.5,217.25):
            yield f'fresh_{which}_clock_{ppm}', 'fresh_clock', group, generate(clock=1+ppm/1e6), width, 'cf32_le'
        for delay,amplitude,phase in ((5,.25,1.3),(12,.8,-.8),(31,.9,2.1)):
            x = [z+(amplitude*cmath.exp(1j*phase)*base[n-delay] if n>=delay else 0) for n,z in enumerate(base)]
            yield f'fresh_{which}_echo_{delay}', 'fresh_multipath', group, x, width, 'cf32_le'
        for carrier in (-.2e6,.2e6):
            yield f'fresh_{which}_cfo_{carrier}', 'fresh_cfo', group, generate(carrier=carrier), width, 'cf32_le'
        for factor in (.85,1.15):
            yield f'fresh_{which}_off_slope_{factor}', 'fresh_off_template', group, generate(factor=factor), width, 'cf32_le'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--refine', action='store_true')
    p.add_argument('--fresh', action='store_true')
    args = p.parse_args()
    output, tool = args.output.resolve(), args.tool.resolve()
    if output.exists():
        raise ValueError('output must be a new directory')
    output.mkdir(parents=True)
    source_hashes = {str(f.relative_to(ROOT)): hashlib.sha256(f.read_bytes()).hexdigest() for f in
        (Path(__file__), ROOT/'src/cyclostationary/link_evidence.cpp', ROOT/'src/cyclostationary/link_evidence.hpp')}
    records = []
    def run(name, category, group, path, rate, width, fmt, samples, checksum, provenance):
        command = [str(tool), 'analyze', '--input', str(path), '--format', fmt, '--sample-rate', str(rate),
                   '--samples', str(samples), '--frames', '256', '--assess-evidence']
        if args.refine:
            command += ['--refine-chirps']
        if width is not None:
            command += ['--usable-bandwidth', str(width)]
        result = json.loads(subprocess.run(command, check=True, capture_output=True, text=True, timeout=10).stdout)
        e = result['dsp_evidence']
        assert e['policy_version'] == POLICY and not e['named_family_acceptance_enabled'] and not e['thresholds_calibrated']
        assert result['classification'] == {'waveform': 'unknown', 'link_family': 'unknown', 'drone_assessment': 'insufficient_evidence', 'model_status': 'not_trained'}
        record = {'case': name, 'category': category, 'source_group': group, 'provenance': provenance,
                  'sample_rate_hz': rate, 'samples': samples, 'format': fmt, 'sha256_iq': checksum,
                  'classification': result['classification'], 'dsp_evidence': e,
                  'chirp_structure': result['chirp_structure'], 'ofdm_structure': result['ofdm_structure']}
        if args.refine:
            record['chirp_refinement'] = result['chirp_refinement']
        records.append(record)
    with tempfile.TemporaryDirectory(prefix='rfmon-evidence-benchmark-') as temp:
        path = Path(temp)/'control.iq'
        for name, category, group, x, width, fmt in (fresh_cases() if args.fresh else cases()):
            if fmt == 'ci16_le':
                data = b''.join(struct.pack('<hh', max(-32768, min(32767, round(z.real*32768))),
                    max(-32768, min(32767, round(z.imag*32768)))) for z in x)
            else:
                data = b''.join(struct.pack('<ff', z.real, z.imag) for z in x)
            path.write_bytes(data)
            run(name, category, group, path, RATE, width, fmt, len(x), hashlib.sha256(data).hexdigest(), 'analytic_software_control_no_drone_truth')
    for name in ('airtel-2g4', 'avgarde-5g', 'avgarde-5g-offset'):
        header = json.loads((ROOT/'tests/fixtures/wifi_ofdm'/(name+'.json')).read_text())
        path = ROOT/'tests/fixtures/wifi_ofdm'/header['iq_file']
        checksum = hashlib.sha256(path.read_bytes()).hexdigest()
        run(name, 'received_wifi', 'received-wifi:2026-09-19-session', path, header['sample_rate_hz'], None,
            header['format'], header['samples'], checksum, 'decoded_wifi_waveform_physical_platform_unverified')
        assert hashlib.sha256(path.read_bytes()).hexdigest() == checksum
    status, category_counts = Counter(), Counter()
    for record in records:
        for c in record['dsp_evidence']['candidates']:
            status[c['kind']+':'+c['status']] += 1
            if c['pattern_consistent']:
                category_counts[record['category']+':'+c['kind']] += 1
    summary = {'schema': 'rfmon.cyclo.evidence_benchmark.v1', 'policy_version': POLICY, 'policy_and_generator_sha256': source_hashes,
        'thresholds_frozen_before_run': True, 'thresholds_informed_by_prior_development': True,
        'cases': len(records), 'source_groups': len({r['source_group'] for r in records}),
        'case_set': 'fresh_controls_and_impairments' if args.fresh else 'prior_development_controls',
        'partition': 'software_development_benchmark_only', 'independent_real_unit_session_test': False,
        'radio_opened': False, 'model_trained': False, 'remote_id_decoded': False,
        'named_family_acceptance_enabled': False, 'family_precision_recall': None,
        'status_counts': dict(status), 'category_pattern_counts': dict(category_counts), 'records': records}
    if args.refine:
        summary['refinement_shape_counts'] = dict(Counter(r['category'] for r in records
            if any(c['shape_consistent'] for c in r['chirp_refinement']['candidates'])))
        summary['offline_refinement_diagnostic_only'] = True
        summary['policy_and_generator_sha256']['src/cyclostationary/chirp_refinement.cpp'] = hashlib.sha256(
            (ROOT/'src/cyclostationary/chirp_refinement.cpp').read_bytes()).hexdigest()
    (output/'summary.json').write_text(json.dumps(summary, indent=2, allow_nan=False)+'\n')
    print(json.dumps({k: v for k, v in summary.items() if k != 'records'}, indent=2))


if __name__ == '__main__':
    main()
