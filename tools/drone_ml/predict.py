"""Standalone reusable IQ/FFT inference, with experimental output status."""
import argparse
import json
import math
from pathlib import Path
import numpy as np
import torch

from .common import read_source, stamp
from .fft_input import read_fft, spectrogram_from_fft
from .model import SpectrogramCNN
from .preprocess import from_metadata, spectrogram
from .train import accept


class InferenceEngine:
    """Load once and evaluate bounded intervals; no receiver or GUI ownership."""

    def __init__(self, checkpoint_path):
        with read_source(checkpoint_path) as f:
            self.checkpoint = torch.load(f, map_location='cpu', weights_only=True)
        schema = self.checkpoint.get('schema')
        if schema not in ('rfmon.drone_ml.checkpoint.v1', 'rfmon.drone_ml.expanded_checkpoint.v1'):
            raise ValueError('unsupported checkpoint')
        self.config = from_metadata(self.checkpoint['preprocessing'])
        self.config.validate()
        torch.set_num_threads(2)
        if schema == 'rfmon.drone_ml.expanded_checkpoint.v1':
            from .residual_model import RFResidualCNN
            architecture = self.checkpoint.get('architecture')
            if architecture not in ('residual', 'baseline'):
                raise ValueError('unsupported expanded model architecture')
            factory = RFResidualCNN if architecture == 'residual' else SpectrogramCNN
        else:
            factory = SpectrogramCNN
        self.model = factory(len(self.checkpoint['classes']))
        self.model.load_state_dict(self.checkpoint['state_dict'])
        self.model.eval()

    def predict_iq(self, iq, rate, offset=0):
        if type(offset) is not int or offset < 0:
            raise ValueError('invalid IQ interval offset')
        try:
            image, metadata = spectrogram(iq, rate, self.config, return_metadata=True)
        except ValueError as e:
            return dict(status='insufficient_evidence', reason=str(e), accepted_class=None)
        return self._predict_image(image, dict(input_kind='iq',
                                  selected_offset_hz=metadata['selected_offset_hz'],
                                  source_interval_samples=[offset, offset + len(iq)]))

    def predict_fft(self, frames, metadata):
        # Incompatible FFT contracts are caller errors, not classifier decisions.
        image = spectrogram_from_fft(frames, metadata, self.config)
        return self._predict_image(image, dict(input_kind='fft',
                                  source_interval_samples=[metadata['interval_start_sample'],
                                    metadata['interval_start_sample'] + round(
                                        metadata['source_rate_hz'] * self.config.duration_seconds)],
                                  selected_offset_hz=metadata['selected_offset_hz']))

    def _predict_image(self, image, context):
        checkpoint = self.checkpoint
        if checkpoint.get('activity_policy'):
            from .activity import assess
            activity = assess(image, checkpoint['activity_policy'])
            if not activity['eligible']:
                return dict(status='insufficient_evidence', reason='structured_rf_proxy_below_threshold',
                            activity=activity, accepted_class=None, **context)
        norm = checkpoint['normalization']
        if not np.isfinite([norm['mean'], norm['scale']]).all() or norm['scale'] <= 0:
            raise ValueError('invalid checkpoint normalization')
        with torch.no_grad():
            logits, embedding = self.model(torch.from_numpy(((image - norm['mean']) / norm['scale'])[None, None]))
        probabilities = logits.softmax(1).numpy()
        best = int(probabilities.argmax())
        result = dict(status='development_prediction_only', accepted_class=None,
                      candidate=checkpoint['classes'][best], softmax_score=float(probabilities[0, best]),
                      top_candidates=[dict(label=checkpoint['classes'][int(k)],score=float(probabilities[0,k]))
                                      for k in np.argsort(probabilities[0])[::-1][:3]],
                      class_scores=[dict(label=label,score=float(probabilities[0,k]))
                                    for k,label in enumerate(checkpoint['classes'])],
                      score_kind='uncalibrated_known_class_softmax',
                      score_is_calibrated_probability=False, **context,
                      model_validated_for_live_use=False)
        if checkpoint.get('policy_blockers') and checkpoint['policy'] is None:
            result.update(status='identification_unavailable', reason=checkpoint['policy_blockers'])
        if not checkpoint.get('development_smoke', False) and checkpoint['policy'] is not None:
            distances = np.maximum(0, 1 - embedding.numpy() @ np.asarray(checkpoint['prototypes']).T)
            selected = int(accept(probabilities, distances, checkpoint['policy'])[0])
            result.update(status='known_class_candidate' if selected >= 0 else 'unknown_rf_signal',
                          accepted_class=checkpoint['classes'][selected] if selected >= 0 else None)
        return result


def predict(checkpoint_path, path, fmt, rate, offset=0):
    """Backward-compatible bounded IQ-file entry point."""
    if (type(offset) is not int or offset < 0 or fmt not in ('cf32_le', 'ci16_le')
            or not math.isfinite(rate) or rate != int(rate) or not 0 < rate <= 1_000_000_000):
        raise ValueError('invalid IQ interval, rate or format')
    engine = InferenceEngine(checkpoint_path)
    count = round(rate * engine.config.duration_seconds)
    if count <= 0 or count > 4_000_000:
        raise ValueError('IQ interval exceeds budget')
    width = 8 if fmt == 'cf32_le' else 4
    before = stamp(path)
    with read_source(path) as f:
        if before[2] % width or (offset + count) * width > before[2]:
            raise ValueError('short or misaligned source')
        f.seek(offset * width)
        payload = f.read(count * width)
    if stamp(path) != before or len(payload) != count * width:
        raise ValueError('source changed or short read')
    if width == 8:
        iq = np.frombuffer(payload, dtype='<c8')
    else:
        v = np.frombuffer(payload, dtype='<i2').reshape(-1, 2).astype(np.float32)
        iq = v[:, 0] + 1j * v[:, 1]
    return engine.predict_iq(iq, rate, offset)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--model', type=Path, required=True)
    inputs = p.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--iq', type=Path)
    inputs.add_argument('--fft', type=Path, help='time-by-frequency numerical NPY array')
    p.add_argument('--fft-metadata', type=Path)
    p.add_argument('--format', choices=('cf32_le', 'ci16_le'))
    p.add_argument('--sample-rate-hz', type=int)
    p.add_argument('--offset-samples', type=int, default=0)
    a = p.parse_args()
    if a.iq:
        if a.format is None or a.sample_rate_hz is None or a.fft_metadata is not None:
            p.error('IQ input requires --format and --sample-rate-hz, without FFT metadata')
        result = predict(a.model, a.iq, a.format, a.sample_rate_hz, a.offset_samples)
    else:
        if a.fft_metadata is None or a.format is not None or a.sample_rate_hz is not None or a.offset_samples:
            p.error('FFT input requires --fft-metadata; rate and origin come from metadata')
        result = InferenceEngine(a.model).predict_fft(*read_fft(a.fft, a.fft_metadata))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
