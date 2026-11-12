"""Blind cyclic-prefix lag probe for recorded IQ (offline, read-only).

An OFDM signal repeats the tail of every symbol as its cyclic prefix, so its
autocorrelation has a peak at the useful-symbol duration T_u = 1 / subcarrier spacing.
This measures the normalised autocorrelation magnitude against lag in short blocks and
reports (a) blind peaks, and (b) how often named hypotheses stand out from neighbouring
lags. It is a structural measurement: a peak shows OFDM timing, not a drone.
"""
import argparse
import functools
import json
from pathlib import Path

import numpy as np

from .common import write_json

BLOCK = 131072          # at 20 MSps (6.6 ms); scaled with rate by block_size()
# name -> useful-symbol duration in seconds
HYPOTHESES = {'wifi_312.5kHz': 3.2e-6, 'wifi_ax_78.125kHz': 12.8e-6, 'lte_like_15kHz': 1 / 15000,
              'lte_like_30kHz': 1 / 30000, 'lte_like_7.5kHz': 1 / 7500,
              # Added 10 Nov from the blind peak on the Tampere Lightbridge recordings (30.5 us).
              'measured_32.8kHz': 1 / 32812.5}
GROUP_THRESHOLD = .3   # capture-level flagged fraction; chosen after seeing X310 data, not calibrated


def timing_group(result, threshold=GROUP_THRESHOLD):
    """Capture-level symbol-timing group from a probe() result, or None when no listed timing stands out."""
    f = {k: v['flagged_fraction'] for k, v in result['hypotheses'].items()}
    if f.get('measured_32.8kHz', 0) >= threshold:
        return '32.8kHz'
    k15, k30 = f.get('lte_like_15kHz', 0) >= threshold, f.get('lte_like_30kHz', 0) >= threshold
    return '15kHz+30kHz' if k15 and k30 else '30kHz' if k30 else '15kHz' if k15 else None
RATIO = 4.0   # block flagged when the lag stands this far above the local median (Rayleigh null ~1.5e-5)


SPIKE_CHUNK, SPIKE_CAP = 256, 20.0   # spectral bins per chunk; cap as a multiple of the chunk median


@functools.lru_cache(maxsize=4)
def _hann(n):
    return np.hanning(n).astype(np.float32)


def lag_profile(x, max_lag):
    """Normalised autocorrelation magnitude for lags 0..max_lag, robust to narrowband spikes.

    A strong tone adds the same amount at every lag and hides the cyclic-prefix peak. The block is
    Hann-windowed so a tone stays within a few bins, and each power-spectrum bin is capped at
    SPIKE_CAP times the median of its 256-bin neighbourhood. For noise-like spectra (OFDM, noise) a
    bin exceeds that cap with probability about 1e-6, so wideband signals are unchanged.
    Returns None for an empty or non-finite block.
    """
    power = _capped_power(x)
    return None if power is None else _normalised(power, max_lag)


def _capped_power(x):
    x = np.asarray(x, dtype=np.complex64)
    x = x - x.mean()
    n = x.size // SPIKE_CHUNK * SPIKE_CHUNK
    if n < 4 * SPIKE_CHUNK:
        return None
    power = np.abs(np.fft.fft(x[:n] * _hann(n))) ** 2
    cap = SPIKE_CAP * np.median(power.reshape(-1, SPIKE_CHUNK), axis=1)
    return np.minimum(power.reshape(-1, SPIKE_CHUNK), cap[:, None]).ravel()


def _normalised(power, max_lag):
    acf = np.abs(np.fft.ifft(power)[:max_lag + 1])
    if not np.isfinite(acf).all() or acf[0] <= 0:
        return None
    return acf / acf[0]


def subband_centres(rate, width_hz, step_hz=None):
    """Sub-band centre offsets inside the guarded 90% of the capture; empty when the capture is not
    meaningfully wider than one sub-band."""
    extent = .45 * rate - width_hz / 2
    if extent < width_hz / 4:
        return []
    step = step_hz or width_hz / 2
    k = int(np.floor(extent / step))
    return [float(i * step) for i in range(-k, k + 1)]


@functools.lru_cache(maxsize=32)
def _band_mask(n, rate, centre, width):
    """Raised-cosine frequency window: flat over the middle 80% of the sub-band."""
    d = np.abs(np.fft.fftfreq(n, 1 / rate) - centre) / (width / 2)
    return np.where(d <= .8, 1., np.where(d < 1, .5 * (1 + np.cos(np.pi * (d - .8) / .2)), 0.)).astype(np.float32)


