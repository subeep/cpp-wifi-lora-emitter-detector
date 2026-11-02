"""Experimental STFT + masked DSP + causal context model, offline only.

No decoder success gate, no serial/product fields, and no conversion of scores
into identities. All measurements must belong to the same receiver view, finish no later than
the tile end and stay within its contiguous context; late/unavailable values
remain masked. Outputs are available at tile end, not at the tile start.
An O4-profile correlation is a feature, not a positive presence label.
"""
import math
import torch
from torch import nn
from torch.nn import functional as F

from .temporal_model import TemporalDroneModel, joint_loss, DILATIONS

ARCHITECTURE = 'stft_masked_dsp_causal_tcn_v1'
# Fixed order and normalization are part of the checkpoint contract. Do not
# populate unavailable fields with zero while claiming they were measured.
FEATURES = ('o4_cp_correlation', 'o4_lag_prominence', 'o4_cyclic_correlation',
            'o4_cyclic_prominence', 'o4_harmonic_correlation', 'clipped_fraction',
            'zc_correlation', 'occupied_bandwidth_fraction', 'burst_duty_cycle')
PROMINENCE = (1, 3)


class HybridDroneModel(TemporalDroneModel):
    def __init__(self, classes):
        super().__init__(classes)
        self.dsp_encoder = nn.Sequential(nn.Linear(2*len(FEATURES), 32), nn.SiLU(),
                                         nn.Linear(32, 32), nn.SiLU())
        self.fusion = nn.Sequential(nn.Linear(96, 64), nn.SiLU())

    def forward_features(self, features, measurements, available, valid, segment_start):
        """Return per-tile logits, embedding and validity; reset on every gap.

        segment_start marks retunes/session changes even without a missing tile.
        Missing tiles also break continuity. No state survives between calls;
        callers must provide overlapping context if they need a sliding view.
        """
        if features.ndim != 3 or features.shape[-1] != 64:
            raise ValueError('expected B,T,64 STFT features')
        b, t = features.shape[:2]
        if not 1 <= b <= 8 or not 1 <= t <= 128 or b*t > 128:
            raise ValueError('batch exceeds 128 tiles')
        if (measurements.shape != (b, t, len(FEATURES)) or available.shape != measurements.shape
                or valid.shape != (b, t) or segment_start.shape != (b, t)
                or any(x.dtype != torch.bool for x in (available, valid, segment_start))
                or any(x.device != features.device for x in (measurements, available, valid, segment_start))):
            raise ValueError('invalid DSP, validity or continuity masks')
        if not torch.isfinite(features[valid]).all():
            raise ValueError('non-finite observed STFT features')
        observed = available & valid.unsqueeze(-1)
        if not torch.isfinite(measurements[observed]).all() or (measurements[observed] < 0).any():
            raise ValueError('invalid observed DSP measurement')
        x = torch.where(observed, measurements, torch.zeros_like(measurements))
        bounded = [i for i in range(len(FEATURES)) if i not in PROMINENCE]
        if (x[..., bounded] > 1.00001).any():
            raise ValueError('correlation and fraction features must be in [0,1]')
        # Prominence is a nonnegative ratio; bound large outliers deterministically.
        columns = [torch.log1p(x[..., i].clamp(max=100))/math.log(101)
                   if i in PROMINENCE else x[..., i].clamp(max=1)
                   for i in range(len(FEATURES))]
        dsp = self.dsp_encoder(torch.cat((torch.stack(columns, -1), observed.to(x.dtype)), -1))
        safe_features = torch.where(valid.unsqueeze(-1), features, torch.zeros_like(features))
        fused = self.fusion(torch.cat((safe_features, dsp), -1))
        rows = []
        for row in range(b):
            pieces = []
            i = 0
            while i < t:
                if not valid[row, i]:
                    pieces.append(torch.zeros_like(fused[row, i:i+1])); i += 1
                    continue
                end = i+1
                while end < t and valid[row, end] and not segment_start[row, end]:
                    end += 1
                piece = self.temporal(fused[row:row+1, i:end].transpose(1, 2))
                pieces.append(piece.transpose(1, 2)[0]); i = end
            rows.append(torch.cat(pieces, 0))
        y = torch.stack(rows)
        mask = valid.unsqueeze(-1)
        return dict(presence_logits=self.presence_head(y).masked_fill(~mask, 0),
                    identity_logits=self.identity_head(y).masked_fill(~mask, 0),
                    embedding=F.normalize(y, dim=-1).masked_fill(~mask, 0), valid=valid,
                    segment_start=segment_start)

    def forward(self, tiles, measurements, available, valid, segment_start):
        if tiles.ndim != 5 or tiles.shape[2:] != (1, 256, 625):
            raise ValueError('expected B,T,1,256,625 numerical STFT tiles')
        b, t = tiles.shape[:2]
        if not 1 <= b <= 8 or not 1 <= t <= 128 or b*t > 128 or valid.shape != (b, t) or valid.dtype != torch.bool:
            raise ValueError('invalid tile batch or mask')
        # Invalid tiles need not exist numerically and never enter the CNN.
        selected = tiles[valid]
        if not torch.isfinite(selected).all():
            raise ValueError('non-finite observed STFT')
        features = tiles.new_zeros(b, t, 64)
        if len(selected):
            features[valid] = self.encoder(selected)
        return self.forward_features(features, measurements, available, valid, segment_start)


