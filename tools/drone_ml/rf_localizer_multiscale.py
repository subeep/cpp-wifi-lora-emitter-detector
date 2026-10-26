"""Offline multiscale RF proposals with separate transient and stable paths.

This experiment does not infer drone presence or label its own proposals.
Stable spectral excess is explicitly ambiguous with receiver/background shape.
The original rf_localizer remains the reproducible baseline.
"""
from dataclasses import asdict, dataclass
import math

import numpy as np
from scipy import fft, ndimage

from .rf_localizer import _rate, proposal_view


@dataclass(frozen=True)
class MultiscaleConfig:
    nfft: int = 1024
    hop: int = 512
    pool: int = 2
    chunk_seconds: float = .01
    max_regions_per_chunk: int = 256
    max_regions_per_record: int = 4096
    reference_method: str = 'median'
    # Exploratory settings: synthetic development only; not physical CFAR.
    fast_high_db: float = 8.
    fast_low_db: float = 4.
    integrated_high_db: float = 2.5
    integrated_low_db: float = .8
    stable_detail_high_db: float = 1.8
    stable_detail_low_db: float = .8
    stable_envelope_high_db: float = 6.
    stable_envelope_low_db: float = 3.

    def validate(self):
        if self.reference_method not in ('median','lower_tail'):
            raise ValueError('unknown reference method')
        if (self.nfft, self.hop, self.pool, self.chunk_seconds) != (1024, 512, 2, .01):
            raise ValueError('only the frozen multiscale sampling profile is supported')
        for name in ('fast', 'integrated', 'stable_detail', 'stable_envelope'):
            low, high = getattr(self, name+'_low_db'), getattr(self, name+'_high_db')
            if not all(math.isfinite(v) for v in (low, high)) or not 0 < low < high <= 30:
                raise ValueError('invalid contrast thresholds')
        for name, upper in [('max_regions_per_chunk', 1024), ('max_regions_per_record', 16384)]:
            value = getattr(self, name)
            if type(value) is not int or not 1 <= value <= upper:
                raise ValueError('invalid proposal budget')


def _combine(items):
    result = dict(items[0])
    for item in items[1:]:
        for k in ('start_sample', 'low_hz'):
            result[k] = min(result[k], item[k])
        for k in ('stop_sample', 'high_hz', 'peak_relative_db'):
            result[k] = max(result[k], item[k])
        for k in ('cells', 'high_cells', 'chunk_parts'):
            result[k] += item[k]
        result['branches'] = sorted(set(result['branches']) | set(item['branches']))
    # Presence of a transient branch does not disprove background contamination.
    result['stable_background_ambiguity'] = any(b.startswith('stable') for b in result['branches'])
    return result


def _intersection(a, b):
    return max(0, min(a['stop_sample'], b['stop_sample'])-max(a['start_sample'], b['start_sample'])) * max(
        0., min(a['high_hz'], b['high_hz'])-max(a['low_hz'], b['low_hz']))


def _area(r):
    return (r['stop_sample']-r['start_sample'])*(r['high_hz']-r['low_hz'])


def _family(r):
    return 'stable' if all(b.startswith('stable') for b in r['branches']) else 'transient'


def _boundary_match(a, b):
    # One region must not bridge unrelated frequencies or absorb an ambiguous
    # stable background. This is conservative stitching, not emitter tracking.
    if _family(a) != _family(b) or _intersection(a,b) <= 0:
        return False
    wa, wb = a['high_hz']-a['low_hz'], b['high_hz']-b['low_hz']
    ca, cb = (a['high_hz']+a['low_hz'])/2, (b['high_hz']+b['low_hz'])/2
    return min(wa,wb) >= .5*max(wa,wb) and abs(ca-cb) <= .25*min(wa,wb)


def _deduplicate(regions):
    """Combine nested/overlapping scales, not identities of distinct emitters."""
    kept = []
    for item in sorted(regions, key=_area, reverse=True):
        matches = [r for r in kept if _family(r)==_family(item)
                   and _intersection(r, item) >= .8*min(_area(r), _area(item))]
        if matches:
            item = _combine([item, *matches])
            for r in matches:
                kept.remove(r)
        kept.append(item)
    return kept


