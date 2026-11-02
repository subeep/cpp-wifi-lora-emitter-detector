"""Conservative input policy for the first hybrid-model comparison.

Keep receiver-specific ADC knowledge as audit metadata. Its availability differs
by source dataset and must not become a shortcut into aircraft identity.
"""
import torch
from .hybrid_model import FEATURES

POLICY = 'common_dsp_only_v1'
INDICES = (0, 1, 2, 3, 4)


def common_dsp_inputs(measurements, available):
    if (measurements.ndim != 3 or measurements.shape[-1] != len(FEATURES)
            or available.shape != measurements.shape or available.dtype != torch.bool
            or measurements.device != available.device):
        raise ValueError('invalid hybrid measurement contract')
    allowed = torch.zeros(len(FEATURES), dtype=torch.bool, device=available.device)
    allowed[list(INDICES)] = True
    mask = available & allowed
    return torch.where(mask, measurements, torch.zeros_like(measurements)), mask


def description():
    return dict(policy=POLICY, features=list(FEATURES), permitted_indices=list(INDICES),
        excluded=['source-dependent ADC rail availability', 'unimplemented ZC/bandwidth/duty measurements'],
        qualification=False, limitation='Common DSP features can still carry receiver/environment shortcuts; grouped and RF-only ablations remain required.')
