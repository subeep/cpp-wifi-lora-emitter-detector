#!/usr/bin/env python3
"""Software-render the actual ImGui test draw list/font atlas; no GPU/radio."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image


def render(path,output):
    data=json.loads(path.read_text());width,height=int(data['width']),int(data['height'])
    atlas=np.frombuffer(Path(str(path)+'.rgba').read_bytes(),np.uint8).reshape(data['atlas_height'],data['atlas_width'],4).astype(float)/255
    canvas=np.zeros((height,width,4),float);canvas[:]=[.06,.06,.065,1]
    for lst in data['lists']:
        vertices=np.array(lst['vertices']);indices=np.array(lst['indices'])
        colors=vertices[:,4].astype(np.uint32)
        rgba=np.stack([colors&255,(colors>>8)&255,(colors>>16)&255,colors>>24],axis=1)/255
        for command in lst['commands']:
            clip=command['clip'];first=command['first'];count=command['count'];offset=command['vertex_offset']
            for tri in indices[first:first+count].reshape(-1,3)+offset:
                v=vertices[tri];c=rgba[tri]
                x0=max(0,int(np.floor(max(clip[0],v[:,0].min()))));x1=min(width,int(np.ceil(min(clip[2],v[:,0].max()))))
                y0=max(0,int(np.floor(max(clip[1],v[:,1].min()))));y1=min(height,int(np.ceil(min(clip[3],v[:,1].max()))))
                if x0>=x1 or y0>=y1:continue
                x,y=np.meshgrid(np.arange(x0,x1)+.5,np.arange(y0,y1)+.5)
                a,b,d=v[:,:2];den=(b[1]-d[1])*(a[0]-d[0])+(d[0]-b[0])*(a[1]-d[1])
                if abs(den)<1e-10:continue
                wa=((b[1]-d[1])*(x-d[0])+(d[0]-b[0])*(y-d[1]))/den
                wb=((d[1]-a[1])*(x-d[0])+(a[0]-d[0])*(y-d[1]))/den;wc=1-wa-wb
                mask=(wa>=-1e-9)&(wb>=-1e-9)&(wc>=-1e-9)
                if not mask.any():continue
                uv=wa[...,None]*v[0,2:4]+wb[...,None]*v[1,2:4]+wc[...,None]*v[2,2:4]
                tx=np.clip(uv[...,0]*data['atlas_width']-.5,0,data['atlas_width']-1)
                ty=np.clip(uv[...,1]*data['atlas_height']-.5,0,data['atlas_height']-1)
                ix=tx.astype(int);iy=ty.astype(int);fx=(tx-ix)[...,None];fy=(ty-iy)[...,None]
                jx=np.minimum(ix+1,data['atlas_width']-1);jy=np.minimum(iy+1,data['atlas_height']-1)
                texture=(atlas[iy,ix]*(1-fx)+atlas[iy,jx]*fx)*(1-fy)+(atlas[jy,ix]*(1-fx)+atlas[jy,jx]*fx)*fy
                tint=wa[...,None]*c[0]+wb[...,None]*c[1]+wc[...,None]*c[2];source=texture*tint
                alpha=source[...,3:4]*mask[...,None]
                dest=canvas[y0:y1,x0:x1];dest[:,:,:3]=source[:,:,:3]*alpha+dest[:,:,:3]*(1-alpha)
    Image.fromarray(np.round(np.clip(canvas,0,1)*255).astype(np.uint8)).save(output)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('draw',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();render(args.draw,args.output)