def _components(contrast, high, low, branch, rate, start, support, step,
                min_cells=6, min_high=2, seed_time_margin=0):
    # Close only one-cell holes; preserve edges using explicit edge padding.
    mask = contrast >= low
    if min(mask.shape) >= 3:
        mask = ndimage.binary_closing(np.pad(mask, 1, mode='edge'), structure=np.ones((3, 3)))[1:-1, 1:-1]
    labels, _ = ndimage.label(mask, structure=np.ones((3, 3)))
    seeds = contrast >= high
    if seed_time_margin:
        seeds[:seed_time_margin] = False
        seeds[-seed_time_margin:] = False
    regions = []; df = rate/1024
    for index, box in enumerate(ndimage.find_objects(labels), 1):
        if box is None:
            continue
        t, f = box
        member = labels[box] == index
        cells = int(member.sum()); high_cells = int(np.count_nonzero(member & seeds[box]))
        if f.stop-f.start < 3 or cells < min_cells or high_cells < min_high:
            continue
        regions.append(dict(start_sample=start+t.start*step,
            stop_sample=start+(t.stop-1)*step+support,
            low_hz=max(-rate/2, (f.start-512-.5)*df),
            high_hz=min(rate/2, (f.stop-512-.5)*df),
            peak_relative_db=float(contrast[box][member].max()), cells=cells,
            high_cells=high_cells, chunk_parts=1, branches=[branch],
            stable_background_ambiguity=branch.startswith('stable')))
    return regions


