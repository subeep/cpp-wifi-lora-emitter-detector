"""Exploratory: narrowband control-uplink bursts and their timing pattern (offline, read-only).

A DJI OcuSync controller measured in a shielded chamber (KU Leuven Mini 2 RC) sends bursts about
0.5 ms long and about 1.4 MHz wide, hopping in frequency, with start times alternating 4 ms and
6 ms apart (two bursts per 10 ms frame). This module finds short narrow bursts in a capture and
measures how many have a partner at those spacings, against the number expected by chance.
"""
import numpy as np
from scipy import ndimage

GAPS = (4e-3, 6e-3, 10e-3)       # burst-start spacings of the measured pattern
TOL = 6e-5                       # +/- tolerance on a spacing (STFT frame is about 10-13 us)
DUR = (3.5e-4, 6.5e-4)           # candidate burst duration
BW = (6e-4 * 1e9, 4e6)           # candidate bandwidth: 0.6-4 MHz (threshold skirts widen strong bursts)


def bursts(x, rate, thresh_db=10.0):
    """Connected time-frequency regions above each bin's median. Rows: start_s, duration_s,
    centre_offset_hz, bandwidth_hz, peak_over_floor_db."""
    nfft = int(2 ** round(np.log2(rate / 97656.25)))
    n = x.size // nfft * nfft
    s = np.abs(np.fft.fftshift(np.fft.fft(x[:n].reshape(-1, nfft) * np.hanning(nfft).astype(np.float32), axis=1), axes=1)) ** 2
    floor = np.median(s, axis=0, keepdims=True)
    db = 10 * np.log10(np.maximum(s, 1e-30) / np.maximum(floor, 1e-30))
    lab, _ = ndimage.label(ndimage.binary_closing(db > thresh_db, structure=np.ones((3, 3))))
    dt, df, out = nfft / rate, rate / nfft, []
    for i, (t, f) in enumerate(ndimage.find_objects(lab), 1):
        if (lab[t, f] == i).sum() >= 12:
            out.append((t.start * dt, (t.stop - t.start) * dt, ((f.start + f.stop) / 2 - nfft / 2) * df, (f.stop - f.start) * df,
                        float(db[t, f][lab[t, f] == i].max())))
    return np.array(out).reshape(-1, 5)


def candidates(b):
    return b[(b[:, 1] >= DUR[0]) & (b[:, 1] <= DUR[1]) & (b[:, 3] >= BW[0]) & (b[:, 3] <= BW[1])]


def pattern(x, rate):
    """Counts candidate bursts and how many have a later partner at 4, 6 or 10 ms, with the number
    expected if the same bursts fell at random times."""
    c = candidates(bursts(x, rate))
    seconds = x.size / rate
    t = np.sort(c[:, 0])
    gaps = (t[None, :] - t[:, None])
    matched = int(sum(np.any([np.abs(gaps[i] - g) <= TOL for g in GAPS]) for i in range(len(t))))
    expected = len(t) * (len(t) / seconds) * len(GAPS) * 2 * TOL if seconds > 0 else 0.0
    return dict(seconds=seconds, candidates=int(len(t)), matched=matched, expected_by_chance=float(expected),
                frequencies=int(len(np.unique(np.round(c[:, 2] / 1e6)))))


def gap_histogram(x, rate, max_gap=0.0205, bin_s=1e-4):
    t = np.sort(candidates(bursts(x, rate))[:, 0])
    g = (t[None, :] - t[:, None]); g = g[(g > 0) & (g < max_gap)]
    return np.histogram(g, bins=np.arange(0, max_gap + bin_s, bin_s))[0], len(t)
