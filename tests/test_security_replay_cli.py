#!/usr/bin/env python3
"""End-to-end recorded IQ -> production decoder -> timed events -> replay.
Anchors added below are synthetic test metadata, NEVER measured RF timing.
"""
import json
from pathlib import Path
import shutil
import random
import struct
import subprocess
import sys
import tempfile
import zlib

root, build = map(Path, sys.argv[1:3])

def run(tool, *args, ok=True):
    p = subprocess.run([str(build / tool), *map(str, args)], capture_output=True, text=True)
    if ok and p.returncode:
        raise AssertionError(f'{tool} failed: {p.stderr}\n{p.stdout}')
    if not ok:
        assert p.returncode, f'{tool} accepted corrupt evidence'
        return p
    return json.loads(p.stdout)

def timing(n, device_ns=5_000_000_000):
    return dict(device_time_valid=True, device_time_ns=device_ns,
                host_before_ns=1_700_000_000_000_000_000,
                host_after_ns=1_700_000_000_000_200_000,
                requested_samples=n, overflows=[], timed_out=False, exception=False,
                retuned=True, actual_rf_hz=0, actual_dsp_hz=0, gain_db=20)

with tempfile.TemporaryDirectory(prefix='rfmon-security-cli-') as name:
    tmp = Path(name)
    # Independent byte/CRC fixtures exercise production default rules through
    # the actual CLI. These are synthetic records, not positive RF captures.
    def flood_campaign(targets=1, retry=False, phase=0):
        rows = []
        for seq in range(1, 33 + phase):
            start = seq * 1_100_000_000
            count = 12 if seq >= 31 + phase else 0
            for n in range(count):
                raw = bytearray.fromhex('c0003a01ffffffffffff00112233445500112233445530120700')
                raw[1] = 8 if retry else 0
                raw[22:24] = (7 << 4).to_bytes(2, 'little')
                if targets > 1:
                    raw[4] = 2 * (n % targets + 1)
                mpdu = raw + zlib.crc32(raw).to_bytes(4, 'little')
                offset = 1000 + n * 500_000
                rows.append(dict(kind='frame', schema=1, run_id='synthetic-flood-cli', capture_seq=seq,
                                 radio_session=1, sample_start=offset, sample_length=400, phy='OFDM',
                                 band='wifi_2g4', channel=6, sample_rate_hz=20_000_000, rate_mbps=6,
                                 clock='usrp_device', device_time_ns=start + offset * 50,
                                 host_time_ns=1_700_000_000_000_000_000 + start + offset * 50,
                                 fcs_valid=True, mpdu=mpdu.hex()))
            rows.append(dict(kind='capture', schema=1, run_id='synthetic-flood-cli', capture_seq=seq,
                             radio_session=1, band='wifi_2g4', channel=6, sample_rate_hz=20_000_000,
                             clock='usrp_device', device_time_ns=start, gain_db=20, antenna='RX2', device='test',
                             host_before_ns=1_700_000_000_000_000_000 + start, processed=True,
                             samples_requested=20_000_000, samples_received=20_000_000,
                             analysed_samples=20_000_000, events_submitted=count))
        return rows

    def save_rows(path, rows):
        path.write_text(''.join(json.dumps(row) + '\n' for row in rows))

    for targets, path in [(1, 'repeated_management_frames'), (4, 'ap_wide_repeated_management_frames')]:
        for phase in (0, 1):
            campaign = tmp / f'flood-{targets}-{phase}.ndjson'
            save_rows(campaign, flood_campaign(targets=targets, phase=phase))
            reexport = tmp / f'flood-export-{targets}-{phase}.ndjson'
            classified = run('wifi_security_replay', '--record', reexport, campaign)
            assert classified['incidents_total'] == 1
            incident = classified['incidents'][0]
            assert incident['path'] == path and incident['numerator'] == 24
            assert incident['rule_version'] == '2'
            assert classified == run('wifi_security_replay', reexport)
    print('PASS Default repeated/AP-wide rules, boundary phase and CLI re-export equivalence')
    retry_log = tmp / 'retry-flood.ndjson'
    save_rows(retry_log, flood_campaign(retry=True))
    assert run('wifi_security_replay', retry_log)['incidents_total'] == 0
    gap_rows = []
    for row in flood_campaign(targets=4):
        gap_rows.append(row)
        if row['kind'] == 'capture' and row['capture_seq'] == 31:
            gap_rows.append(dict(kind='loss', events_dropped=1, captures_dropped=0))
    gap_log = tmp / 'flood-gap.ndjson'
    save_rows(gap_log, gap_rows)
    assert run('wifi_security_replay', gap_log)['incidents_total'] == 0
    print('PASS Default retry and interrupted-flood CLI negative controls')
    src = root / 'tests/fixtures/wifi_ofdm/airtel-2g4.json'
    old = json.loads(src.read_text())
    # Cropped packet fixtures lack the quiet context needed by burst detection.
    # Supply deterministic low-level noise around the unmodified real IQ crop.
    rng = random.Random(42)
    noise = b''.join(struct.pack('<ff', rng.gauss(0, 1e-5), rng.gauss(0, 1e-5)) for _ in range(20_000))
    padded = noise + (src.parent / old['iq_file']).read_bytes() + noise
    checksum = 14695981039346656037
    for byte in padded:
        checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
    old.update(samples=len(padded)//8, fnv1a64=str(checksum))
    (tmp / old['iq_file']).write_bytes(padded)
    meta = dict(old, schema=2, run_id='synthetic-test-anchor', radio_session=1,
                capture_seq=1, timing=timing(old['samples']))
    manifest = tmp / 'wifi.json'
    manifest.write_text(json.dumps(meta))
    record, frames = tmp / 'wifi.ndjson', tmp / 'wifi-frames.ndjson'
    result = run('wifi_security_replay', '--recent', '--record', record, '--frames', frames, manifest)
    assert result['frames_accepted'] > 0
    assert result['frames_without_device_time'] == 0
    assert result == run('wifi_security_replay', '--recent', record), 'IQ/event state differs'
    events = [json.loads(s) for s in frames.read_text().splitlines()]
    assert events and all(e['clock'] == 'usrp_device' for e in events)
    print('PASS Wi-Fi IQ -> hardware-domain test timestamps -> event replay equivalence')
    legacy_manifest = tmp / 'legacy.json'
    legacy_manifest.write_text(json.dumps(old))
    legacy = run('wifi_security_replay', legacy_manifest)
    assert legacy['frames_accepted'] == result['frames_accepted']
    assert legacy['frames_without_device_time'] == legacy['frames_accepted']
    print('PASS Legacy IQ retains decoder yield without claimed device timestamps')
    # Re-export preserves a lost-input boundary rather than repairing history.
    damaged = tmp / 'damaged.ndjson'
    lines = record.read_text().splitlines()
    lines.insert(1, '{not-json')
    damaged.write_text('\n'.join(lines) + '\n')
    exported = tmp / 'exported.ndjson'
    loss_result = run('wifi_security_replay', '--record', exported, damaged)
    assert loss_result['input_lines_rejected'] == 1
    assert loss_result == run('wifi_security_replay', exported)
    print('PASS Rejected Wi-Fi input boundary survives recording re-export')
    iq = tmp / old['iq_file']
    raw = bytearray(iq.read_bytes()); raw[0] ^= 1; iq.write_bytes(raw)
    run('wifi_security_replay', manifest, ok=False)
    print('PASS IQ checksum failure stops classification')
    lora_src = sorted((root / 'tests/fixtures/lora_sx1262').glob('*/manifest.json'))[0].parent
    lora = tmp / 'lora'; shutil.copytree(lora_src, lora)
    lm = json.loads((lora / 'manifest.json').read_text())
    lm.update(version=2, run_id='synthetic-lora-anchor', capture_seq=1, radio_session=1,
              timing=timing(lm['sample_count']))
    (lora / 'manifest.json').write_text(json.dumps(lm))
    lora_record = tmp / 'lora.ndjson'
    lr = run('lora_security_replay', '--record', lora_record, lora)
    assert lr['eligible'] > 0 and lr['device_timed'] == 1
    assert lr['attack_rules_enabled'] == 0
    assert lr == run('lora_security_replay', lora_record)
    assert all(e['start_uncertainty_samples'] > 0 for e in lr['recent'])
    print('PASS Real SX1262 IQ -> synthetic timestamp anchors -> LoRa event replay equivalence')
    bad_lora = tmp / 'bad-lora.ndjson'
    bad_lora.write_text('{not-json\n' + lora_record.read_text())
    exported_lora = tmp / 'exported-lora.ndjson'
    rejected = run('lora_security_replay', '--record', exported_lora, bad_lora, ok=False)
    repeated = run('lora_security_replay', exported_lora, ok=False)
    assert json.loads(rejected.stdout) == json.loads(repeated.stdout)
    print('PASS Rejected LoRa input remains a gap after re-export')
    assert run('lora_security_replay', lora_src)['device_timed'] == 0
    print('PASS LoRa legacy recording timing remains unavailable')
print('PASS All CLI replay integration checks')
