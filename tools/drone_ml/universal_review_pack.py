"""Prepare 20 deterministic spectrogram windows per class for B2 visual review.

Contact sheets and a manifest are evidence for manual inspection, not automatic approval.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from .preprocess import spectrogram,STFTConfig
from .universal_inventory import read_json
from .universal_features import extract
from .universal_presence import label


def prepare(store,output,models=None):
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    grouped=defaultdict(list)
    for path in sorted(Path(store).glob('*.json')):
        if len(path.stem)!=24:continue
        meta=read_json(path);row=meta['source_record']
        if models and row['model'] not in models:continue
        for i,window in enumerate(meta['windows']):
            grouped[row['model']].append((path,i,row,window))
    report=[]
    for model,rows in sorted(grouped.items()):
        ids=np.unique(np.linspace(0,len(rows)-1,min(20,len(rows)),dtype=int))
        chosen=[rows[i] for i in ids];fig,axes=plt.subplots(5,4,figsize=(16,12),constrained_layout=True)
        fig.suptitle(model+' — deterministic review sample (not approved labels)',fontsize=16)
        entries=[]
        for ax,(path,index,row,window) in zip(axes.flat,chosen):
            x=np.load(path.with_suffix('.npy'),mmap_mode='r',allow_pickle=False)[index]
            facts=extract(x);decision=label(row,facts)
            power=spectrogram(x,20_000_000,STFTConfig(duration_seconds=.008,hop=256))
            im=ax.imshow(power,origin='lower',aspect='auto',extent=[0,8,-10,10],vmin=-40,vmax=10,cmap='magma')
            ax.set_title(f"{row['dataset']} {row['recording_id'][:6]}:{index}\n{decision['state']}",fontsize=7)
            ax.set_xlabel('ms',fontsize=7);ax.set_ylabel('MHz from view centre',fontsize=7)
            ax.tick_params(labelsize=6)
            entries.append(dict(recording_id=row['recording_id'],source=row['source'],member=row.get('member'),
                view_index=index,window=window,automatic_decision=decision,lag_ratios=facts['lag_ratios']))
        for ax in list(axes.flat)[len(chosen):]:ax.axis('off')
        fig.colorbar(im,ax=axes.ravel().tolist(),label='dB after per-window RMS normalization',shrink=.8)
        stem=hashlib.sha256(model.encode()).hexdigest()[:12]
        image=output/(stem+'.png');fig.savefig(image,dpi=100);plt.close(fig)
        entry=dict(model=model,image=str(image.resolve()),windows=entries,visually_reviewed=False)
        (output/(stem+'.json')).write_text(json.dumps(entry,indent=2)+'\n');report.append(entry)
    (output/'manifest.json').write_text(json.dumps(dict(classes=report,review_complete=False),indent=2)+'\n')
    print(json.dumps({r['model']:r['image'] for r in report}),flush=True)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--store',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--model',action='append');a=p.parse_args()
    prepare(a.store,a.output,a.model)
if __name__=='__main__':main()
