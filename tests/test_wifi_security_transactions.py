#!/usr/bin/env python3
"""Production CLI transaction rules with independent bytes/CRC and synthetic timing."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import zlib

TOOL=Path(sys.argv[1]); AP=bytes.fromhex('020000face01'); STA=bytes.fromhex('020000face10')

def packet(kind, seq=1, response=False, stage=0, counter=1, nonce=1, retry=False):
    raw=bytearray(24)
    if stage:
        raw[0]=8;raw[1]=2 if stage in (1,3) else 1
        response=stage in (1,3) # AP sends M1/M3
    else:raw[0]={'auth':0xb0,'assoc':0x10 if response else 0,'reassoc':0x30 if response else 0x20,'deauth':0xc0}[kind]
    if retry:raw[1]|=8
    raw[4:10]=STA if response else AP;raw[10:16]=AP if response else STA;raw[16:22]=AP
    raw[22:24]=(seq<<4).to_bytes(2,'little')
    if stage:
        key=bytearray(95);key[0]=2
        flags={1:0x008a,2:0x010a,3:0x03ca,4:0x030a}[stage]
        key[1:3]=flags.to_bytes(2,'big');key[3:5]=(16).to_bytes(2,'big')
        key[5:13]=counter.to_bytes(8,'big')
        if stage!=4:key[13:45]=bytes([nonce])*32
        raw+=bytes.fromhex('aaaa03000000888e')+bytes([2,3])+len(key).to_bytes(2,'big')+key
    elif kind=='auth':raw+=b'\0\0'+(2 if response else 1).to_bytes(2,'little')+b'\0\0'
    elif kind in ('assoc','reassoc'):
        raw+=b'\x01\0'+(b'\0\0\x01\xc0' if response else b'\x0a\0')
        if kind=='reassoc' and not response:raw+=AP
    else:raw+=b'\x07\0'
    return bytes(raw)+zlib.crc32(raw).to_bytes(4,'little')

def recording(packets):
    rows=[]
    for n,raw in enumerate(packets,1):
        c=dict(kind='capture',schema=1,run_id='transaction-test',radio_session=1,capture_seq=n,
               band='wifi_2g4',channel=6,sample_rate_hz=20000000,clock='usrp_device',
               device_time_ns=n*500000000,gain_db=20,antenna='RX2',device='fixture',
               host_before_ns=1700000000000000000+n*500000000,processed=True,
               samples_requested=8000000,samples_received=8000000,analysed_samples=8000000,events_submitted=1)
        e=dict(kind='frame',schema=1,run_id=c['run_id'],capture_seq=n,radio_session=1,
               sample_start=1000,sample_length=1000,phy='OFDM',band=c['band'],channel=c['channel'],
               sample_rate_hz=c['sample_rate_hz'],rate_mbps=6,clock='usrp_device',
               device_time_ns=c['device_time_ns']+50000,host_time_ns=c['host_before_ns']+50000,
               fcs_valid=True,mpdu=raw.hex())
        rows += [e,c]
    return rows

with tempfile.TemporaryDirectory(prefix='transaction-rule-') as directory:
    root=Path(directory); cases=0
    def check(name,rows,expected,rule=None):
        global cases
        src=root/(name+'.ndjson');src.write_text(''.join(json.dumps(r)+'\n' for r in rows))
        out=subprocess.run([str(TOOL),str(src)],capture_output=True,text=True,timeout=15)
        assert out.returncode==0,out.stderr
        result=json.loads(out.stdout)
        if len(sys.argv)>2 and name=='eapol-historical':
            export=Path(sys.argv[2]);export.mkdir(parents=True,exist_ok=True)
            (export/'synthetic-eapol.ndjson').write_text(src.read_text())
            (export/'synthetic-eapol-snapshot.json').write_text(json.dumps(result,indent=2)+'\n')
        assert result['incidents_total']==expected,(name,result['incidents'],result['transactions'])
        if expected:
            i=result['incidents'][0]
            assert i['rule']==rule and i['confidence']=='low' and i['tier']=='suspected'
            assert len(i['first_evidence'])==5
        cases+=1
        return result
    for kind in ('auth','assoc','reassoc'):
        a=packet(kind,10);b=packet(kind,11)
        positive=recording([a,packet(kind,20,True),b,packet(kind,21,True),a])
        rule='management_transaction_replay_anomaly'
        check(kind+'-historical',positive,1,rule)
        check(kind+'-ordinary-retry',recording([a,packet(kind,20,True),a]),0)
        check(kind+'-new-request',recording([a,packet(kind,20,True),b,packet(kind,21,True),packet(kind,12)]),0)
        check(kind+'-unanswered',recording([a,b,a]),0)
        check(kind+'-retry-bit',recording([a,packet(kind,20,True),b,packet(kind,21,True),packet(kind,10,retry=True)]),0)
        for variant in ('loss','session','profile','anchor','overflow','gap','count','fragment','fcs','reverse','disconnect'):
            rows=copy.deepcopy(positive)
            if variant=='loss':rows.insert(-2,dict(kind='loss',events_dropped=1,captures_dropped=0))
            if variant=='session':
                for r in rows[-2:]:r['radio_session']=2
            if variant=='profile':
                for r in rows[-2:]:r['channel']=11
            if variant=='anchor':rows[-2]['device_time_ns']+=100000
            if variant=='overflow':rows[-1]['overflows']=[{'sample_index':0}]
            if variant=='gap':
                for r in rows[-2:]:r['device_time_ns']+=10000000000
            if variant=='count':rows[-1]['events_submitted']=2
            if variant in ('fragment','fcs'):
                raw=bytearray.fromhex(rows[-2]['mpdu']);raw[22]|=1
                if variant=='fragment':raw[-4:]=zlib.crc32(raw[:-4]).to_bytes(4,'little')
                rows[-2]['mpdu']=raw.hex()
            if variant=='reverse':rows[-2]['device_time_ns']=100
            if variant=='disconnect':rows=recording([a,packet(kind,20,True),b,packet(kind,21,True),packet('deauth'),a])
            check(kind+'-'+variant,rows,0)
    m1=packet('',stage=1,counter=1);m2=packet('',stage=2,counter=1,nonce=2)
    m3=packet('',stage=3,counter=2);m4=packet('',stage=4,counter=2)
    hs=[m1,m2,m3,m4]
    check('eapol-historical',recording(hs+[m1]),1,'eapol_handshake_replay_anomaly')
    check('eapol-normal',recording(hs),0)
    check('eapol-retry-m3',recording(hs+[m3,m4]),0)
    check('eapol-retry-m1',recording([m1,m1,m2,m3,m4]),0)
    check('eapol-missing-m2',recording([m1,m3,m4,m1]),0)
    check('eapol-missing-m4',recording(hs[:-1]+[m1]),0)
    check('eapol-new-nonce',recording(hs+[packet('',stage=1,counter=1,nonce=3)]),0)
    check('eapol-wrong-response-counter',recording([m1,packet('',stage=2,counter=9),m3,m4,m1]),0)
    check('eapol-reconnect',recording(hs+[packet('assoc'),m1]),0)
    check('eapol-wrong-anonce',recording([m1,m2,packet('',stage=3,counter=2,nonce=3),m4,m1]),0)
    for variant in ('truncated','unsupported_descriptor','error','wrong_direction','bad_mic_layout'):
        raw=bytearray(m1);off=32
        if variant=='truncated':raw[off+3]+=1
        if variant=='unsupported_descriptor':raw[off+4]=254
        if variant=='error':raw[off+5]|=4
        if variant=='wrong_direction':raw[1]=1
        if variant=='bad_mic_layout':raw[-6:-4]=b'\0\x01'
        raw[-4:]=zlib.crc32(raw[:-4]).to_bytes(4,'little')
        check('eapol-'+variant,recording(hs+[bytes(raw)]),0)
    check('eapol-big-counter',recording([
        packet('',stage=1,counter=2**60),packet('',stage=2,counter=2**60,nonce=2),
        packet('',stage=3,counter=2**60+1),packet('',stage=4,counter=2**60+1),
        packet('',stage=1,counter=2**60)]),1,'eapol_handshake_replay_anomaly')
    for variant in ('loss','session','profile','anchor','gap','expiry','missing_count','duplicate_ingest','client'):
        rows=recording(hs+[m1])
        if variant=='loss':rows.insert(-2,dict(kind='loss',events_dropped=1,captures_dropped=0))
        if variant=='session':
            for r in rows[-2:]:r['radio_session']=2
        if variant=='profile':rows[-1]['gain_db']=30
        if variant=='anchor':rows[-2]['device_time_ns']+=50
        if variant in ('gap','expiry'):
            for r in rows[-2:]:r['device_time_ns']+=40000000000
        if variant=='missing_count':rows[-1]['events_submitted']=2
        if variant=='duplicate_ingest':rows.insert(-1,dict(rows[-2]))
        if variant=='client':
            raw=bytearray.fromhex(rows[-2]['mpdu']);raw[9]^=2
            raw[-4:]=zlib.crc32(raw[:-4]).to_bytes(4,'little');rows[-2]['mpdu']=raw.hex()
        result=check('eapol-'+variant,rows,1 if variant=='duplicate_ingest' else 0,
                     'eapol_handshake_replay_anomaly')
        if variant=='duplicate_ingest':assert result['incidents'][0]['observations']==1
    # Too many eligible frames invalidate the whole capture before any alert.
    rows=recording(hs+[m1]);last=rows[-2];c=rows[-1]
    extra=[]
    for n in range(257):
        e=dict(last);e['sample_start']=1000+n*1000;e['device_time_ns']=c['device_time_ns']+e['sample_start']*50
        extra.append(e)
    c['events_submitted']=257
    check('capacity-no-partial-alert',rows[:-2]+extra+[c],0)
    print(f'PASS {cases} transaction replay cases; no radio; production default policy')
