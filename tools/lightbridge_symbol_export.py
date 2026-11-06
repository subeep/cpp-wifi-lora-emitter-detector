"""Export an exploratory partial-band OFDM grid for the strongest clean prefix match."""
import argparse
import json
from pathlib import Path
import numpy as np
from tools.lightbridge_prefix_check import channel
from tools.lightbridge_full_scan import EDGES, RATE, FFT_SIZE


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    directory=parser.parse_args().directory
    candidates=[]
    for line in (directory/'prefix.jsonl').read_text().splitlines():
        row=json.loads(line)
        if row['interference']=='clean':
            for t in row['tests']:
                if t['prefix']==128 and min(t['confirmation'],t['discovery'])>.15 and t['confirmation']-t['wrong_period_confirmation']>.1:
                    candidates.append((t['confirmation'],row,t))
    _,row,test=max(candidates,key=lambda item:item[0])
    with open(row['path'],'rb') as stream:
        stream.seek(row['offset']*8)
        iq=np.fromfile(stream,'<c8',count=RATE//50)
    band=row['band']
    offset_hz=sum(EDGES[band:band+2])*RATE/(2*FFT_SIZE)
    x=channel(iq,offset_hz)[test['phase']:]
    symbols=x[:len(x)//1152*1152].reshape(-1,1152)
    prefix_product=np.vdot(symbols[:,:128],symbols[:,1024:])
    omega=np.angle(prefix_product)/1024
    corrected=symbols*np.exp(-1j*omega*np.arange(symbols.size).reshape(symbols.shape))
    bins=np.fft.fftshift(np.fft.fft(corrected[:,128:],axis=1),axes=1)
    meta=dict(source=row['path'],interval_start_sample=row['offset'],center_hz=2437.5e6+offset_hz,
              rate_hz=11.2e6,filter_cutoff_hz=3.28e6,fft_size=1024,prefix_samples=128,
              phase_samples_after_filter=test['phase'],number_of_symbols=len(symbols),
              fractional_cfo_hz=float(omega*11.2e6/(2*np.pi)),cfo_modulus_hz=11.2e6/1024,
              note='Partial-band unequalized FFT observations. Transport, pilots and payload unknown.')
    with (directory/'example_symbols.npz').open('xb') as out:
        np.savez_compressed(out,bins=bins.astype(np.complex64),metadata=json.dumps(meta))
    print(json.dumps(meta))


if __name__=='__main__':
    main()
