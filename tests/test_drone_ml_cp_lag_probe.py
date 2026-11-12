from pathlib import Path
import tempfile
import unittest

import numpy as np
from scipy.signal import resample_poly

from tools.drone_ml.cp_lag_probe import probe


def ofdm(rng, n_fft, cp, symbols, used):
    out = []
    for _ in range(symbols):
        x = np.zeros(n_fft, complex)
        x[np.r_[1:used // 2 + 1, -used // 2:0]] = np.exp(1j * np.pi / 2 * rng.integers(0, 4, used))
        s = np.fft.ifft(x)
        out.append(np.r_[s[-cp:], s])
    return np.concatenate(out)


class CpLagProbeTest(unittest.TestCase):
    def run_case(self, signal, rate, fmt='cf32_le'):
        rng = np.random.default_rng(7)
        signal = signal / np.std(signal) * .05 + (rng.normal(size=signal.size) + 1j * rng.normal(size=signal.size)) * .005
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'x.iq'
            if fmt == 'cf32_le':
                signal.astype('<c8').tofile(f)
            else:
                np.column_stack([signal.real, signal.imag]).ravel().__mul__(32768).astype('<i2').tofile(f)
            return probe(f, fmt, rate, 8)

    def flagged(self, result):
        return {k for k, v in result['hypotheses'].items() if v['flagged_fraction'] > .5}

    def test_wifi_timing_only_flags_wifi(self):
        r = self.run_case(ofdm(np.random.default_rng(1), 64, 16, 16000, 52), 20e6)
        self.assertEqual(self.flagged(r), {'wifi_312.5kHz'})
        self.assertAlmostEqual(r['blind_peaks'][0]['spacing_khz'], 312.5)

    def test_fractional_lag_30khz_and_15khz_are_distinguished(self):
        rng = np.random.default_rng(2)
        for n_fft, cp, name in ((1024, 72, 'lte_like_30kHz'), (2048, 144, 'lte_like_15kHz')):
            native = ofdm(rng, n_fft, cp, 1300 * 2048 // n_fft, n_fft * 600 // 1024)   # 30.72 MSps
            r = self.run_case(resample_poly(native, 125, 192), 20e6, 'sc16_le')       # non-integer lag at 20 MSps
            self.assertEqual(self.flagged(r), {name})

    def test_strong_tone_does_not_hide_the_link(self):
        rng = np.random.default_rng(4)
        link = resample_poly(ofdm(rng, 1024, 72, 2600, 600), 125, 192)            # 30 kHz at 20 MSps
        link = link / np.std(link)
        n = np.arange(link.size)
        noise = .1 * (rng.normal(size=n.size) + 1j * rng.normal(size=n.size))      # 17 dB below the link
        tone = 100 * np.exp(2j * np.pi * 0.0137 * n)                               # 40 dB above the link
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'x.iq'
            ((link + noise + tone) / 200).astype('<c8').tofile(f)
            self.assertEqual(self.flagged(probe(f, 'cf32_le', 20e6, 8)), {'lte_like_30kHz'})
            ((noise + tone) / 200).astype('<c8').tofile(f)
            self.assertEqual(self.flagged(probe(f, 'cf32_le', 20e6, 8)), set())

    def test_subband_search_finds_a_weak_narrow_link_in_a_wide_capture(self):
        rng = np.random.default_rng(5)
        # 9 MHz-wide 15 kHz link, 22 MHz off centre in an 80 MSps capture, 10.5 dB below the noise.
        link = resample_poly(ofdm(rng, 1024, 72, 1400, 600), 125, 24)              # 15.36 MSps -> 80 MSps
        link = link / np.std(link)
        n = np.arange(link.size)
        x = .3 * link * np.exp(2j * np.pi * 22e6 / 80e6 * n) + (rng.normal(size=n.size) + 1j * rng.normal(size=n.size)) / np.sqrt(2)
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'x.iq'
            (x / 50).astype('<c8').tofile(f)
            full = probe(f, 'cf32_le', 80e6, 6)['hypotheses']['lte_like_15kHz']
            sub = probe(f, 'cf32_le', 80e6, 6, subband_hz=20e6)
        self.assertLess(full['flagged_fraction'], .3)
        self.assertGreaterEqual(sub['hypotheses']['lte_like_15kHz']['flagged_fraction'], .5)
        self.assertEqual(self.flagged(sub), {'lte_like_15kHz'})
        self.assertEqual(sub['hypotheses']['lte_like_15kHz']['best_offset_hz'], 20e6)

    def test_subband_search_keeps_noise_and_narrow_captures_unchanged(self):
        rng = np.random.default_rng(6)
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'x.iq'
            (rng.normal(size=4_400_000) + 1j * rng.normal(size=4_400_000)).astype('<c8').tofile(f)
            self.assertEqual(self.flagged(probe(f, 'cf32_le', 80e6, 8, subband_hz=20e6)), set())
            self.assertEqual(probe(f, 'cf32_le', 20e6, 8, subband_hz=20e6)['bands'], 1)

    def test_noise_flags_nothing(self):
        rng = np.random.default_rng(3)
        r = self.run_case(rng.normal(size=1_200_000) + 1j * rng.normal(size=1_200_000), 20e6)
        self.assertEqual(self.flagged(r), set())
        self.assertEqual(r['blind_peaks'], [])


if __name__ == '__main__':
    unittest.main()
