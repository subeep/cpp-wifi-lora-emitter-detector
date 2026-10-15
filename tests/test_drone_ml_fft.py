"""FFT/IQ equivalence and rejection of incompatible producer inputs."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.drone_ml.fft_input import read_fft, spectrogram_from_fft
from tools.drone_ml.model import SpectrogramCNN
from tools.drone_ml.predict import InferenceEngine
from tools.drone_ml.preprocess import STFTConfig, fft_frames, spectrogram


class FFTTests(unittest.TestCase):
    def setUp(self):
        self.cfg = STFTConfig()
        rng = np.random.default_rng(82)
        self.iq = (rng.normal(size=20000) + 1j * rng.normal(size=20000)
                   + 2 * np.exp(2j * np.pi * .12 * np.arange(20000))).astype(np.complex64)
        self.frames, self.meta = fft_frames(self.iq, 20000000, self.cfg)

    def test_iq_equivalence_across_views_and_contexts(self):
        for cfg, rate in [(self.cfg, 20000000),
                          (STFTConfig(offset_hz=20000000), 100000000),
                          (STFTConfig(auto_frequency_view=True), 100000000),
                          (STFTConfig(auto_frequency_view=True, duration_seconds=.008, hop=256), 100000000)]:
            n = round(rate * cfg.duration_seconds)
            rng = np.random.default_rng(91)
            iq = rng.normal(size=n) + 1j * rng.normal(size=n)
            frames, meta = fft_frames(iq, rate, cfg)
            np.testing.assert_array_equal(spectrogram(iq, rate, cfg), spectrogram_from_fft(frames, meta, cfg))

    def test_scaled_natural_order_complex_and_power(self):
        expected = spectrogram(self.iq, 20000000, self.cfg)
        for kind in ('complex_fft', 'power_fft'):
            meta = dict(self.meta, representation=kind, bin_order='natural', forward_scale=1 / 256)
            frames = np.fft.ifftshift(self.frames, axes=1) / 256
            if kind == 'power_fft':
                frames = np.abs(frames) ** 2
            np.testing.assert_allclose(spectrogram_from_fft(frames, meta, self.cfg), expected, atol=1e-5)

    def test_wrong_contracts_rejected(self):
        bad = [dict(bin_order='one_sided'), dict(representation='db'), dict(forward_scale=0),
               dict(forward_scale=float('nan')), dict(input_conditioning='raw_adc'),
               dict(dropped_samples=1), dict(interval_start_sample=-1),
               dict(source_rate_hz=10000000), dict(selected_offset_hz=20000000),
               dict(preprocessing=STFTConfig(hop=256).metadata())]
        for change in bad:
            with self.subTest(change=change), self.assertRaises(ValueError):
                spectrogram_from_fft(self.frames, dict(self.meta, **change), self.cfg)

    def test_missing_duplicate_and_out_of_order_frames_rejected(self):
        for starts in (self.meta['frame_start_samples'][1:],
                       [0] * len(self.frames), self.meta['frame_start_samples'][::-1]):
            with self.assertRaises(ValueError):
                spectrogram_from_fft(self.frames, dict(self.meta, frame_start_samples=starts), self.cfg)
        with self.assertRaises(ValueError):
            spectrogram_from_fft(self.frames.mean(axis=0, keepdims=True), self.meta, self.cfg)

    def test_bad_values_and_dtype_rejected(self):
        for frames in (np.full_like(self.frames, np.nan), np.zeros_like(self.frames),
                       self.frames.real, self.frames.astype(object)):
            with self.assertRaises(ValueError):
                spectrogram_from_fft(frames, self.meta, self.cfg)
        with self.assertRaises(ValueError):
            spectrogram_from_fft(-np.ones(self.frames.shape), dict(self.meta, representation='power_fft'), self.cfg)

    def test_file_read_roundtrip_and_pickle_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            p, m = root / 'frames.npy', root / 'frames.json'
            np.save(p, self.frames)
            m.write_text(json.dumps(self.meta))
            frames, metadata = read_fft(p, m)
            np.testing.assert_array_equal(frames, self.frames)
            self.assertEqual(metadata, self.meta)
            np.save(p, np.array([[object()]], dtype=object))
            with self.assertRaises(ValueError):
                read_fft(p, m)

    def test_hostile_shape_rejected_before_allocation(self):
        with tempfile.TemporaryDirectory() as temp:
            p, m = Path(temp) / 'frames.npy', Path(temp) / 'frames.json'
            with p.open('wb') as f:
                np.lib.format.write_array_header_1_0(f, dict(descr='<c16', fortran_order=False, shape=(10**9, 256)))
            m.write_text(json.dumps(self.meta))
            with self.assertRaises(ValueError):
                read_fft(p, m)

    def test_reused_engine_parity_and_policy_status(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp) / 'model.pt'
            torch.manual_seed(1)
            checkpoint = dict(schema='rfmon.drone_ml.checkpoint.v1', preprocessing=self.cfg.metadata(),
                              normalization=dict(mean=-10., scale=10.), classes=['synthetic_a', 'synthetic_b'],
                              state_dict=SpectrogramCNN(2).state_dict(), policy=None,
                              policy_blockers=['insufficient_validation'], development_smoke=False)
            torch.save(checkpoint, p)
            engine = InferenceEngine(p)
            a = engine.predict_iq(self.iq, 20000000)
            b = engine.predict_fft(self.frames, self.meta)
            for key in ('status', 'candidate', 'softmax_score', 'accepted_class', 'reason'):
                self.assertEqual(a[key], b[key])
            self.assertEqual(b['status'], 'identification_unavailable')
            self.assertIsNone(b['accepted_class'])
            self.assertFalse(b['model_validated_for_live_use'])
            checkpoint['activity_policy'] = dict(version=1, block_frames=8, contrast_db=6,
                minimum_structured_fraction=.1, core_fraction=.75, interpretation='structured_rf_proxy_not_drone_truth')
            torch.save(checkpoint, p)
            engine = InferenceEngine(p)
            rng = np.random.default_rng(23)
            noise = rng.normal(size=20000) + 1j * rng.normal(size=20000)
            frames, metadata = fft_frames(noise, 20000000, self.cfg)
            self.assertEqual(engine.predict_iq(noise, 20000000)['status'], 'insufficient_evidence')
            self.assertEqual(engine.predict_fft(frames, metadata)['status'], 'insufficient_evidence')


if __name__ == '__main__':
    unittest.main()
