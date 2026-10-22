"""Experimental shared CNN/causal TCN; no live checkpoint compatibility.

Independent of the existing identity/presence models. Outputs are logits and an
embedding, never accepted identities or calibrated probabilities. Weights must
be trained and qualified separately before any deployment work.
"""
import torch
from torch import nn
from torch.nn import functional as F

from .model import SpectrogramCNN

ARCHITECTURE='compact_stft_causal_tcn_v1'
DILATIONS=(1,2,4,8,16,32)


class CausalBlock(nn.Module):
    def __init__(self,channels,dilation):
        super().__init__()
        self.left=2*dilation
        self.conv=nn.Conv1d(channels,channels,3,dilation=dilation)
        # Normalize channels independently at each time position. Normalizing
        # over the time axis would leak future observations into earlier ones.
        self.norm=nn.LayerNorm(channels)

    def forward(self,x):
        y=self.conv(F.pad(x,(self.left,0)))
        y=self.norm(y.transpose(1,2)).transpose(1,2)
        return x+F.silu(y)


class TemporalDroneModel(nn.Module):
    def __init__(self,classes):
        super().__init__()
        if type(classes) is not int or not 2<=classes<=256:raise ValueError('invalid class count')
        self.classes=classes
        self.encoder=SpectrogramCNN(classes).features
        self.temporal=nn.Sequential(*(CausalBlock(64,d) for d in DILATIONS))
        self.presence_head=nn.Linear(64,2)
        self.identity_head=nn.Linear(64,classes)

    def encode(self,tiles):
        if tiles.ndim!=5 or tiles.shape[2:]!=(1,256,625):
            raise ValueError('expected B,T,1,256,625 numerical STFT tiles')
        b,t=tiles.shape[:2]
        if not 1<=b<=8 or not 1<=t<=128 or b*t>128:
            raise ValueError('temporal batch exceeds 128 tiles')
        return self.encoder(tiles.reshape(b*t,1,256,625)).reshape(b,t,64)

    def forward_features(self,features):
        if features.ndim!=3 or features.shape[-1]!=64 or not 1<=features.shape[1]<=128:
            raise ValueError('expected B,T,64 features from one uninterrupted view')
        y=self.temporal(features.transpose(1,2)).transpose(1,2)
        return dict(presence_logits=self.presence_head(y),identity_logits=self.identity_head(y),
                    embedding=F.normalize(y,dim=-1))

    def forward(self,tiles):
        sequence=self.forward_features(self.encode(tiles))
        return {k:v[:,-1] for k,v in sequence.items()}


def joint_loss(output,presence,identity,identity_weight=1.):
    """Identity loss only on verified known-drone rows; -1 means unavailable.

    A withheld unknown drone may have presence=1, identity=-1. This helper does
    not authorize training on any held-out row or assign unknown ground truth.
    """
    p=output['presence_logits'];i=output['identity_logits']
    if (p.ndim!=2 or p.shape[1]!=2 or i.ndim!=2 or i.shape[0]!=p.shape[0]
            or presence.shape!=(p.shape[0],) or identity.shape!=presence.shape
            or presence.dtype!=torch.long or identity.dtype!=torch.long):
        raise ValueError('invalid labels or logits')
    if (not torch.isfinite(torch.as_tensor(identity_weight)) or identity_weight<0
            or torch.any((presence < -1)|(presence>1))
            or torch.any((identity < -1)|(identity>=i.shape[1]))
            or torch.any((identity>=0)&(presence!=1))):
        raise ValueError('identity requires verified drone presence and valid labels')
    observed=presence>=0;known=identity>=0
    if not observed.any():raise ValueError('batch contains no reviewed presence labels')
    p_loss=F.cross_entropy(p[observed],presence[observed])
    i_loss=F.cross_entropy(i[known],identity[known]) if known.any() else i.sum()*0.
    return p_loss+identity_weight*i_loss


def description(classes):
    return dict(architecture=ARCHITECTURE,classes=classes,tile_seconds=.008,
        tile_shape=[1,256,625],encoder_channels=[16,32,64,96],embedding_dimensions=64,
        temporal_kernel=3,dilations=list(DILATIONS),receptive_field_tiles=127,
        receptive_field_seconds=127*.008,default_context_tiles=16,
        outputs=['presence_logits','identity_logits','embedding'],
        trained=False,live_qualified=False,policy=None,
        limitations=['No calibrated scores, unknown-drone decision or background guarantee.',
            'Context must remain on one frequency view, source and contiguous timeline.',
            'Offline batched inference only; no live streaming state or integration.'])
