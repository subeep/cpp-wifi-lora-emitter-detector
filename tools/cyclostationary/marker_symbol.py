"""Bounded offline full-span diagnostic, never named-family acceptance.

Selection and diagnostic use the same IQ. Thresholds are fixed development
policy, not calibrated error rates. Prefix content and exact duration remain
unverified even when span support and surrounding contrast both pass.
"""
import math
import numpy as np

POLICY = 'marker_symbol_span_v1'
PROFILES = (135.2e9, 270.2e9, -135.3e9)


def assess_symbol(iq, rate, peak, slope, frequency_mean_hz, *, fixed_start=None):
    x = np.asarray(iq, dtype=np.complex128)
    if (x.ndim != 1 or len(x) > 65536 or not np.isfinite(x).all() or
        not math.isfinite(rate) or not 1e6 <= rate <= 1e9 or
        not isinstance(peak, int) or peak < 0 or peak >= len(x) or
        slope not in PROFILES or not math.isfinite(frequency_mean_hz)):
        raise ValueError('invalid symbol input or bounded profile')
    span = math.floor(rate * (1 / 15000) + .5)  # Reported approximately 66.67 us.
    legacy = 2 * math.floor(rate * 32e-6 + .5)
    slack = max(0, span - legacy)
    starts = sorted(set(peak - round(v) for v in np.linspace(0, slack, 17)))
    if fixed_start is not None:
        if not isinstance(fixed_start,int) or not 0<=fixed_start<len(x):
            raise ValueError('invalid fixed symbol start')
        starts=[fixed_start]
    result = dict(policy=POLICY, status='insufficient_boundary_context',
        expected_span_samples=span, expected_span_seconds=1/15000,
        start_trials=len(starts), eligible_trials=0, block_count=34,
        coherence_floor=.8, minimum_relative_block_energy=.1,
        boundary_coherence_ceiling=.5, span_supported=False,
        boundary_contrast=False, prefix_verified=False,
        full_symbol_duration_established=False, family_accepted=False,
        thresholds_calibrated=False, independent_evidence=False)
    nominal_sweep = 18e6 if slope == 270.2e9 else 9e6
    if rate <= nominal_sweep:
        result['status'] = 'unsupported_sample_rate'
        return result
    # Normalize before subtracting the mean to avoid overflow on finite input.
    scale = float(max(np.max(np.abs(x.real)), np.max(np.abs(x.imag))))
    if scale == 0:
        result['status'] = 'no_energy'
        return result
    x = x / scale
    x = x - x.mean()
    t = (np.arange(len(x)) - peak - (legacy-1)/2) / rate
    z = x * np.exp(-2j*np.pi*(frequency_mean_hz*t + slope*t*t/2))
    power = np.abs(x)**2
    def measure(a, b):
        energy = float(power[a:b].mean())
        score = float(abs(z[a:b].mean())**2 / energy) if energy > 1e-20 else 0.0
        return min(1.0, score), energy
    best = None
    for start in starts:
        end = start + span
        # Leave room for a possible ~4 us prefix, without claiming to decode it.
        pre = (start-round(rate*8e-6), start-round(rate*6e-6))
        post = (end+round(rate*2e-6), end+round(rate*4e-6))
        if pre[0] < 0 or post[1] > len(x):
            continue
        result['eligible_trials'] += 1
        edges = np.rint(np.linspace(start, end, 35)).astype(int)
        blocks = [measure(int(a), int(b)) for a, b in zip(edges, edges[1:])]
        reference = float(np.median([e for _, e in blocks]))
        minimum = min(c for c, _ in blocks)
        balance = min(e for _, e in blocks) / reference if reference > 1e-20 else 0.0
        before, after = measure(*pre), measure(*post)
        contrast = all(c <= .5 or e <= .1*reference for c,e in (before,after))
        supported = minimum >= .8 and balance >= .1
        row = dict(start_offset_samples=start, end_offset_samples_exclusive=end,
            minimum_block_coherence=minimum, minimum_relative_block_energy_observed=balance,
            pre_boundary_coherence=before[0], post_boundary_coherence=after[0],
            pre_boundary_relative_energy=before[1]/reference if reference > 1e-20 else 0.0,
            post_boundary_relative_energy=after[1]/reference if reference > 1e-20 else 0.0,
            span_supported=bool(supported), boundary_contrast=bool(contrast))
        rank = (supported and contrast, supported, minimum * min(1.0,balance))
        if best is None or rank > best[0]:
            best = rank, row
    if best:
        result.update(best[1])
        result['status'] = ('span_with_boundary_contrast' if result['span_supported'] and result['boundary_contrast']
            else 'span_without_boundary_contrast' if result['span_supported'] else 'full_span_not_supported')
    return result


def prepare_filtered_window(iq, rate, trial):
    """Reproduce the documented C++ Hamming FIR geometry for offline diagnostics."""
    p = trial['preprocessing']
    taps = p['filter_taps']; step = p['input_sample_step']
    if (p['method'] != 'hamming_sinc_integer_decimator_v1' or
        p['source_mean_removed_before_downmix'] or not 33 <= taps <= 1023 or
        taps % 2 != 1 or not 1 <= step <= 16 or len(iq) > 65536):
        raise ValueError('unsupported preprocessing provenance')
    cutoff = (p['passband_width_hz']/2 + p['stopband_edge_hz'])/(2*rate)
    t = np.arange(taps) - taps//2
    h = 2*cutoff*np.sinc(2*cutoff*t)*np.hamming(taps)
    h /= h.sum()
    mixed = iq * np.exp(-2j*np.pi*p['roi_offset_hz']*np.arange(len(iq))/rate)
    out = np.convolve(mixed, h, mode='valid')[::step].astype(np.complex64)
    if len(out) != p['output_samples']:
        raise ValueError('preprocessing output count mismatch')
    return out, rate/step
