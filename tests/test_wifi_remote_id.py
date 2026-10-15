"""Compare the native port against pinned Core C encoder vectors and prior parser.
Fixtures are synthetic, independently encoded; this test never transmits.
"""
import sys,json,zlib,subprocess,random
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
FIX=ROOT/'tests/fixtures/remote_id'
sys.path.insert(0,str(FIX))
import reference_parser as ref
EXE=sys.argv[1]
versions=json.loads((FIX/'remote_id_versions_reference.json').read_text())['cases']
full=dict(line.split() for line in (FIX/'remote_id_full_reference.txt').read_text().splitlines())
def fcs(b): return b+zlib.crc32(b).to_bytes(4,'little')
def beacon(payload):
    body=bytes.fromhex('fa0bbc0d2a')+payload
    return bytes.fromhex(full['beacon'])[:36]+bytes([221,len(body)])+body

def expected(b):
    if len(b)<28 or int.from_bytes(b[-4:],'little')!=zlib.crc32(b[:-4]):return []
    if ref.management_frame(b,0xd0):out=ref.parse_nan_frame(b)
    elif ref.management_frame(b,0x80) and len(b)>=40:
        at=36;out=[]
        while at<len(b)-4:
            if at+2>len(b)-4:return []
            kind,size=b[at:at+2];at+=2
            if at+size>len(b)-4:return []
            body=b[at:at+size];at+=size
            if kind==221:
                p=ref.parse_vendor_body(body)
                if p:out.append(p)
    else:return []
    for p in out:p['transmitter']=':'.join(f'{v:02x}' for v in b[10:16])
    return out
cases=[]
def add(label,b,want=None):cases.append((label,b,expected(b) if want is None else want))
for version,c in versions.items():
    for name in ('nan','maximum_nan','beacon'):
        if name in c['vectors']:add(f'Core C v{version} {name}',fcs(bytes.fromhex(c['vectors'][name])))
    add(f'v{version} beacon container',fcs(beacon(bytes.fromhex(c['vectors']['pack']))))
for name in ('beacon','nan'):add('full v2 '+name,fcs(bytes.fromhex(full[name])))
for name in ('location_unavailable',):add(name,fcs(beacon(bytes.fromhex('f21901'+full[name]))))
base=bytes.fromhex(full['beacon'])
add('ordinary beacon',fcs(base[:36]),[])
for flag in (0x4000,0x8000,0x0400,0x0100,0x0200):
    bad=bytearray(base);bad[:2]=(int.from_bytes(bad[:2],'little')|flag).to_bytes(2,'little');add('unsupported MAC flags',fcs(bad),[])
bad=bytearray(base);bad[22]|=1;add('fragment number',fcs(bad),[])
add('malformed trailing IE',fcs(base+b'\xdd\xff'),[])
add('bad FCS',fcs(base)[:-1]+bytes([fcs(base)[-1]^1]),[])
for i in range(len(base)):
    add('truncated MPDU '+str(i),fcs(base[:i]))
# Mutate valid pack fields and keep transport FCS correct, so semantic rejection
# is exercised rather than always failing at the transport checksum.
rng=random.Random(994)
payload=bytes.fromhex(versions['2']['vectors']['pack'])
for i in range(250):
    p=bytearray(payload);at=rng.randrange(len(p));p[at]^=1<<rng.randrange(8)
    add('mutated pack '+str(i),fcs(beacon(p)))
for v in (3,15):
    p=bytearray(payload);p[0]=0xf0|v;add('future version',fcs(beacon(p)),[])
p=bytearray(payload);p[3]=(p[3]&0xf0)|1;add('mixed version',fcs(beacon(p)),[])
raw=bytes.fromhex(full['basic']);other=bytearray(raw);other[3]^=1
add('conflicting same-type IDs',fcs(beacon(bytes([0xf2,25,2])+raw+other)),[])
proc=subprocess.run([EXE],input='\n'.join(b.hex() for _,b,_ in cases)+'\n',text=True,capture_output=True,check=True,timeout=30)
actual=[json.loads(s) for s in proc.stdout.splitlines()]
assert len(actual)==len(cases)
for (label,b,want),got in zip(cases,actual):
    assert got==want, f'{label}:\nexpected {want}\nactual {got}'
positive=actual[0][0]
assert positive['protocol_version']==0
system=next(m for m in positive['messages'] if m['type']=='System')
assert system['timestamp_2019_s'] is None and system['operator_altitude_geo_m'] is None
assert positive['authenticated'] is False
print(f'{len(cases)} native/reference comparisons passed: Beacon/NAN v0/v1/v2, fields, sentinels, malformed and conflicting inputs')
