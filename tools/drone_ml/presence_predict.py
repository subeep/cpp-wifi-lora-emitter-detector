"""Bounded offline drone-system activity scoring from reviewed presence checkpoints.

Scores are experimental. All usable observations remain eligible for identity
inference until an independently qualified rejection policy exists.
"""
import argparse
import json

import numpy as np
import torch

from .common import read_source, sha256
from .dsp_triage import predict_with_triage
from .input_audit import read_interval
from .model import SpectrogramCNN
from .predict import InferenceEngine
from .presence import apply_unqualified_policy
from .presence_dataset import CONFIG, LABELS


class PresenceEngine(InferenceEngine):
    """Reuse the training STFT and inference geometry without identity decisions."""

    def __init__(self, checkpoint_path):
        with read_source(checkpoint_path) as source:
            self.checkpoint = torch.load(source, map_location='cpu', weights_only=True)
        checkpoint = self.checkpoint
        if (checkpoint.get('schema') != 'rfmon.drone_ml.presence_checkpoint.v1'
                or checkpoint.get('architecture') != 'compact_presence'
                or checkpoint.get('classes') != list(LABELS)
                or checkpoint.get('preprocessing') != CONFIG.metadata()):
            raise ValueError('incompatible presence checkpoint')
        norm = checkpoint['normalization']
        if (not np.isfinite([norm['mean'], norm['scale']]).all()
                or norm['scale'] <= 0):
            raise ValueError('invalid checkpoint normalization')
        self.config = CONFIG
        torch.set_num_threads(2)
        self.model = SpectrogramCNN(2)
        self.model.load_state_dict(checkpoint['state_dict'])
        if any(not torch.isfinite(value).all() for value in self.model.state_dict().values()):
            raise ValueError('nonfinite presence weights')
        self.model.eval()

    def _predict_image(self, image, context):
        norm = self.checkpoint['normalization']
        tensor = torch.from_numpy(((image - norm['mean']) / norm['scale'])[None, None])
        with torch.inference_mode():
            logits, _ = self.model(tensor)
            score = float(logits.softmax(1)[0, 1])
        if not np.isfinite(score):
            raise ValueError('nonfinite presence score')
        return dict(status='development_prediction_only', activity_score=score,
                    score_kind='uncalibrated_drone_system_activity_softmax',
                    score_is_calibrated_probability=False, **context)

    def assess_iq(self, iq, rate, offset=0):
        result = apply_unqualified_policy(predict_with_triage(self, iq, rate, offset))
        result.update(schema='rfmon.drone_ml.activity_observation.v1',
                      target='drone_system_rf_activity', transmitter_role='unresolved',
                      identity_route=('defer' if result['status'] == 'insufficient_evidence'
                                      else 'forward'),
                      background_rejection_enabled=False)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', required=True)
    parser.add_argument('--iq', required=True, help='Little-endian complex float32 IQ')
    parser.add_argument('--sample-rate-hz', required=True, type=int)
    parser.add_argument('--offset-samples', type=int, default=0)
    args = parser.parse_args()
    engine = PresenceEngine(args.model)
    iq, digest = read_interval(args.iq, args.sample_rate_hz, args.offset_samples)
    result = engine.assess_iq(iq, args.sample_rate_hz, args.offset_samples)
    result.update(iq_sha256=digest, model_sha256=sha256(args.model))
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