def region_loss(output, presence, identity, identity_weight=1.):
    """Only for adjudicated per-tile regions, never inherited bag labels."""
    valid = output['valid']
    if presence.shape != valid.shape or identity.shape != valid.shape:
        raise ValueError('region labels must match the temporal grid')
    if ((presence != -1) & ~valid).any() or ((identity != -1) & ~valid).any():
        raise ValueError('invalid tiles cannot carry labels')
    return joint_loss({k: output[k][valid] for k in ('presence_logits', 'identity_logits')},
                      presence[valid], identity[valid], identity_weight)


def bag_loss(output, presence, identity, identity_weight=1.):
    """Weak bag objective, not evidence that every tile contains its model.

    Negative bags supervise all valid tiles as absent. Positive bags supervise
    a pooled presence margin; identity pooling uses detached presence attention.
    This objective is experimental and needs ablations against region labels.
    Bags must stay inside a reviewed source/member and one continuity segment.
    Unknown drone bags use identity=-1; unavailable presence is not supervised.
    """
    p, i, valid = output['presence_logits'], output['identity_logits'], output['valid']
    if (p.ndim != 3 or p.shape[-1] != 2 or i.ndim != 3 or i.shape[:2] != p.shape[:2]
            or valid.shape != p.shape[:2] or valid.dtype != torch.bool
            or presence.shape != (len(p),) or identity.shape != presence.shape
            or presence.dtype != torch.long or identity.dtype != torch.long):
        raise ValueError('invalid bag labels or outputs')
    if (not math.isfinite(identity_weight) or identity_weight < 0
            or ((presence < -1) | (presence > 1)).any()
            or ((identity < -1) | (identity >= i.shape[-1])).any()
            or ((identity >= 0) & (presence != 1)).any()):
        raise ValueError('identity requires reviewed positive bag context')
    losses = []
    for n in range(len(p)):
        if presence[n] < 0:
            continue
        pp, ii = p[n, valid[n]], i[n, valid[n]]
        if not len(pp):
            raise ValueError('labelled bag has no observed tiles')
        indices = valid[n].nonzero().flatten()
        if (indices[-1]-indices[0]+1 != len(indices)
                or output['segment_start'][n, indices[1:]].any()):
            raise ValueError('bag must be one uninterrupted receiver view')
        if presence[n] == 0:
            losses.append(F.cross_entropy(pp, torch.zeros(len(pp), dtype=torch.long, device=pp.device)))
        else:
            margin = pp[:, 1]-pp[:, 0]
            pooled = torch.logsumexp(margin, 0)-math.log(len(margin))
            loss = F.softplus(-pooled)
            if identity[n] >= 0:
                weights = torch.softmax(margin.detach(), 0)
                logits = (weights[:, None]*ii).sum(0, keepdim=True)
                loss = loss + identity_weight*F.cross_entropy(logits, identity[n:n+1])
            losses.append(loss)
    if not losses:
        raise ValueError('no reviewed bag labels')
    return torch.stack(losses).mean()


def description(classes):
    return dict(architecture=ARCHITECTURE, classes=classes, trained=False, live_qualified=False,
        tile_seconds=.008, tile_shape=[1, 256, 625], features=list(FEATURES),
        dsp_layers=[18, 32, 32], fusion_layers=[96, 64], encoder_channels=[16, 32, 64, 96],
        temporal_dilations=list(DILATIONS), receptive_field_tiles=127,
        maximum_context_seconds=1.024, default_context_tiles=32, outputs_at="tile_end", reset_on=['missing_tile', 'segment_start'],
        outputs=['presence_logits', 'identity_logits', 'embedding', 'valid'],
        policy=None, limitations=[
            'Untrained research candidate; no measured accuracy or probability calibration.',
            'Same-interval DSP extraction/join and streaming deployment are not implemented.',
            'Native O4 profile is not generic drone evidence; use missing-feature and source-held-out ablations.',
            'Bag presence is weak supervision; no automatic per-tile identity labels.',
            'Unknown decisions require withheld-model evaluation and a separately frozen acceptance policy.',
            'Missingness can reveal source/receiver; train with feature dropout and test RF-only ablation.'])
