#!/usr/bin/env python3
"""Synthetic robustness, conditional on a known marker position; no accuracy claim."""
import argparse
import itertools
import json
from pathlib import Path
import time
import numpy as np
from marker_prefix import assess_marker_structure


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    rng=np.random.default_rng(41299);results=[]
    for rate,slope,cfo,snr,echo,ppm in itertools.product([20e6,100e6,200e6],[135.2e9,270.2e9,-135.3e9],
            [0,500e3],[10,20],[0,.3],[-50,0,50]):
        start=round(rate*20e-6);n=round(rate*120e-6);scale=1+ppm*1e-6
        t=(np.arange(n)-start)*scale/rate;period=1/15000
        active=(t>=-4e-6)&(t<period);u=np.where(t<0,t+period,t)-period/2
        x=np.where(active,np.exp(1j*np.pi*slope*u*u),0j)
        delay=round(rate*.3e-6)
        if echo:x[delay:]+=echo*np.exp(.7j)*x[:-delay].copy()
        x*=np.exp(2j*np.pi*cfo*np.arange(n)/rate)
        x+=10**(-snr/20)/np.sqrt(2)*(rng.normal(size=n)+1j*rng.normal(size=n))
        peak=start+round(rate*1e-6);legacy=2*round(rate*32e-6)
        mean=cfo+slope*scale*((peak-start+(legacy-1)/2)*scale/rate-period/2)
        before=time.perf_counter();r=assess_marker_structure(x,rate,peak,slope,mean)
        results.append(dict(rate=rate,slope=slope,cfo=cfo,snr_db=snr,echo_amplitude=echo,clock_ppm=ppm,
            prefix_compatible=r['prefix_compatible'],joint_span_and_prefix=r.get('joint_span_and_prefix',False),
            elapsed_ms=(time.perf_counter()-before)*1000))
    report=dict(schema='marker_structure_stress.v1',selection='oracle_marker_position_and_carrier_not_full_search',
        seed=41299,cases=len(results),joint_passes=sum(r['joint_span_and_prefix'] for r in results),
        receiver_performance_qualified=False,accuracy_estimated=False,results=results)
    (a.output/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(report['cases'],report['joint_passes'])

if __name__=='__main__':main()
