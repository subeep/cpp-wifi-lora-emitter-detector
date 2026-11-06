"""Recheck the fixed 1024+128 sample hypothesis away from discovery windows."""
import argparse
import json
from pathlib import Path

import numpy as np
from tools.lightbridge_full_scan import EDGES, FFT_SIZE, RATE
from tools.lightbridge_prefix_check import channel, contrast


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    directory=p.parse_args().directory
    with (directory/'recheck.jsonl').open('x') as out:
        for name in ['prefix','control-prefix']:
            for line in (directory/(name+'.jsonl')).read_text().splitlines():
                row=json.loads(line)
                # Fixed distance from start, with a second fixed choice if necessary.
                count=RATE//50
                start=RATE//4
                if abs(start-row['offset'])<count:
                    start=3*RATE//4
                with open(row['path'],'rb') as f:
                    f.seek(start*8)
                    iq=np.fromfile(f,'<c8',count=count)
                if len(iq)!=count:
                    raise ValueError('Short second window')
                b=row['band']
                x=channel(iq,sum(EDGES[b:b+2])*RATE/(2*FFT_SIZE))
                phase,first,second=contrast(x,1024,128,1152)
                wrong=contrast(x,1024,128,1159)[2]
                print(json.dumps(dict(path=row['path'],band=b,offset=start,phase=phase,
                    first=first,second=second,wrong=wrong,group=name)),file=out)


if __name__=='__main__':
    main()
