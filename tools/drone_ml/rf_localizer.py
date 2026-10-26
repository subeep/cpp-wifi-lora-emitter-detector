"""Offline RF-region proposals, not packets, drone detections or training labels.

Only IQ and its actual sample rate enter the localizer. Reference annotations
and model scores are deliberately absent. The relative floor is a heuristic,
not a calibrated noise estimate; broadband/colored noise remains ambiguous.
"""
from dataclasses import asdict, dataclass
import math

import numpy as np
from scipy import ndimage


@dataclass(frozen=True)
class LocalizerConfig:
    nfft: int = 1024
    hop: int = 512
    pool: int = 2
    smooth_bins: int = 3
    high_db: float = 10.
    low_db: float = 6.
    min_cells: int = 6
    min_bins: int = 3
    max_regions_per_chunk: int = 256
    chunk_seconds: float = .01

    def validate(self):
        if (self.nfft != 1024 or self.hop != 512 or self.pool != 2
                or self.smooth_bins != 3):
            raise ValueError('only the frozen 1024/512/pool2/smooth3 profile is supported')
        if (not all(math.isfinite(v) for v in (self.high_db, self.low_db, self.chunk_seconds))
                or not 0 < self.low_db < self.high_db <= 30
                or not .001 <= self.chunk_seconds <= .01):
            raise ValueError('invalid thresholds or chunk duration')
        if any(not isinstance(v, int) or v < 1 for v in
               (self.min_cells, self.min_bins, self.max_regions_per_chunk)):
            raise ValueError('invalid component budget')
        if self.max_regions_per_chunk > 1024:
            raise ValueError('component budget too large')


def _rate(rate):
    if not math.isfinite(rate) or not 20e6 <= rate <= 200e6 or rate != int(rate):
        raise ValueError('unsupported actual sample rate')


def proposal_view(low, high, rate):
    """Proposed nominal view with 20% total margin; never feed old weights.

    Guarded resampling uses the same 90%-of-native-band constraint as the
    existing preprocessor. Native full-band views are retained as fallback.
    """
    _rate(rate)
    if not all(math.isfinite(v) for v in (low, high)) or not -rate/2 <= low < high <= rate/2:
        raise ValueError('invalid frequency bounds')
    for target in sorted(set([20e6, 25e6, 50e6, 100e6, float(rate)])):
        if target > rate:
            continue
        extent = 0. if target == rate else .45*rate-target/2
        if extent < 0:
            continue
        offset = float(np.clip((low+high)/2, -extent, extent))
        if low >= offset-.4*target and high <= offset+.4*target:
            return dict(rate_hz=int(target), offset_hz=offset, margin_satisfied=True,
                        requires_matching_model_preprocessing=True)
    return dict(rate_hz=int(rate), offset_hz=0., margin_satisfied=False,
                requires_matching_model_preprocessing=True)