def subband_profiles(x, max_lag, rate, width_hz, step_hz=None):
    """Lag profiles of the whole block and of each sub-band, at the block's own lag resolution.

    A link 10-20 MHz wide inside a wider capture is diluted by the rest of the band. Restricting the
    capped power spectrum to a sub-band before forming the autocorrelation removes that dilution
    without resampling. Returns (offsets_hz with None first for the full band, array [bands, lags]).
    """
    power = _capped_power(x)
    if power is None:
        return None, None
    offsets, out = [None], [_normalised(power, max_lag)]
    for c in subband_centres(rate, width_hz, step_hz):
        prof = _normalised(power * _band_mask(power.size, rate, c, width_hz), max_lag)
        if prof is not None:
            offsets.append(c); out.append(prof)
    if out[0] is None:
        return None, None
    return offsets, np.stack(out)


def block_size(rate):
    """Power of two closest to 6.6 ms, so blocks cover a similar duration at every rate."""
    return int(2 ** round(np.log2(BLOCK * rate / 20e6)))


def open_iq(path, fmt):
    """Returns (total complex samples, reader(start, count) -> complex64 array, close)."""
    if fmt == 'cf32_le':
        d = np.memmap(path, dtype='<c8', mode='r')
        return d.size, lambda s, n: np.asarray(d[s:s + n], dtype=np.complex64), lambda: None
    if fmt == 'sc16_le':
        d = np.memmap(path, dtype='<i2', mode='r')
        def read(s, n):
            raw = np.asarray(d[2 * s:2 * (s + n)], dtype=np.float32)
            return (raw[0::2] + 1j * raw[1::2]) / 32768
        return d.size // 2, read, lambda: None
    if fmt == 'drff_mat':
        import h5py                                  # DRFF-R2: separate RF0_I / RF0_Q float vectors
        h = h5py.File(path, 'r')
        names = ('RF0_I', 'RF0_Q') if 'RF0_I' in h else ('I', 'Q')     # DRFF-R2, or DroneRFb-DIR
        i, q = h[names[0]], h[names[1]]
        if i.shape != q.shape or len(i.shape) != 2 or min(i.shape) != 1:
            raise ValueError('unsupported DRFF IQ layout')
        cut = (lambda d, s, n: d[s:s + n, 0]) if i.shape[1] == 1 else (lambda d, s, n: d[0, s:s + n])
        return i.size, lambda s, n: (cut(i, s, n) + 1j * cut(q, s, n)).astype(np.complex64), h.close
    raise ValueError('unsupported format')