def chunk_regions(iq, rate, start_sample=0, config=MultiscaleConfig()):
    config.validate(); _rate(rate)
    raw = np.asarray(iq)
    if (raw.ndim != 1 or not 1536 <= raw.size <= 4_000_000 or not np.isfinite(raw).all()
            or type(start_sample) is not int or start_sample < 0):
        raise ValueError('invalid IQ chunk or source offset')
    # Scaling protects numerical range without deleting relative spectral shape.
    magnitude = float(np.max(np.abs(raw)))
    if not math.isfinite(magnitude):
        raise ValueError('invalid IQ magnitude')
    x = np.asarray(raw/(magnitude or 1.), dtype=np.complex64)
    x -= x.mean()
    n = ((len(x)-1024)//512+1)//2
    frames = np.lib.stride_tricks.sliding_window_view(x, 1024)[::512][:n*2]
    window = np.hanning(1025)[:-1].astype(np.float32)
    transform = fft.fftshift(fft.fft(frames*window, axis=1, workers=1), axes=1)
    power = np.mean((transform.real**2+transform.imag**2).reshape(n, 2, 1024), axis=1)
    power = ndimage.uniform_filter1d(power, 3, axis=1, mode='nearest')
    # Median over *this chunk* is adaptive context, not trained calibration.
    # High-duty or continuous energy is absorbed here and retained separately.
    epsilon = max(float(power.max())*1e-10, 1e-30)
    reference = np.maximum(np.median(power, axis=0, keepdims=True), epsilon)
    fast_reference = reference if config.reference_method=='median' else np.maximum(
        np.quantile(power,.2,axis=0,keepdims=True),epsilon)
    normalized = power/fast_reference
    fast = 10*np.log10(np.maximum(normalized, 1e-10))
    if config.reference_method=='median':
        integrated = ndimage.uniform_filter(normalized, size=(9, 9), mode='reflect')
        integrated /= np.maximum(np.median(integrated, axis=0, keepdims=True), 1e-10)
    else:
        # Estimate the lower reference AFTER integration: applying a noisy
        # single-cell lower quantile first inflates even noise-only contrast.
        integrated = ndimage.uniform_filter(power, size=(9,9), mode='reflect')
        integrated = integrated/np.maximum(np.quantile(integrated,.2,axis=0,keepdims=True),epsilon)
    integrated = 10*np.log10(np.maximum(integrated, 1e-10))
    support = 1536; step = 1024
    regions = _components(fast, config.fast_high_db, config.fast_low_db,
                          'transient_fast', rate, start_sample, support, step)
    regions += _components(integrated, config.integrated_high_db, config.integrated_low_db,
                           'transient_integrated', rate, start_sample, support, step, 18, 6,
                           seed_time_margin=4)
    profile = ndimage.uniform_filter1d(reference[0], 5, mode='nearest')
    profile_db = 10*np.log10(np.maximum(profile, epsilon))
    detail = profile_db-ndimage.gaussian_filter1d(profile_db, 24, mode='nearest')
    envelope = profile_db-float(np.quantile(profile_db, .2))
    # Stable paths have the whole chunk as their support. Broad excess is
    # intentionally kept as ambiguous, not silently declared noise or a drone.
    for branch, values, high, low in [
        ('stable_detail', detail, config.stable_detail_high_db, config.stable_detail_low_db),
        ('stable_envelope', envelope, config.stable_envelope_high_db, config.stable_envelope_low_db)]:
        bands = _components(values[None, :], high, low, branch, rate, start_sample,
                            (n-1)*step+support, step, min_cells=3, min_high=1)
        regions += bands
    raw_count = len(regions)
    regions = _deduplicate(regions)
    regions.sort(key=lambda r: (-_area(r), -r['high_cells']))
    dropped = max(0, len(regions)-config.max_regions_per_chunk)
    return regions[:config.max_regions_per_chunk], dict(pooled_frames=n,
        pre_deduplication_regions=raw_count, dropped_regions=dropped,
        exact_zero_fraction=float(np.mean(raw == 0)), all_zero=bool(np.all(raw == 0)),
        stable_background_ambiguity=True, source_component_peak=float(max(np.max(abs(raw.real)), np.max(abs(raw.imag)))),
        spectrum_profile_db=profile_db[::8].astype(float).tolist(),
        profile_bin_stride=8, profile_is_relative=True,
        integrated_seed_time_margin_cells=4,
        integrated_seed_eligible_cells=max(0,n-8))


def localize(get, available, rate, seconds=.1, config=MultiscaleConfig()):
    config.validate(); _rate(rate)
    if not math.isfinite(seconds) or not .001 <= seconds <= 2:
        raise ValueError('duration must be 1 ms to 2 seconds')
    limit = min(available, round(seconds*rate))
    stride = min(round(rate*.01)//1024*1024, (4_000_000-512)//1024*1024)
    start = 0; used = 0; regions = []; chunks = []; record_drops = 0
    while start+1536 <= limit:
        length = min(stride, (limit-start-512)//1024*1024)
        # Fold a tiny tail into the preceding block so it does not become an
        # independent, statistically inadequate background estimate.
        remaining_cells = (limit-start-length-512)//1024
        if 0 < remaining_cells < 16 and length+remaining_cells*1024+512 <= 4_000_000:
            length += remaining_cells*1024
        iq = get(start, length+512)
        if len(iq) != length+512:
            raise ValueError('short source read')
        incoming, quality = chunk_regions(iq, rate, start, config)
        # Join boundary components only; maintain combined branch provenance.
        active = [r for r in regions if r['stop_sample'] >= start]
        for item in incoming:
            matches = [r for r in active if item['start_sample'] <= start+1536 and _boundary_match(r,item)]
            if matches:
                old = max(matches,key=lambda r:_intersection(r,item)/min(_area(r),_area(item)))
                item = _combine([item, old])
                regions.remove(old); active.remove(old)
            if len(regions) < config.max_regions_per_record:
                regions.append(item)
            else:
                record_drops += 1
        chunks.append(dict(start_sample=start, samples=len(iq), **quality))
        used = start+len(iq); start += length
    if not chunks:
        raise ValueError('insufficient source samples')
    regions.sort(key=lambda r: (r['start_sample'], r['low_hz']))
    for index, r in enumerate(regions):
        r.update(id=index, start_ms=r['start_sample']/rate*1000,
            duration_us=(r['stop_sample']-r['start_sample'])/rate*1e6,
            center_offset_hz=(r['low_hz']+r['high_hz'])/2, bandwidth_hz=r['high_hz']-r['low_hz'],
            touches_observation_edge=r['start_sample']==0 or r['stop_sample']>=used,
            touches_frequency_edge=r['low_hz']<=-.48*rate or r['high_hz']>=.48*rate,
            proposed_view=proposal_view(r['low_hz'], r['high_hz'], rate))
    return dict(algorithm='multiscale_v1' if config.reference_method=='median' else 'multiscale_lower_tail_v1',
        config=asdict(config), sample_rate_hz=rate,
        stitching_policy='separate_stable_transient_one_to_one_frequency_consistency',
        requested_seconds=seconds, available_samples=available, analysed_samples=used,
        trailing_samples_not_analysed=limit-used, grid_step_seconds=1024/rate,
        frame_support_seconds=1536/rate, integrated_support_seconds=(1536+8*1024)/rate,
        tail_join_threshold_cells=16,
        frequency_bin_hz=rate/1024, regions=regions, chunks=chunks,
        dropped_regions=sum(c['dropped_regions'] for c in chunks)+record_drops,
        record_budget_drops=record_drops,
        fallback=dict(required=True, kind='retain_native_full_band_overview', sample_rate_hz=rate,
                      model_input_created=False),
        drone_presence=None, accepted_identity=None, training_eligible=False,
        live_qualified=False, reference_coordinates_used=False)
