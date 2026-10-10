#!/usr/bin/env python3
"""Acquire one bounded, CRC-verified member from a public KU Leuven IQ ZIP.

Never executes publisher code. The outer archive MD5 is not verified by range
acquisition; the exact member and HTTP ranges are recorded instead.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import urllib.request
import zipfile

class RemoteZip(io.RawIOBase):
    def __init__(self,url,size):
        self.url,self.size,self.pos=url,size,0
        self.requests=[];self.total=0
    def seekable(self):return True
    def readable(self):return True
    def tell(self):return self.pos
    def seek(self,offset,whence=0):
        p=offset+(self.pos if whence==1 else self.size if whence==2 else 0)
        if p<0 or p>self.size:raise ValueError('seek outside archive')
        self.pos=p;return p
    def read(self,n=-1):
        n=min(self.size-self.pos,n if n>=0 else self.size-self.pos)
        if n==0:return b''
        if n>256*1024*1024 or self.total+n>300*1024*1024:raise ValueError('download budget exceeded')
        end=self.pos+n-1
        req=urllib.request.Request(self.url,headers={'Range':f'bytes={self.pos}-{end}'})
        with urllib.request.urlopen(req,timeout=60) as r:
            if r.status!=206 or r.headers.get('Content-Range')!=f'bytes {self.pos}-{end}/{self.size}':
                raise ValueError('server did not honor exact byte range')
            raw=r.read(n+1)
        if len(raw)!=n:raise ValueError('short/oversized range')
        self.requests.append({'first':self.pos,'bytes':n,'sha256':hashlib.sha256(raw).hexdigest()})
        self.total+=n;self.pos+=n;return raw

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--metadata',type=Path,required=True)
    p.add_argument('--file-id',type=int,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--member')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    allfiles=json.loads(a.metadata.read_text())['data']['latestVersion']['files']
    f=next(x['dataFile'] for x in allfiles if x['dataFile']['id']==a.file_id)
    url=f'https://rdr.kuleuven.be/api/access/datafile/{a.file_id}'
    remote=RemoteZip(url,f['filesize'])
    with zipfile.ZipFile(remote) as z:
        members=[{'name':x.filename,'compressed':x.compress_size,'bytes':x.file_size,'crc32':f'{x.CRC:08x}'} for x in z.infolist()]
        (a.output/'archive_members.json').write_text(json.dumps(members,indent=2)+'\n')
        if not a.member:
            print(json.dumps({'members':len(members),'first':members[:8]},indent=2));return
        info=z.getinfo(a.member)
        if info.file_size>256*1024*1024 or info.compress_size>256*1024*1024:raise ValueError('member budget exceeded')
        target=a.output/'recording.mat'
        if target.exists():raise ValueError('refusing to overwrite acquired data')
        # A fixed local basename prevents archive paths from escaping output.
        raw=z.read(info) # zipfile verifies member CRC during the full read.
        if len(raw)!=info.file_size:raise ValueError('member length mismatch')
        target.write_bytes(raw)
    manifest={'source':'doi:10.48804/HZRVNZ','license':'CC-BY-NC-4.0','archive':f,'member':a.member,
        'member_crc32_verified':True,'archive_md5_verified':False,'member_sha256':hashlib.sha256(raw).hexdigest(),
        'path':str(target.resolve()),'bytes':len(raw),'requests':remote.requests,'downloaded_bytes':remote.total,
        'sample_rate_hz':100000000,'capture_center_hz':2440000000,
        'partition':'development_only','split_group':'kuleuven:'+f['filename'],
        'link_family_truth':None,'label_status':'publisher_device_role_label_not_packet_link_truth'}
    (a.output/'acquisition.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({k:v for k,v in manifest.items() if k not in ('requests','archive')},indent=2))

if __name__=='__main__':main()
