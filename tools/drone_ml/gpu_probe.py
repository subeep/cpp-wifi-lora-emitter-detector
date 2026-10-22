"""Bounded GPU recovery/throughput probe on training-only data; never saves weights."""
import argparse
import json
from pathlib import Path
import time
import os
os.environ.setdefault('CUBLAS_WORKSPACE_CONFIG',':4096:8')
import torch
from torch.utils.data import DataLoader
from .expanded_training import ShardedDataset,cross_unit_contrastive
from .residual_model import RFResidualCNN
from .common import write_json,sha256


def probe(root,output):
    root=Path(root)
    torch.set_num_threads(2);torch.manual_seed(17);torch.use_deterministic_algorithms(True)
    torch.backends.cudnn.benchmark=False;torch.backends.cudnn.deterministic=True
    torch.backends.cudnn.allow_tf32=False;torch.backends.cuda.matmul.allow_tf32=False
    if not torch.cuda.is_available():raise RuntimeError('CUDA unavailable')
    # This file is replaced atomically by the CPU worker. Snapshot its bytes once.
    import io
    checkpoint_bytes=(root/'residual_model/training_state.pt').read_bytes()
    state=torch.load(io.BytesIO(checkpoint_bytes),map_location='cpu',weights_only=True)
    import hashlib
    digest=hashlib.sha256(checkpoint_bytes).hexdigest()
    m=json.loads((root/'corpus/corpus.json').read_text())
    rows=[r for r in m['records'] if r['partition']=='train' and r['model_eligible']]
    x,y,indices=next(iter(DataLoader(ShardedDataset(root/'corpus',m,rows),batch_size=8)))
    model=RFResidualCNN(len(m['classes'])).eval();model.load_state_dict(state['model'])
    with torch.no_grad():cpu=model(x)[0]
    model=model.cuda()
    with torch.no_grad():gpu=model(x.cuda())[0].cpu()
    torch.testing.assert_close(cpu,gpu,rtol=2e-4,atol=2e-4)
    optimizer=torch.optim.AdamW(model.parameters(),lr=3e-4,weight_decay=.01)
    optimizer.load_state_dict(state['optimizer'])
    model.train();times=[];losses=[];torch.cuda.reset_peak_memory_stats()
    for i in range(12):
        torch.cuda.synchronize();start=time.perf_counter()
        optimizer.zero_grad();logits,emb=model(x.cuda());labels=y.cuda()
        loss=torch.nn.functional.cross_entropy(logits,labels,label_smoothing=.05)
        loss+=.1*cross_unit_contrastive(emb,labels,[rows[int(k)]['split_group'] for k in indices])
        if not torch.isfinite(loss):raise ValueError('nonfinite GPU loss')
        loss.backward();torch.nn.utils.clip_grad_norm_(model.parameters(),5,error_if_nonfinite=True);optimizer.step()
        torch.cuda.synchronize();times.append(time.perf_counter()-start);losses.append(float(loss.detach()))
    report=dict(gpu=torch.cuda.get_device_name(),torch_version=str(torch.__version__),cuda_version=torch.version.cuda,
        checkpoint_sha256=digest,completed_epochs=state['completed_epochs'],corpus_sha256=sha256(root/'corpus/corpus.json'),
        cpu_gpu_max_logit_difference=float((cpu-gpu).abs().max()),batch_size=8,precision='float32',
        measured_steps=10,mean_step_seconds=sum(times[2:])/10,
        peak_allocated_bytes=torch.cuda.max_memory_allocated(),peak_reserved_bytes=torch.cuda.max_memory_reserved(),
        losses=losses,weights_saved=False,scope='training-only fixed batch, warm GPU; excludes disk loading and full-run overhead')
    write_json(output,report);print(json.dumps(report,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--job',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args();probe(a.job,a.output)
