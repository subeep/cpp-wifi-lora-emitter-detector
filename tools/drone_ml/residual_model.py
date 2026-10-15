"""Larger numerical-STFT encoder. Instantiation never trains or loads weights."""
import torch
from torch import nn
from torch.nn import functional as F

ARCHITECTURE = 'rf_stft_residual_v1'


class ResidualBlock(nn.Module):
    def __init__(self, incoming, outgoing, stride=1):
        super().__init__()
        self.body=nn.Sequential(
            nn.Conv2d(incoming,outgoing,3,stride=stride,padding=1,bias=False),
            nn.GroupNorm(8,outgoing),nn.SiLU(),
            nn.Conv2d(outgoing,outgoing,3,padding=1,bias=False),nn.GroupNorm(8,outgoing))
        self.skip=nn.Identity() if incoming==outgoing and stride==1 else nn.Sequential(
            nn.Conv2d(incoming,outgoing,1,stride=stride,bias=False),nn.GroupNorm(8,outgoing))
    def forward(self,x):return F.silu(self.body(x)+self.skip(x))


class RFResidualCNN(nn.Module):
    def __init__(self, classes):
        super().__init__()
        if type(classes) is not int or classes<2:raise ValueError('at least two known classes required')
        self.stem=nn.Sequential(nn.Conv2d(1,32,5,stride=2,padding=2,bias=False),nn.GroupNorm(8,32),nn.SiLU())
        self.stage1=nn.Sequential(ResidualBlock(32,32),ResidualBlock(32,32))
        self.stage2=nn.Sequential(ResidualBlock(32,64,2),ResidualBlock(64,64))
        self.stage3=nn.Sequential(ResidualBlock(64,128,2),ResidualBlock(128,128))
        self.stage4=nn.Sequential(ResidualBlock(128,192,2),ResidualBlock(192,192))
        self.pool=nn.AdaptiveAvgPool2d((2,4))
        self.embedding=nn.Sequential(nn.Flatten(),nn.Linear(192*2*4,128),nn.LayerNorm(128),nn.SiLU())
        self.dropout=nn.Dropout(.2)
        self.classifier=nn.Linear(128,classes,bias=False)
    def forward(self,x):
        if x.ndim!=4 or x.shape[1:]!=(1,256,625):raise ValueError('requires B x 1 x 256 x 625 STFT')
        x=self.stem(x)
        for stage in (self.stage1,self.stage2,self.stage3,self.stage4):x=stage(x)
        embedding=F.normalize(self.embedding(self.pool(x)),dim=1)
        # Fixed cosine temperature; scores require separate held-group calibration.
        logits=F.linear(F.normalize(self.dropout(embedding),dim=1),F.normalize(self.classifier.weight,dim=1))/.1
        return logits,embedding


def describe(classes):
    """Forward-only architecture audit; no optimizer, backward pass or weight file."""
    torch.manual_seed(17);torch.set_num_threads(2)
    model=RFResidualCNN(classes).eval();before={k:v.clone() for k,v in model.state_dict().items()}
    layers=[];handles=[]
    for name,module in model.named_children():
        if name not in ('dropout','classifier'):
            handles.append(module.register_forward_hook(lambda m,i,o,n=name:layers.append(dict(layer=n,shape=list(o.shape)))))
    with torch.no_grad():logits,embedding=model(torch.zeros(1,1,256,625))
    for handle in handles:handle.remove()
    assert all(torch.equal(v,before[k]) for k,v in model.state_dict().items())
    return dict(architecture=ARCHITECTURE,parameters=sum(p.numel() for p in model.parameters()),
        convolution_layers=sum(isinstance(m,nn.Conv2d) for m in model.modules()),
        residual_blocks=8,embedding_dimensions=128,classes=classes,layers=layers,
        logits_shape=list(logits.shape),embedding_shape=list(embedding.shape),
        forward_only=True,weights_unchanged=True,optimizer_steps=0,trained=False,
        input_shape=[1,1,256,625],precision='float32',binary_drone_head=False,
        unknown_handling='held_class_calibration_after_training_not_an_unknown_softmax_class')
