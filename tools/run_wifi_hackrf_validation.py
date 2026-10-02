#!/usr/bin/env python3
"""Finite conducted-only lab validation. Requires confirmed >=30 dB cable path
from HackRF to X310 RF A RX2, no antennas. Fixed synthetic identities/vectors.
Verify benign on-air preflight before invoking; does not test client impact.
"""
import argparse,datetime,hashlib,json,pathlib,signal,subprocess,time
ROOT=pathlib.Path(__file__).resolve().parents[1]
RATE=20_000_000
PHASES=[('baseline',40,None),
        ('historical_replay',4,'historical_beacon_after_newer'),
        ('target_distinct',6,'distinct_management_frames'),
        ('target_repeated',6,'repeated_management_frames'),
        ('ap_distinct',6,'ap_wide_distinct_management_frames'),
        ('ap_repeated',6,'ap_wide_repeated_management_frames'),('retry_negative',6,None)]
def utc():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def stop(p):
    if p is not None and p.poll() is None:
        p.send_signal(signal.SIGINT)
        try:p.wait(timeout=3)
        except subprocess.TimeoutExpired:p.kill();p.wait()
def complete_rows(path):
    rows=[];end=0
    for line in path.read_text().splitlines():
        try:j=json.loads(line)
        except json.JSONDecodeError:break
        rows.append(j)
        if j.get('kind')=='capture':end=len(rows)
    return rows[:end]
