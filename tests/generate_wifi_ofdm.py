"""Software OFDM vectors. This is a local transmitter, not an external oracle.
Requires NumPy/SciPy. Generates only files; no SDR or transmitter hardware access.
Usage: python3 tests/generate_wifi_ofdm.py OUTPUT_DIRECTORY
"""
import numpy as np, pathlib, json, zlib, sys
from scipy.signal import resample_poly
out=pathlib.Path(sys.argv[1]);out.mkdir(parents=True, exist_ok=True)
# Standard training sequence in natural subcarrier order, independent Python transmitter.
L=np.array([1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,0,1,-1,-1,1,1,-1,1,-1,1,-1,-1,-1,-1,-1,1,1,-1,-1,1,-1,1,-1,1,1,1,1])
bins=np.arange(-26,27)%64
f=np.zeros(64,complex);f[bins]=L;ltf=np.fft.ifft(f)*8
# STF specified nonzero tones (-24,-20,...,24).
st=np.zeros(64,complex);st[np.array([-24,-20,-16,-12,-8,-4,4,8,12,16,20,24])%64]=np.sqrt(13/6)*np.array([1,-1,1,-1,-1,1,-1,-1,1,1,1,1])*(1+1j)
stf=np.tile((np.fft.ifft(st)*8)[:16],10)
pilots=[-21,-7,7,21];data=[k for k in range(-26,27) if k and k not in pilots]
pol=[];state=[1]*7
for _ in range(127):
 b=state[6]^state[3];pol.append(1-2*b);state=[b]+state[:6]
def conv(bits):
 memory=[0]*6;out=[]
 for bit in bits:
  r=[int(bit)]+memory
  out.extend([r[0]^r[2]^r[3]^r[5]^r[6],r[0]^r[1]^r[2]^r[3]^r[6]])
  memory=r[:6]
 return np.array(out)
def ofdm(bits,bpsc,symbol):
 n=len(bits);order=[];step=max(1,bpsc//2)
 for k in range(n):
  i=(n//16)*(k%16)+k//16;order.append(step*(i//step)+(i+n-(16*i)//n)%step)
 inter=np.zeros(n,int);inter[order]=bits
 def axis(b):
  if len(b)==1: amp=1
  elif len(b)==2: amp=3-2*b[1]
  else: amp=4-(1-2*b[1])*(2-(1-2*b[2])) # check against standard gray levels below
  return (2*b[0]-1)*amp
 def ax(b):
  if len(b)<3:return axis(b)
  return (2*b[0]-1)*{(0,0):7,(1,0):1,(0,1):5,(1,1):3}[tuple(b[1:])]
 scale=np.sqrt({1:1,2:2,4:10,6:42}[bpsc]);tones=np.zeros(64,complex)
 for k,b in zip(data,inter.reshape(-1,bpsc)):
  tones[k%64]=(ax(b) if bpsc==1 else ax(b[:bpsc//2])+1j*ax(b[bpsc//2:]))/scale
 tones[np.array(pilots)%64]=np.array([1,1,1,-1])*pol[symbol%127]
 t=np.fft.ifft(tones)*8;return np.r_[t[-16:],t]
def make(rate,code,bpsc,dbps, name=None, bad_fcs=False, bad_sig=False, sample_rate=20e6, offset=0, multipath=False, truncate=False, long=False):
 mac=bytes.fromhex('80000000ffffffffffff0011223344550011223344551000')+bytes(8)+bytes.fromhex('64001100')+b'\x00\x0aOFDM-fixtu'+b'\x03\x01\x06'
 if long: mac += (b'\xdd\xfa' + bytes(range(250))) * 8
 mac+=zlib.crc32(mac).to_bytes(4,'little')
 if bad_fcs: mac=mac[:-1]+bytes([mac[-1]^128])
 bits=np.unpackbits(np.frombuffer(mac,dtype=np.uint8),bitorder='little')
 h=[(code>>i)&1 for i in range(4)]+[0]+[(len(mac)>>i)&1 for i in range(12)];h+=[sum(h)%2]+[0]*6
 if bad_sig: h[17]^=1
 nsyms=(16+len(bits)+6+dbps-1)//dbps;plain=np.r_[np.zeros(16,int),bits,np.zeros(nsyms*dbps-16-len(bits),int)]
 state=[1,0,1,1,1,0,1];scrambled=[]
 for bit in plain:
  fb=state[6]^state[3];scrambled.append(int(bit)^fb);state=[fb]+state[:6]
 scrambled[16+len(bits):16+len(bits)+6]=[0]*6
 encoded=conv(scrambled);mask=([1,1] if dbps*2==48*bpsc else [1,1,1,0] if rate==48 else [1,1,1,0,0,1]);encoded=encoded[np.resize(mask,len(encoded)).astype(bool)]
 wave=np.concatenate([stf,ltf[-32:],ltf,ltf,ofdm(conv(h),1,0)]+[ofdm(encoded[i*48*bpsc:(i+1)*48*bpsc],bpsc,i+1) for i in range(nsyms)])
 if multipath: wave=np.convolve(wave, np.r_[1.,0,0,.18+.12j,0,0,-.07j])
 # Keep only 4 us lead-in; match the decoder's burst API.
 wave=np.r_[np.zeros(80), wave, np.zeros(80)]
 if sample_rate != 20e6: wave=resample_poly(wave,14,5)
 rng=np.random.default_rng(rate)
 wave=wave*np.exp(2j*np.pi*(42000-offset)*np.arange(len(wave))/sample_rate)+.004*(rng.normal(size=len(wave))+1j*rng.normal(size=len(wave)))
 if truncate: wave=wave[:int(len(wave)*.7)]
 raw=wave.astype('<c8').tobytes();fnv=14695981039346656037
 for c in raw:fnv=((fnv^c)*1099511628211)&((1<<64)-1)
 stem=name or str(rate);(out/(stem+'.cf32')).write_bytes(raw)
 (out/(stem+'.json')).write_text(json.dumps(dict(schema=1,format='cf32_le',samples=len(wave),sample_rate_hz=sample_rate,channel_hz=2437e6,capture_center_hz=2437e6+offset,overflow=False,iq_file=stem+'.cf32',fnv1a64=str(fnv),expected_rate_mbps=rate,expected_mpdu_hex=mac.hex(),expected_fcs=not(bad_fcs or bad_sig or truncate))))

for args in [(6,11,1,24),(9,15,1,36),(12,10,2,48),(18,14,2,72),(24,9,4,96),(36,13,4,144),(48,8,6,192),(54,12,6,216)]:make(*args)

make(6,11,1,24,name="bad-fcs",bad_fcs=True)
make(6,11,1,24,name="bad-signal",bad_sig=True)
make(6,11,1,24,name="truncated",truncate=True)
make(6,11,1,24,name="offset-56m",sample_rate=56e6,offset=1.5e6,multipath=True)
make(54,12,6,216,name="multipath-54",multipath=True)
make(6,11,1,24,name="long-pilot-wrap",long=True)