def probe(path, fmt, rate, max_blocks=200, subband_hz=None):
    total, read, close = open_iq(path, fmt)
    BLOCK = block_size(rate)
    count = min(max_blocks, total // BLOCK)
    if count < 1:
        raise ValueError('recording shorter than one block')
    max_lag = int(min(BLOCK // 4, round(rate * 150e-6)))
    lags = {name: rate * t for name, t in HYPOTHESES.items() if 8 <= rate * t <= max_lag - 60}
    profiles, powers, bands, offsets = [], [], [], [None]
    for start in np.linspace(0, total - BLOCK, count).astype(np.int64):
        x = read(int(start), BLOCK)
        p = float(np.mean(np.abs(x - x.mean()) ** 2))
        if subband_hz:
            offsets, stack = subband_profiles(x, max_lag, rate, subband_hz)
            profile = None if stack is None else stack[0]
        else:
            profile = lag_profile(x, max_lag); stack = None if profile is None else profile[None]
        if profile is None or not np.isfinite(p) or p <= 0:
            continue
        profiles.append(profile); powers.append(p); bands.append(stack)
    close()
    profiles, powers, bands = np.array(profiles), np.array(powers), np.array(bands)   # bands: [blocks, bands, lags]
    floor = np.percentile(powers, 10)
    active = powers > floor * 10 ** .6                      # 6 dB above this file's quiet decile
    out = dict(file=str(path), rate_hz=rate, blocks=int(len(powers)), block_seconds=BLOCK / rate, subband_hz=subband_hz, bands=len(offsets),
               median_power_dbfs=float(10 * np.log10(np.median(powers))), active_blocks=int(active.sum()), hypotheses={})
    for name, lag in lags.items():
        near = [int(np.floor(lag)), int(np.ceil(lag))] if abs(lag - round(lag)) > 1e-6 else [int(round(lag))]
        lo, hi = near[0] - 50, near[-1] + 51
        mask = np.ones(hi - lo, bool); mask[near[0] - lo - 3:near[-1] - lo + 4] = False
        ratios = bands[:, :, near].max(axis=2) / np.median(bands[:, :, lo:hi][:, :, mask], axis=2)   # [blocks, bands]
        best = ratios.argmax(axis=1)
        flagged = ratios.max(axis=1) > RATIO                      # any band (full capture first) stands out
        base = np.median(profiles[:, lo:hi][:, mask], axis=1)
        value = profiles[:, near].max(axis=1)
        entry = dict(lag_samples=float(lag), flagged_fraction=float(flagged.mean()),
            flagged_fraction_active=float(flagged[active].mean()) if active.any() else None,
            median_value=float(np.median(value)), max_value=float(value.max()), median_ratio=float(np.median(value / base)))
        if subband_hz and flagged.any():
            hit = [offsets[k] for k in best[flagged]]
            entry['flagged_full_band_fraction'] = float((ratios[:, 0] > RATIO).mean())
            entry['best_offset_hz'] = max(set(hit), key=hit.count)   # most frequent sub-band; None = full capture
        out['hypotheses'][name] = entry
    # Blind peaks on the block-averaged profile: no hypothesis assumed.
    mean = profiles.mean(axis=0)
    peaks = []
    from numpy.lib.stride_tricks import sliding_window_view
    w = sliding_window_view(mean[8:], 101)                     # window centred on lag = index + 58
    centre = w[:, 50]
    hit = np.flatnonzero((centre == w.max(axis=1)) & (centre > 3 * np.median(w, axis=1)))
    for k in hit:
        lag = int(k) + 58
        if lag < max_lag - 50:
            peaks.append(dict(lag_samples=lag, useful_us=lag / rate * 1e6, spacing_khz=rate / lag / 1e3,
                              value=float(mean[lag]), ratio=float(centre[k] / np.median(w[k]))))
    out['blind_peaks'] = sorted(peaks, key=lambda p: -p['ratio'])[:6]
    return out


def block_ratios(x, rate):
    """Hypothesis-lag value over the local median for one block of IQ (same statistic as probe())."""
    max_lag = int(min(len(x) // 4, round(rate * 150e-6)))
    acf = lag_profile(x, max_lag)
    if acf is None:
        return {}
    out = {}
    for name, t in HYPOTHESES.items():
        lag = rate * t
        if not 8 <= lag <= max_lag - 60:
            continue
        near = [int(np.floor(lag)), int(np.ceil(lag))] if abs(lag - round(lag)) > 1e-6 else [int(round(lag))]
        lo, hi = near[0] - 50, near[-1] + 51
        mask = np.ones(hi - lo, bool); mask[near[0] - lo - 3:near[-1] - lo + 4] = False
        out[name] = float(acf[near].max() / np.median(acf[lo:hi][mask]))
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', type=Path, nargs='+', required=True)
    p.add_argument('--format', choices=('cf32_le', 'sc16_le', 'drff_mat'), required=True)
    p.add_argument('--sample-rate', type=float, required=True)
    p.add_argument('--max-blocks', type=int, default=200)
    p.add_argument('--subband-hz', type=float, help='also search sub-bands of this width (for captures wider than the link)')
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    results = [probe(f, a.format, a.sample_rate, a.max_blocks, a.subband_hz) for f in a.input]
    if a.output:
        if a.output.exists():
            raise SystemExit('output exists; not overwritten')
        write_json(a.output, dict(schema='rfmon.drone_ml.cp_lag_probe.v2', ratio=RATIO, spike_cap=SPIKE_CAP, results=results))
    for r in results:
        h = r['hypotheses']
        print(Path(r['file']).name, f"{r['median_power_dbfs']:.1f} dBFS",
              ' '.join(f"{k}={v['flagged_fraction']:.2f}" for k, v in h.items()),
              '| blind:', ', '.join(f"{b['spacing_khz']:.2f}kHz x{b['ratio']:.0f}" for b in r['blind_peaks'][:3]))


if __name__ == '__main__':
    main()
