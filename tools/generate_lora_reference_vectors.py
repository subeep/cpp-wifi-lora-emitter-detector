#!/usr/bin/env python3
"""Generate software conformance vectors from LoRaPHY conventions (MIT).
Reference commit 4fddd9a7b47682781c663608bfc4f196e4bf656d.
See third_party/loraphy_reference/LICENSE. Run from repository root.
These are supplemental tests, not independent hardware interoperability proof.
"""
import json
from pathlib import Path
# Reference-derived transmit vector generator; no application decoder imports.
def encode(payload,sf,cr,ldro,crc_on=True):
 crc=0
 for b in payload[:-2]:
  crc^=b<<8
  for _ in range(8): crc=((crc<<1)^(0x1021 if crc&0x8000 else 0))&65535
 if payload: crc^=payload[-1]
 if len(payload)>1: crc^=payload[-2]<<8
 state=255; b=[]
 for x in payload:
  b.append(x^state); state=((state<<1)|(((state>>7)^(state>>5)^(state>>4)^(state>>3))&1))&255
 if crc_on:b += [crc&255,crc>>8]
 h=[len(payload)>>4,len(payload)&15,2*cr+int(crc_on)]
 rows=['111100000000','100011100001','010010011010','001001010111','000100101111']
 bits=f'{h[0]:04b}{h[1]:04b}{h[2]:04b}'
 check=[sum(int(x)*int(y) for x,y in zip(r,bits))%2 for r in rows]
 h += [check[0],sum(v<<(3-i) for i,v in enumerate(check[1:]))]
 nib=h+[x for v in b for x in [v&15,v>>4]]
 w=sf-2*ldro
 while len(nib)<sf-2 or (len(nib)-(sf-2))%w:nib.append(0)
 cw=[]
 for i,x in enumerate(nib):
  bits=[(x>>j)&1 for j in range(4)]; a,b,c,d=bits; rate=4 if i<sf-2 else cr
  ps=[a^b^c^d] if rate==1 else [a^b^c,b^c^d,a^b^d,a^c^d][:rate]
  cw.append(x+sum(v<<(4+j) for j,v in enumerate(ps)))
 syms=[]; start=0
 while start<len(cw):
  width=sf-2 if start==0 else w; redundancy=8 if start==0 else cr+4
  for k in range(redundancy):
   g=sum(((cw[start+(m+k)%width]>>k)&1)<<m for m in range(width)); v=g; mask=g>>1
   while mask:v^=mask; mask>>=1
   syms.append((v*(4 if start==0 or ldro else 1)+1)%(1<<sf))
  start+=width
 return syms
published=[2541,1153,673,2397,1189,3509,41,3089,3237,3917,2729,2765,1417,2833,1389,801,3197,345,961,745,3101,297,1893,469]
# Padding bytes differ across encoder implementations. Published vector remains
# the independent oracle; locally generated vectors exercise the parameter grid.
out=[dict(sf=12,cr=4,ldro=True,crc=True,payload=list(range(1,10)),symbols=published,source='LoRaPHY published example')]
for sf in range(7,13):
 for cr in range(1,5):
  for ldro in [False,True]:
   for crc in [False,True]:
    payload=[0,255,81,16,173,0,127,128,2]
    out.append(dict(sf=sf,cr=cr,ldro=ldro,crc=crc,payload=payload,symbols=encode(payload,sf,cr,ldro,crc),source='reference-derived Python vector'))
for length in [0,1,2,255]:
 payload=[i%256 for i in range(length)]
 out.append(dict(sf=7,cr=4,ldro=False,crc=True,payload=payload,symbols=encode(payload,7,4,False),source='reference-derived Python length boundary'))
Path('tests/fixtures/lora_standard_symbols.json').write_text(json.dumps(out,indent=2)+'\n')
