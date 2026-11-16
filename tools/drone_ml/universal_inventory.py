"""Universal-classifier metadata inventory. Never opens IQ or locked recordings.

Archive directories are enumerated; existing audited metadata is reused with size/mtime
checks. No archive member is extracted. This is preparation, not a training admission list.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import zipfile

TYPES = ('dji_ofdm', 'lightbridge', 'wifi_drone', 'hopping_controller',
         'analogue_video', 'other_drone', 'not_a_drone')
DRFF = dict(mavicAir2='Air 2', mavicAir2s='Air 2S', mini4PRO='Mini 4 Pro',
            mini5PRO='Mini 5 Pro', mavic3='Mavic 3', mini3pro='Mini 3 Pro')
DIR = dict(A='DroneRFb A (code 84)', C='DroneRFb C (code 88)', D='DroneRFb D (code 93)',
           E='Mini 3', F='DroneRFb F (code 90)', G='Air 2S')
TAMPERE = dict(DJI_inspire_2='Inspire 2', DJI_matrice_100='Matrice 100',
               DJI_matrice_210='Matrice 210', DJI_mavic_mini='Mavic Mini',
               DJI_mavic_pro='Mavic Pro', DJI_phantom_4='Phantom 4',
               DJI_phantom_4_pro_plus='Phantom 4 Pro+', Parrot_disco='Parrot Disco',
               Parrot_mambo_control='Parrot Mambo', Parrot_mambo_video='Parrot Mambo',
               Yuneec_typhoon_h='Yuneec Typhoon H')
LEUVEN = {'mini2': 'Mini 2', 'inspire2': 'Inspire 2', 'matrice': 'Matrice 300',
          'Mavic': 'Mavic 2 Enterprise Advanced', 'Frysky': 'Taranis Q X7',
          'NineEagles': 'Nine Eagles', 'Q205': 'Q205', 'SJRC_pro': 'SJRC F11 Pro',
          'Spektrum_DX4e': 'Spektrum DX4e', 'wltoys': 'WLtoys'}
RFUAV = {'DJI AVATA2': 'Avata 2', 'DJI FPV COMBO': 'FPV', 'DJI MAVIC3 PRO': 'Mavic 3 Pro',
         'DJI MINI3': 'Mini 3', 'DJI MINI4 PRO': 'Mini 4 Pro', 'DAUTEL EVO NANO': 'Autel Evo Nano'}


def locked_name(path):
    text = str(path).lower()
    return bool(re.search(r'mavic3[cs](?:[_/.]|$)', text) or
                re.search(r'mavicair2_(?:[5-9]|\d{2,})(?:[_/.]|$)', text) or
                re.search(r'mavicair2s_(?:[5-9]|\d{2,})(?:[_/.]|$)', text) or
                re.search(r'mini4pro_(?:[5-9]|\d{2,})(?:[_/.]|$)', text))


class SourceGuard:
    """Deny locked names, symlink aliases, and hardlink aliases before opening anything."""
    def __init__(self, root):
        self.root = Path(root).resolve(strict=True)
        self.locked_inodes = set()
        for p in self.root.rglob('*'):
            if locked_name(p) and p.is_file():
                s = p.stat()
                self.locked_inodes.add((s.st_dev, s.st_ino))

    def check(self, path, member=None):
        path = Path(path)
        resolved = path.resolve(strict=True)
        if self.root not in resolved.parents or locked_name(path) or locked_name(resolved) or locked_name(member or ''):
            raise PermissionError('locked or outside corpus: ' + str(path))
        s = resolved.stat()
        if (s.st_dev, s.st_ino) in self.locked_inodes:
            raise PermissionError('locked inode alias: ' + str(path))
        if member and (Path(member).is_absolute() or '..' in Path(member).parts):
            raise PermissionError('unsafe archive member')
        return resolved

    def open(self, path):
        resolved = self.check(path)
        fd = os.open(resolved, os.O_RDONLY | os.O_NOFOLLOW | os.O_NOATIME | os.O_NONBLOCK)
        s = os.fstat(fd)
        if not stat.S_ISREG(s.st_mode) or (s.st_dev, s.st_ino) in self.locked_inodes:
            os.close(fd)
            raise PermissionError('not an admitted regular file')
        return os.fdopen(fd, 'rb')


def read_json(path):
    return json.loads(Path(path).read_text())


def proposal(model, dataset, role=''):
    if model is None:
        return None, 'Identity unresolved; exclude from model training pending review.'
    if model == 'not_a_drone':
        return 'not_a_drone', 'Background context only; scene approval and per-window review still required.'
    if model in ('Inspire 2', 'Matrice 100', 'Matrice 210', 'Phantom 4', 'Phantom 4 Pro', 'Phantom 4 Pro+'):
        return 'lightbridge', 'Plan section 4 Lightbridge assignment; verify presence at 32.8 kHz.'
    if model in ('Mavic Mini', 'Parrot Disco', 'Parrot Mambo', 'Yuneec Typhoon H', 'SJRC F11 Pro', 'Autel Evo Nano'):
        return 'wifi_drone', 'Plan section 4 candidate Wi-Fi assignment; requires measurement (especially Autel).'
    if dataset == 'DRFF-R2' or dataset == 'DroneRFb-DIR' or model in (
            'Air 3S', 'Mini 2', 'Matrice 300', 'Mavic 2 Enterprise Advanced', 'Mavic Pro',
            'Avata 2', 'FPV', 'Mavic 3 Pro', 'Mini 3', 'Mini 4 Pro'):
        return 'dji_ofdm', ('Measured 15 kHz family in DIR probes and decoded product codes.' if dataset == 'DroneRFb-DIR'
                            else 'Plan section 4 DJI OFDM assignment; each positive window still requires own-link timing.')
    return 'hopping_controller', 'Controller equipment label; proposed hopping type needs burst/timing measurement.'


def compile_inventory(root, repo, sessions):
    root, repo = Path(root).resolve(), Path(repo).resolve()
    guard = SourceGuard(root)
    old = read_json(repo/'data/drone_ml/2026-10-20/expanded_v2/inventory.json')
    review = read_json(repo/'data/drone_ml/2026-10-20/expanded_v2/review/review.json')
    previous = {r['source']: r for r in old['records']}
    decisions = {r['source']: r for r in review['source_decisions']}
    records, containers = [], []

    def add(dataset, path, model=None, member=None, aircraft=None, receiver=None, rate=None,
            center=None, flags=(), session=None, metadata_basis=None, **extra):
        path = Path(path)
        signal, reason = proposal(model, dataset)
        locked = locked_name(path) or locked_name(member or '')
        flags = list(flags)
        if model == 'not_a_drone':
            aircraft = None
        if locked:
            flags.append('locked_unreadable')
        if aircraft is None and model not in (None, 'not_a_drone'):
            aircraft = dataset + ':unresolved_single_unit:' + model
            flags.append('physical_unit_unverified')
        row = dict(recording_id=hashlib.sha256((str(path)+'\0'+(member or '')).encode()).hexdigest()[:24],
                   dataset=dataset, source=str(path), member=member, model=model, aircraft=aircraft,
                   receiver=receiver, rate_hz=rate, center_hz=center, session=session,
                   signal_type=signal, type_reason=reason, flags=flags,
                   metadata_basis=metadata_basis, readable=not locked,
                   training_eligible=False, iq_opened=False, **extra)
        records.append(row)
        return row

    def cached(path):
        row = previous.get(str(path))
        if row:
            s = path.stat()
            if s.st_size != row['source_stat'][2] or s.st_mtime_ns != row['source_stat'][4]:
                raise ValueError('source changed since metadata audit: ' + str(path))
        return row or {}

    for path in sorted((root/'DRFF-R2').rglob('*.mat')):
        base = cached(path)
        meta = base.get('embedded_metadata', {})
        target = meta.get('TD') or path.stem
        match = re.match(r'(mavicAir2s|mavicAir2|mini4PRO|mini5PRO|mavic3|mini3pro)_(\d+)(?:_|$)', target)
        model = DRFF[match[1]] if match else None
        flags = []
        if 'environment' in path.name:
            model = 'not_a_drone'; flags.append('environment_absence_unverified')
        elif not match:
            flags.append('unresolved_or_mixed_identity')
        if 'mixed' in str(path): flags.append('mixed_recording_not_clean_model_truth')
        rate = meta.get('Fs', [None])[0]
        center = (meta.get('CenterFreq') or meta.get('CenterFrequence') or [None])[0]
        receiver_match = re.search(r'_(u\d+)_', path.name)
        day_match = re.search(r'_(d\d+)\.mat$', path.name)
        add('DRFF-R2', path, model, aircraft='DRFF-R2:'+match[0].rstrip('_') if match else None,
            receiver=receiver_match[1] if receiver_match else 'USRP (unspecified unit)', rate=rate, center=center,
            flags=flags, session=day_match[1] if day_match else None,
            metadata_basis='existing audit metadata; filename listing only for locked files')

    directory = root/'DroneRFb-DIR/extracted/twin_droneRF'
    for split in ('train', 'test'):
        for line in (directory/(split+'_labels.txt')).read_text().splitlines():
            parts = line.split(); original = parts[0]
            path = directory/split/(original if split == 'train' else parts[1]+'.mat')
            if not path.is_file(): raise ValueError('missing DIR recording: '+str(path))
            match = re.fullmatch(r'(?:([A-Z])(\d)_(IN|OUT)(?:_(S\d))?|(background))_slice_(\d+)\.mat', original)
            if not match: raise ValueError('unknown DIR filename: '+original)
            model = DIR[match[1]] if match[1] else 'not_a_drone'
            add('DroneRFb-DIR', path, model, aircraft='DroneRFb-DIR:'+match[1]+match[2] if match[1] else None,
                receiver='USRP (unit unspecified)', rate=80e6, center=2440e6,
                session=match[3] + ':' + (match[4] or '') if match[3] else None,
                flags=['center_inferred_from_published_span', 'publisher_split_is_not_aircraft_disjoint'],
                publisher_partition=split, original_name=original,
                metadata_basis='publisher label files; existing timing/DroneID reports')

    for path in sorted((root/'KULeuven_DroneRF').glob('*.zip')):
        stem = re.sub(r'(RC|[Vv]id)\d*$', '', path.stem)
        model = LEUVEN.get(stem)
        flags = ['type_assignment_requires_measurement']
        if stem == 'Mavic': flags.append('Mavic_archive_model_not_confirmed_by_local_README')
        with guard.open(path) as f, zipfile.ZipFile(f) as archive:
            members = [m for m in archive.infolist() if m.filename.endswith('.mat')]
        for m in members:
            guard.check(path, m.filename)
            add('KULeuven_DroneRF', path, model, member=m.filename, receiver='USRP X310',
                rate=100e6, center=2440e6, flags=flags, metadata_basis='local README; archive name/directory',
                role='controller_only' if 'RC' in path.stem or stem not in ('mini2','inspire2','matrice','Mavic') else 'video_or_combined',
                member_bytes=m.file_size, member_crc32=m.CRC)
        containers.append(dict(source=str(path), recordings=len(members)))
    for path in sorted((root/'KULeuven_DroneRF').rglob('*.mat')):
        stem = re.sub(r'(RC|[Vv]id)\d*$', '', path.parent.name)
        add('KULeuven_DroneRF', path, LEUVEN.get(stem), receiver='USRP X310', rate=100e6, center=2440e6,
            flags=['possible_extracted_duplicate'], metadata_basis='local README; directory name; not independently counted')

    for path in sorted((root/'RFUAV').glob('*.rar')):
        guard.check(path); cached(path)
        names = subprocess.run(['unrar', 'lb', str(path)], check=True, capture_output=True,
                               text=True, timeout=60).stdout.splitlines()
        xmls = {x['member']: x['metadata'] for x in decisions.get(str(path), {}).get('xml_review', [])}
        members = [n for n in names if n.endswith('.iq')]
        for member in members:
            guard.check(path, member)
            xml = re.sub(r'_\d+-\d+s\.iq$', '.xml', member)
            meta = xmls.get(xml, {})
            flags = ['type_assignment_requires_measurement']
            if not meta:
                same_folder = [(name, m) for name, m in xmls.items() if Path(name).parent == Path(member).parent]
                if len(same_folder) == 1:
                    xml, meta = same_folder[0]
                    flags.append('pack_filename_mismatch_requires_review')
            norm = lambda s: re.sub('[^a-z0-9]', '', s.lower())
            if meta and norm(meta['Drone']) != norm(path.stem): flags.append('archive_XML_label_conflict')
            if not meta: flags.append('missing_pack_metadata')
            if path.stem == 'DAUTEL EVO NANO': flags.append('DAUTEL_alias_requires_review')
            add('RFUAV', path, RFUAV.get(path.stem, path.stem), member=member,
                receiver=meta.get('DeviceType'), rate=meta.get('SampleRate'), center=meta.get('CenterFrequency'),
                flags=flags, session=xml, xml_drone_label=meta.get('Drone'),
                metadata_basis='current RAR directory; cached audited pack XML (archive size/mtime checked)')
        containers.append(dict(source=str(path), recordings=len(members)))

    for path in sorted((root/'Tampere_DroneRF').glob('*.bin')):
        match = re.fullmatch(r'(.+)_(2G|5G)(?:_\dof\d)?\.bin', path.name)
        model = TAMPERE.get(match[1]) if match else None
        band = match[2] if match else None
        add('Tampere_DroneRF', path, model, receiver='publisher receiver unspecified',
            rate=120e6 if band=='2G' else 200e6, center=2440e6 if band=='2G' else 5700e6 if model=='Yuneec Typhoon H' else 5800e6,
            metadata_basis='existing publisher-verified review; original sample rate, not plan summary',
            flags=['type_assignment_requires_measurement'])

    for path in sorted((root/'FPV_Analog_Video').glob('*.zip')):
        with guard.open(path) as f, zipfile.ZipFile(f) as archive:
            members = [m for m in archive.infolist() if m.filename.endswith('iq.bin')]
        for m in members:
            guard.check(path, m.filename)
            row = add('FPV_Analog_Video', path, member=m.filename, flags=['excluded_format_and_zero_tail_unresolved'],
                      metadata_basis='archive directory only; not admitted', member_bytes=m.file_size)
            row['signal_type'] = 'analogue_video'; row['type_reason'] = 'Held back by plan pending format/zero-tail resolution.'
        containers.append(dict(source=str(path), entries=len(members)))

    pending = [dict(source=str(p), status='incomplete_download_excluded', bytes=p.stat().st_size)
               for p in sorted((root/'DroneRFa').glob('*.part'))]
    for session_path in sorted(Path(sessions).glob('*/session.json')):
        session = read_json(session_path)
        scene_path = session_path.with_name('scene.json')
        scene = read_json(scene_path) if scene_path.exists() else {}
        for seg in session['segments']:
            path = session_path.parent/seg['file']
            flags = ['scene_approval_required']
            if seg.get('status') != 'ok': flags.append('capture_not_ok')
            if not path.is_file(): flags.append('missing_file')
            add('Own_X310', path, 'not_a_drone' if session['label'].startswith('bg-') else None,
                receiver='USRP X310', rate=session['rate_hz'], center=seg['centre_hz'], flags=flags,
                session=session_path.parent.name, metadata_basis='recorder session and scene JSON; no IQ read',
                samples=seg['samples'], prior_partition=scene.get('partition'), scene_approval=scene.get('approval'))
    for path in sorted((repo/'data/air3s_o4').glob('*/*/*.json')):
        meta = read_json(path)
        if 'iq_file' not in meta: continue
        iq = path.parent/meta['iq_file']
        background = 'background' in path.parent.name
        add('Own_X310', iq, 'not_a_drone' if background else 'Air 3S',
            receiver='USRP X310', rate=meta.get('sample_rate_hz'), center=meta.get('capture_center_hz'),
            aircraft=None if background else 'Own_X310:Air_3S:unit_1',
            flags=['scene_approval_required'] + ([] if iq.exists() else ['missing_file']),
            session=str(path.parent.relative_to(repo)), metadata_basis='capture sidecar and existing session report')

    # Original continuous Air 3S files, including the 50 MSps captures (converted views are
    # deliberately not independent records). The capture metadata supplies rates/centres.
    for path in sorted((repo.parent.parent/'air3s_whole').glob('*/*/*.json')):
        meta = read_json(path)
        iq = path.with_suffix('.bin')
        if 'sample_rate_hz' not in meta or not iq.exists(): continue
        add('Own_X310', iq, 'Air 3S', aircraft='Own_X310:Air_3S:unit_1', receiver='USRP X310',
            rate=meta['sample_rate_hz'], center=meta['center_freq_hz'], session='Air3S:2026-10-23',
            flags=['scene_approval_required', 'same_aircraft_as_Oct23_sessions'], metadata_basis='original capture JSON')
    for path in sorted((repo/'data/captures').glob('phantom*/session.json')):
        meta = read_json(path)
        for capture in meta['captures']:
            flags = ['scene_approval_required', 'RF_source_attribution_unverified']
            if capture.get('overflow_reported'): flags.append('overflow_gaps_unknown')
            if capture['rate_MSps'] < 20: flags.append('below_deployment_bandwidth')
            add('Own_X310', path.parent/capture['file'], 'Phantom 4 Pro',
                aircraft='Own_X310:Phantom_4_Pro:unit_1', receiver=meta['receiver'],
                rate=capture['rate_MSps']*1e6, center=capture['center_MHz']*1e6,
                session=path.parent.name, flags=flags, metadata_basis='operator scene and capture session JSON')

    ids = [r['recording_id'] for r in records]
    if len(ids) != len(set(ids)): raise ValueError('duplicate recording identities')
    counts = lambda key: dict(sorted(Counter(str(r[key]) for r in records).items()))
    return dict(schema='rfmon.drone_ml.universal_inventory.v1', records=records, containers=containers,
                pending_downloads=pending, summary=dict(recordings=len(records), by_source=counts('dataset'),
                by_model=counts('model'), by_aircraft=counts('aircraft'), by_type=counts('signal_type'),
                locked_unreadable=sum(not r['readable'] for r in records),
                flags=dict(Counter(f for r in records for f in r['flags']))),
                iq_opened=False, training_admitted=False,
                scope='Original dataset entries, archive members and explicit own-X310 sessions; derived windows excluded.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=Path('/media/sudeep/Storage/datasets'))
    p.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    p.add_argument('--sessions', type=Path, default=Path('/media/sudeep/Storage/x310_sessions'))
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.output.resolve().is_relative_to(args.root.resolve()): raise ValueError('output must be outside source root')
    result = compile_inventory(args.root, args.repo, args.sessions)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x') as f: json.dump(result, f, indent=2); f.write('\n')
    print(json.dumps(result['summary'], indent=2))


if __name__ == '__main__':
    main()