def chunk_regions(iq, rate, start_sample=0, config=LocalizerConfig()):
    config.validate(); _rate(rate)
    x = np.asarray(iq)
    if x.ndim != 1 or not config.nfft <= len(x) <= 4_000_000 or not np.isfinite(x).all():
        raise ValueError('invalid or oversized IQ chunk')
    if not isinstance(start_sample, int) or start_sample < 0:
        raise ValueError('invalid source offset')
    n = (1+(len(x)-config.nfft)//config.hop)//config.pool
    if n < 1:
        raise ValueError('too few complete pooled frames')
    x = x.astype(np.complex128)
    x -= x.mean()  # Suppress a constant receiver offset; DC signals can be affected.
    frames = np.lib.stride_tricks.sliding_window_view(x, config.nfft)[::config.hop][:n*config.pool]
    window = np.hanning(config.nfft+1)[:-1]
    psd = abs(np.fft.fftshift(np.fft.fft(frames*window, axis=1), axes=1))**2
    power = psd.reshape(n, config.pool, config.nfft).mean(axis=1)
    power = ndimage.uniform_filter1d(power, config.smooth_bins, axis=1, mode='nearest')
    # Two independent contrasts: against a within-time low spectral quantile,
    # and against a within-frequency low temporal quantile. Their union can
    # retain continuous narrowband signals and bursts over colored backgrounds.
    # Neither establishes a physical noise floor or semantic presence label.
    epsilon = max(float(power.max())*1e-12, np.finfo(float).tiny)
    spectral_floor = np.maximum(np.quantile(power, .2, axis=1, keepdims=True), epsilon)
    temporal_floor = np.maximum(np.quantile(power, .2, axis=0, keepdims=True), epsilon)
    spectral_db = 10*np.log10(np.maximum(power, epsilon)/spectral_floor)
    temporal_db = 10*np.log10(np.maximum(power, epsilon)/temporal_floor)
    contrast = np.maximum(spectral_db, temporal_db)
    labels, count = ndimage.label(contrast >= config.low_db, structure=np.ones((3, 3)))
    boxes = ndimage.find_objects(labels)
    step = config.pool*config.hop; df = rate/config.nfft
    regions = []
    for label_id, box in enumerate(boxes, 1):
        if box is None:
            continue
        time_slice, freq_slice = box
        if freq_slice.stop-freq_slice.start < config.min_bins:
            continue
        mask = labels[box] == label_id
        cells = int(mask.sum()); seed_count = int(np.count_nonzero((contrast[box] >= config.high_db) & mask))
        if cells < config.min_cells or seed_count < 2:
            continue
        low = max(-rate/2, (freq_slice.start-config.nfft/2-.5)*df)
        high = min(rate/2, (freq_slice.stop-config.nfft/2-.5)*df)
        regions.append(dict(start_sample=start_sample+time_slice.start*step,
            stop_sample=start_sample+(time_slice.stop-1)*step+config.nfft+config.hop,
            low_hz=float(low), high_hz=float(high), cells=cells, high_cells=seed_count,
            peak_relative_db=float(contrast[box][mask].max()),
            spectral_seed=bool(np.any((spectral_db[box] >= config.high_db) & mask)),
            temporal_seed=bool(np.any((temporal_db[box] >= config.high_db) & mask)),
            chunk_parts=1))
    regions.sort(key=lambda r: (-r['high_cells'], -r['cells'], r['start_sample'], r['low_hz']))
    dropped = max(0, len(regions)-config.max_regions_per_chunk)
    regions = regions[:config.max_regions_per_chunk]
    return regions, dict(pooled_frames=n, connected_components=count, dropped_regions=dropped,
        exact_zero_fraction=float(np.mean(iq == 0)),
        all_zero=bool(np.all(iq == 0)), maximum_relative_db=float(contrast.max()))


def merge_boundary_regions(regions, incoming, boundary_sample, config, rate):
    """Join only components touching successive chunk boundaries.

    Bounding-box joining can merge intersecting emitters; these are candidate
    regions, not independently tracked emitters or packet boundaries.
    """
    tolerance = config.nfft+config.hop
    active = [r for r in regions if r['stop_sample'] >= boundary_sample]
    for item in incoming:
        matches = [r for r in active if item['start_sample'] <= boundary_sample+tolerance
                   and min(r['high_hz'], item['high_hz']) > max(r['low_hz'], item['low_hz'])
                   and item['start_sample'] <= r['stop_sample']]
        if not matches:
            regions.append(item)
            continue
        merged = dict(item)
        for old in matches:
            merged['start_sample'] = min(merged['start_sample'], old['start_sample'])
            merged['stop_sample'] = max(merged['stop_sample'], old['stop_sample'])
            merged['low_hz'] = min(merged['low_hz'], old['low_hz'])
            merged['high_hz'] = max(merged['high_hz'], old['high_hz'])
            for key in ('cells', 'high_cells', 'chunk_parts'):
                merged[key] += old[key]
            merged['peak_relative_db'] = max(merged['peak_relative_db'], old['peak_relative_db'])
            for key in ('spectral_seed', 'temporal_seed'):
                merged[key] |= old[key]
            regions.remove(old); active.remove(old)
        regions.append(merged); active.append(merged)


def localize(get, available, rate, seconds=.1, config=LocalizerConfig()):
    config.validate(); _rate(rate)
    if not math.isfinite(seconds) or not .001 <= seconds <= 2:
        raise ValueError('duration must be 1 ms to 2 seconds')
    limit = min(available, round(seconds*rate))
    step = config.pool*config.hop
    overlap = config.nfft-config.hop
    cells = min(round(rate*config.chunk_seconds)//step, (4_000_000-overlap)//step)
    stride = max(1, cells)*step
    start = 0; used = 0; regions = []; chunks = []
    while start+step+overlap <= limit:
        length = min(stride, (limit-start-overlap)//step*step)
        iq = get(start, length+overlap)
        if len(iq) != length+overlap:
            raise ValueError('short source read')
        items, quality = chunk_regions(iq, rate, start, config)
        merge_boundary_regions(regions, items, start, config, rate)
        chunks.append(dict(start_sample=start, samples=len(iq), **quality))
        used = start+len(iq); start += length
    if not chunks:
        raise ValueError('insufficient source samples')
    regions.sort(key=lambda r: (r['start_sample'], r['low_hz']))
    for i, r in enumerate(regions):
        r.update(id=i, start_ms=r['start_sample']/rate*1000,
                 duration_us=(r['stop_sample']-r['start_sample'])/rate*1e6,
                 center_offset_hz=(r['low_hz']+r['high_hz'])/2,
                 bandwidth_hz=r['high_hz']-r['low_hz'],
                 touches_observation_edge=r['start_sample']==0 or r['stop_sample']>=used,
                 touches_frequency_edge=r['low_hz']<=-.48*rate or r['high_hz']>=.48*rate,
                 proposed_view=proposal_view(r['low_hz'], r['high_hz'], rate))
    return dict(config=asdict(config), sample_rate_hz=rate, requested_seconds=seconds,
        available_samples=available, analysed_samples=used, trailing_samples_not_analysed=limit-used,
        grid_step_seconds=step/rate, frame_support_seconds=(config.nfft+config.hop)/rate,
        frequency_bin_hz=rate/config.nfft, regions=regions, chunks=chunks,
        dropped_regions=sum(c['dropped_regions'] for c in chunks),
        fallback=dict(required=True, kind='retain_native_full_band_overview',
                      sample_rate_hz=rate, model_input_created=False),
        drone_presence=None, accepted_identity=None, training_eligible=False,
        live_qualified=False, reference_coordinates_used=False)
