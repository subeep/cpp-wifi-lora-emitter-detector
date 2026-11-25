"""Conservative B2 labels: own-family timing, confirmed background, or unknown.

Burst activity alone cannot identify a drone. Without a reviewed matching no-signal
control, non-OFDM recording contexts stay unknown rather than becoming positives.
"""
import argparse
from collections import Counter,defaultdict
from concurrent.futures import ThreadPoolExecutor
import hashlib
import inspect
import json
from pathlib import Path
import numpy as np
from .universal_features import extract,NAMES,VERSION
from .universal_inventory import read_json

K15='lte_like_15kHz'; K30='lte_like_30kHz'; KLB='measured_32.8kHz'
KLB2='lightbridge_10.9375kHz'   # Lightbridge's own symbol (facts v2, 24 Nov 2026); KLB is a third of it


def fired(ratios,key,limit=4):
    """Own-family timing stands out. Lightbridge: either its 32.8 kHz third-lag or its own 10.9375 kHz
    symbol lag (the app's rule, symbol_timing.cpp). With v1 facts (no KLB2) only the third-lag."""
    if key==KLB:return max(ratios.get(KLB,0),ratios.get(KLB2,0))>limit
    return ratios.get(key,0)>limit
MODELS_15={'Air 2','Air 2S','Mavic Pro','Mini 2','Mini 3','Mini 3 Pro','Mini 5 Pro',
           'Mavic 3','Mavic 3 Pro','FPV','Matrice 300','Mavic 2 Enterprise Advanced',
           'Phantom 4 RTK','DJI link module (self-built)'}   # last two: measured on DroneRFa (E4), 15 kHz only
MODELS_30={'Mini 4 Pro','Avata 2','Air 3S'}
# E4 (DroneRFa): DJI OFDM models whose link family is not settled by measurement (Matrice 30T fires
# 15 and 30 kHz about equally, Avata weakly on both): present when either OFDM timing stands out
# (the classifier's family gate allows dji_ofdm on either). data/drone_ml/2026-11-16/e4/family_measure.txt
MODELS_EITHER={'Matrice 30T','Avata'}


def label(row,facts):
    model,signal=row['model'],row['signal_type'];ratios=facts['lag_ratios']
    if model=='not_a_drone':
        if row['dataset']=='Own_X310' and row.get('background_partition') in ('train','held_out'):
            return dict(state='background',model=None,reason='confirmed_scene',model_positive=False)
        if row['dataset']=='DroneRFa':
            # The dataset's background class (town, Wi-Fi, Bluetooth): used only where no drone timing fires.
            if not any(fired(ratios,k) for k in (K15,K30,KLB)):
                return dict(state='background',model=None,reason='dataset_background_no_drone_timing',model_positive=False)
            return dict(state='unknown',model=None,reason='drone_timing_in_dataset_background',model_positive=False)
        return dict(state='unknown',model=None,reason='background_context_requires_sample_review',model_positive=False)
    if signal=='dji_ofdm' and model in MODELS_EITHER:
        best=max((K15,K30),key=lambda k:ratios.get(k,0))
        if ratios.get(best,0)>4:
            return dict(state='signal_present',model=model,reason='own_family_timing',expected_timing=best,model_positive=True)
        return dict(state='unknown',model=None,reason='own_family_not_detected_not_a_negative_label',expected_timing='15/30kHz',model_positive=False)
    expected=KLB if signal=='lightbridge' else K15 if row['dataset']=='DroneRFb-DIR' or model in MODELS_15 else K30 if model in MODELS_30 else None
    if expected:
        if fired(ratios,expected):
            return dict(state='signal_present',model=model,reason='own_family_timing',expected_timing=expected,model_positive=True)
        return dict(state='unknown',model=None,reason='own_family_not_detected_not_a_negative_label',expected_timing=expected,model_positive=False)
    return dict(state='unknown',model=None,reason='matching_source_no_signal_control_missing',model_positive=False)


def process(store,output,workers=4):
    if not 1<=workers<=8:raise ValueError('workers must be 1..8')
    store,output=Path(store),Path(output);output.mkdir(parents=True,exist_ok=True)
    config=read_json(store/'config.json');counts=defaultdict(Counter);records=0;window_count=0
    source_complete=(store/'report.json').exists() and read_json(store/'report.json')['complete']
    code_hash=hashlib.sha256((inspect.getsource(extract)+inspect.getsource(label)).encode()).hexdigest()
    def process_record(path):
        meta=read_json(path.with_suffix('.json'));row=meta['source_record']
        result_path=output/(path.stem+'.json');array_path=result_path.with_suffix('.npz')
        if result_path.exists() and array_path.exists():
            cached=read_json(result_path)
            if cached.get('view_iq_sha256')==meta['iq_sha256'] and cached.get('code_hash')==code_hash:
                if hashlib.sha256(array_path.read_bytes()).hexdigest()!=cached.get('feature_file_sha256'):
                    raise ValueError('cached feature file changed')
                return row['model'],cached['labels']
        iq=np.load(path,mmap_mode='r',allow_pickle=False)
        if hashlib.sha256(iq.tobytes()).hexdigest()!=meta['iq_sha256']:raise ValueError('view hash mismatch')
        vectors=[];available=[];labels=[]
        for index,x in enumerate(iq):
            facts=extract(x);decision=label(row,facts)
            vectors.append(facts['values']);available.append(facts['available']);labels.append(decision)
        np.savez(array_path,values=np.stack(vectors),available=np.stack(available))
        result_path.write_text(json.dumps(dict(recording_id=path.stem,
            view_iq_sha256=meta['iq_sha256'],labels=labels,code_hash=code_hash,
            feature_file_sha256=hashlib.sha256(array_path.read_bytes()).hexdigest()),indent=2)+'\n')
        return row['model'],labels
    # A running B1 writer publishes metadata last. Only fully published records enter this snapshot.
    paths=[p for p in sorted(store.glob('*.npy')) if p.with_suffix('.json').exists()]
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for model,labels in pool.map(process_record,paths):
            for decision in labels:
                counts[model][decision['state']]+=1;window_count+=1
            records+=1
            if records%25==0:print(json.dumps(dict(records=records,windows=window_count)),flush=True)
    report=dict(schema='rfmon.drone_ml.universal_presence.v1',source_store=str(store.resolve()),
                source_store_complete=source_complete,records=records,windows=window_count,feature_names=NAMES,
                feature_version=VERSION,by_class={k:dict(v) for k,v in sorted(counts.items())},
                manual_spectrogram_review_complete=False,qualification_ready=False,
                blockers=['20 spectrograms per class not yet visually reviewed',
                          'same-source no-signal controls missing for non-OFDM classes',
                          'undetected drone-recording windows require review before any negative assignment'])
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--store',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--workers',type=int,default=4);a=p.parse_args()
    report=process(a.store,a.output,a.workers);print(json.dumps({k:v for k,v in report.items() if k not in ('by_class','feature_names')}))
if __name__=='__main__':main()