def run(args):
    v=args.vectors.resolve();run=args.new_directory.resolve()
    ref=json.loads((v/'reference.json').read_text())
    if args.attenuation_db<30 or ref.get('bssid')!='02:00:00:fa:ce:01' or not ref.get('laboratory_only'):
        raise ValueError('Requires confirmed >=30 dB conducted path and fixed lab vectors')
    phases={p['name']:p for p in ref['phases']}
    for name,_,_ in PHASES:
        p=phases[name];raw=(v/p['file']).read_bytes()
        if len(raw)!=p['samples']*2 or hashlib.sha256(raw).hexdigest()!=p['sha256']:
            raise ValueError('Waveform checksum/size mismatch')
    run.mkdir(parents=True,exist_ok=False);rx=run/'receiver'
    stop_file=run/'stop-receiver'
    command=[str(ROOT/'build/wifi_scan_smoke'),'2g4','11','200',str(rx),'--gain','20','--stop-file',str(stop_file)]
    result={'start_utc':utc(),'attenuation_db':args.attenuation_db,'rx_port':'RF A RX2','antennas':False,
            'tx_gain_db':20,'rf_amp':False,'bias_tee':False,'rx_gain_db':20,'rx_command':command,
            'vectors':str(v),'phases':[],'success':False}
    receiver=None;transmitter=None
    def save(): (run/'execution.json').write_text(json.dumps(result,indent=2)+'\n')
    try:
        with (run/'receiver.log').open('w') as log:
            receiver=subprocess.Popen(command,cwd=run,stdout=log,stderr=subprocess.STDOUT)
            events=rx/'events.ndjson';deadline=time.monotonic()+20
            while True:
                rows=complete_rows(events) if events.exists() else []
                captures=[row for row in rows if row.get('kind')=='capture']
                clean=[c for c in captures if c['processed'] and not (c['overflows'] or c['timed_out'] or c['exception'] or c['burst_cap_reached'])]
                if clean and captures[-1] in clean:
                    ready_capture=clean[-1]['capture_seq'];break
                if receiver.poll() is not None or time.monotonic()>deadline:
                    raise RuntimeError('Receiver did not become ready')
                time.sleep(.1)
            result['startup_excluded']=[c['capture_seq'] for c in captures if c not in clean]
            result['ready_capture']=ready_capture
            for name,seconds,expected in PHASES:
                if receiver.poll() is not None:raise RuntimeError('Receiver stopped early')
                if name=='retry_negative':time.sleep(3) # expire prior flood windows before negative control
                before_incidents=0 if name=='baseline' else snap['incidents_total']
                before_observations=0 if name=='baseline' else sum(i['observations'] for i in snap['incidents'])
                tx=['/usr/bin/hackrf_transfer','-d',args.hackrf_serial,'-t',str(v/phases[name]['file']),
                    '-s',str(RATE),'-f','2462000000','-x','20','-a','0','-p','0','-b','20000000',
                    '-R','-n',str(RATE*seconds)]
                phase={'name':name,'seconds_nominal':seconds,'tx_command':tx,'start_utc':utc()}
                result['phases'].append(phase);save();print('Starting '+name,flush=True)
                with (run/(name+'-tx.log')).open('w') as txlog:
                    transmitter=subprocess.Popen(tx,stdout=txlog,stderr=subprocess.STDOUT)
                    try:phase['tx_exit']=transmitter.wait(timeout=seconds+8)
                    finally:stop(transmitter)
                phase['end_utc']=utc()
                if phase['tx_exit']:raise RuntimeError('Transmitter failed')
                # Scanner lanes can publish several seconds behind wall time.
                # Wait for completed received coverage, not a fixed sleep.
                required_end_ns=round(datetime.datetime.fromisoformat(phase['end_utc']).timestamp()*1e9)
                deadline=time.monotonic()+15
                while True:
                    rows=complete_rows(events)
                    captures=[row for row in rows if row.get('kind')=='capture']
                    last=captures[-1] if captures else None
                    if last and last['host_before_ns']+round(last['samples_received']/last['sample_rate_hz']*1e9)>=required_end_ns:break
                    if receiver.poll() is not None or time.monotonic()>deadline:raise RuntimeError('Incomplete phase coverage')
                    time.sleep(.2)
                phase['last_complete_capture']=last['capture_seq']
                prefix=run/(name+'-events.ndjson')
                prefix.write_text(''.join(json.dumps(row)+'\n' for row in rows))
                p=subprocess.run([str(ROOT/'build/wifi_security_replay'),'--recent',str(prefix)],
                                 capture_output=True,text=True,check=True,timeout=10)
                snap=json.loads(p.stdout);(run/(name+'-snapshot.json')).write_text(p.stdout)
                paths={i['path'] for i in snap['incidents']}
                paths.update(pt.get('path','') for i in snap['incidents'] for pt in i['timeline'])
                phase.update(accepted=snap['frames_accepted'],incidents=snap['incidents_total'],paths=sorted(paths))
                if name=='baseline':
                    phase['baseline_mature']=any(b['learned_analysed_s']>=30 and b['windows_included']>=3 for b in snap['baselines'])
                    if not phase['baseline_mature']:raise RuntimeError('Baseline did not mature')
                if expected is None and (snap['incidents_total']!=before_incidents or sum(i['observations'] for i in snap['incidents'])!=before_observations):
                    raise RuntimeError('Negative control produced a new or renewed incident')
                if expected is not None and expected not in paths:raise RuntimeError('Missing expected path: '+expected)
                if snap['queue_events_dropped'] or snap['queue_captures_dropped'] or snap['input_lines_rejected']:
                    raise RuntimeError('Input loss invalidates campaign')
                degraded=[c['capture_seq'] for c in rows if c.get('kind')=='capture' and c['capture_seq']>ready_capture and
                          (c['overflows'] or c['timed_out'] or c['exception'] or c['burst_cap_reached'] or c['events_rejected_by_queue'])]
                if degraded:raise RuntimeError('Degraded test-period captures: '+str(degraded))
                phase['passed']=True;save();print('Passed '+name+'; incidents='+str(snap['incidents_total']),flush=True)
            stop_file.touch();result['rx_exit']=receiver.wait(timeout=15)
            if result['rx_exit']:raise RuntimeError('Receiver tool failed')
            native=json.loads((rx/'snapshot.json').read_text())
            p=subprocess.run([str(ROOT/'build/wifi_security_replay'),'--recent',str(events)],capture_output=True,text=True,check=True)
            (run/'offline-final.json').write_text(p.stdout);offline=json.loads(p.stdout)
            differences=[key for key in native if key not in ('recent','recorded_bytes') and native[key]!=offline.get(key)]
            same_tail=native['recent']==offline['recent'][-len(native['recent']):] if native['recent'] else not offline['recent']
            result.update(live_offline_difference_keys=differences,recent_tail_equal=same_tail,
                          final_incidents=native['incidents_total'],final_frames=native['frames_accepted'])
            if differences or not same_tail:raise RuntimeError('Live/offline mismatch')
            result['success']=True
    except Exception as e:
        result['error']=str(e);raise
    finally:
        stop(transmitter)
        if receiver is not None and receiver.poll() is None:
            stop_file.touch(exist_ok=True)
            try:receiver.wait(timeout=15)
            except subprocess.TimeoutExpired:stop(receiver)
        result['end_utc']=utc();save()
    print(json.dumps(result),flush=True)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('vectors',type=pathlib.Path);p.add_argument('new_directory',type=pathlib.Path)
    p.add_argument('--hackrf-serial',required=True);p.add_argument('--attenuation-db',type=int,required=True)
    run(p.parse_args())
