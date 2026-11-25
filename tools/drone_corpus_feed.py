"""Feeds drone recordings from the public datasets and our own X310 sessions to drone_corpus_check.

Each recording (or each piece of one, with its start time) is streamed to the tool's stdin as
cf32/sc16 with its rate and centre; the tool's offset-log lines go to OUT/<dataset>/<run>.jsonl
and a manifest line per recording to OUT/manifest.jsonl (model, link family, rate, centre,
pieces). Locked DRFF-R2 test units are never opened: every path and archive member goes through
tools.drone_ml.universal_inventory.locked_name first, and a mixed recording naming one is skipped.

  python -m tools.drone_corpus_feed --out DIR [--datasets tampere,leuven,rfuav,dronerfb,drff,own]
                                    [--limit-per-group N] [--capture-s 0.5] [--jobs N] [--no-droneid]
                                    [--only REGEX] [--rfuav-negatives]
Run from the repository root with a Python that has numpy and h5py.
"""
import argparse
import concurrent.futures as cf
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading

import numpy as np

from tools.drone_ml.universal_inventory import locked_name

ROOT = Path('/media/sudeep/Storage/datasets')
REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / 'build/drone_corpus_check'

TAMPERE = dict(DJI_inspire_2=('Inspire 2', 'lightbridge'), DJI_matrice_100=('Matrice 100', 'lightbridge'),
               DJI_matrice_210=('Matrice 210', 'lightbridge'), DJI_phantom_4=('Phantom 4', 'lightbridge'),
               DJI_phantom_4_pro_plus=('Phantom 4 Pro+', 'lightbridge'), DJI_mavic_pro=('Mavic Pro', 'ocusync'),
               DJI_mavic_mini=('Mavic Mini', 'wifi_drone'), Parrot_disco=('Parrot Disco', 'wifi_drone'),
               Parrot_mambo_control=('Parrot Mambo', 'other_drone'), Parrot_mambo_video=('Parrot Mambo', 'other_drone'),
               Yuneec_typhoon_h=('Yuneec Typhoon H', 'other_drone'))
LEUVEN = dict(mini2=('Mini 2', 'ocusync'), inspire2=('Inspire 2', 'lightbridge'), matrice=('Matrice 300', 'ocusync'),
              Mavic=('Mavic 2 Enterprise Advanced', 'ocusync'))
RFUAV_DJI = {'DJI AVATA2': ('Avata 2', 'ocusync'), 'DJI FPV COMBO': ('FPV', 'ocusync'),
             'DJI MAVIC3 PRO': ('Mavic 3 Pro', 'ocusync'), 'DJI MINI3': ('Mini 3', 'ocusync'),
             'DJI MINI4 PRO': ('Mini 4 Pro', 'ocusync'), 'DAUTEL EVO NANO': ('Autel Evo Nano', 'other_drone')}
DIR = dict(A='DroneRFb A (code 84)', C='DroneRFb C (code 88)', D='DroneRFb D (code 93)', E='Mini 3',
           F='DroneRFb F (code 90)', G='Air 2S')
DRFF = dict(mavicAir2='Air 2', mavicAir2s='Air 2S', mini4PRO='Mini 4 Pro', mini5PRO='Mini 5 Pro', mavic3='Mavic 3',
            mini3pro='Mini 3 Pro', mavic_air2s='Air 2S')


def guard(path, member=''):
    if locked_name(path) or locked_name(member):
        raise PermissionError('locked: %s %s' % (path, member))


class Piece:
    """One stretch of one recording: how to read it, where it starts and (if not the recording's)
    its centre frequency."""
    def __init__(self, reader, time0=0.0, label='', centre=None):
        self.reader, self.time0, self.label, self.centre = reader, time0, label, centre


def sc16_file(path):
    return ('file', str(path), 'sc16')


def rar_member(archive, member):
    return ('rar', str(archive), member)


def mat_iq(path, kind):
    return ('mat', str(path), kind)


def run_tool(piece, rec, out, args, seq0):
    """Streams one piece to the tool; returns its stdout lines."""
    kind, src, extra = piece.reader
    cmd = [str(TOOL), '--rate', repr(rec['rate']), '--centre', repr(piece.centre or rec['centre']), '--run', rec['run'],
           '--time0', repr(piece.time0), '--capture-s', repr(args.capture_s), '--seq0', str(seq0),
           '--threads', str(args.threads)]
    if args.droneid:
        cmd.append('--droneid')
    if args.view_step:
        cmd += ['--view-step', repr(args.view_step)]
    cmd += args.extra.split()
    if kind == 'file':
        guard(src)
        p = subprocess.run(cmd + ['--input', src, '--format', extra], capture_output=True, check=False)
    elif kind == 'rar':
        guard(src, extra)
        unrar = subprocess.Popen(['unrar', 'p', '-inul', src, extra], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        p = subprocess.run(cmd + ['--format', 'cf32'], stdin=unrar.stdout, capture_output=True, check=False)
        unrar.stdout.close(); unrar.wait()
    elif kind == 'mat':
        guard(src)
        import h5py
        with h5py.File(src, 'r') as h:
            if extra == 'uhd':        # KU Leuven: complex float64 compound
                d = h['uhd_samps'][()].reshape(-1)
                iq = (d['real'] + 1j * d['imag']).astype(np.complex64)
            elif extra == 'iq_rows':  # DroneRFb: I, Q float32 (1, n)
                iq = (h['I'][()].reshape(-1) + 1j * h['Q'][()].reshape(-1)).astype(np.complex64)
            elif extra == 'rf0':      # DRFF-R2: RF0_I, RF0_Q float32 (n, 1)
                iq = (h['RF0_I'][()].reshape(-1) + 1j * h['RF0_Q'][()].reshape(-1)).astype(np.complex64)
            else:
                raise ValueError(extra)
        p = subprocess.run(cmd + ['--format', 'cf32'], input=iq.tobytes(), capture_output=True, check=False)
        del iq
    else:
        raise ValueError(kind)
    if p.returncode:
        raise RuntimeError('%s %s: %s' % (rec['run'], piece.label, p.stderr.decode(errors='replace')[-400:]))
    return p.stdout.decode().splitlines(), p.stderr.decode(errors='replace').strip()


# ---- sources -------------------------------------------------------------------------------

def tampere(args):
    for path in sorted((ROOT / 'Tampere_DroneRF').glob('*.bin')):
        m = re.fullmatch(r'(.+?)_(2G|5G)(?:_(\d)of(\d))?', path.stem)
        model, family = TAMPERE[m[1]]
        band = m[2]
        rate = 120e6 if band == '2G' else 200e6
        centre = 2440e6 if band == '2G' else 5700e6 if model == 'Yuneec Typhoon H' else 5800e6
        yield dict(dataset='tampere', run='tampere_' + path.stem, model=model, family=family, rate=rate, centre=centre,
                   aircraft='tampere:' + m[1], receiver='tampere', pieces=[Piece(sc16_file(path), 0.0, path.name)])


def leuven(args):
    base = ROOT / '_derived/kuleuven_members_02'
    for d in sorted(base.iterdir()):
        stem = re.sub(r'(RC|Vid|vid)\d*$', '', d.name)
        if stem not in LEUVEN:
            continue
        model, family = LEUVEN[stem]
        role = 'controller' if 'RC' in d.name else 'aircraft'
        for path in sorted(d.glob('*.mat'))[:args.limit_per_group]:
            yield dict(dataset='leuven', run='leuven_%s_%s' % (d.name, path.stem), model=model, family=family,
                       role=role, rate=100e6, centre=2440e6, aircraft='leuven:' + stem, receiver='leuven',
                       pieces=[Piece(mat_iq(path, 'uhd'), 0.0, path.name)])


def rar_list(archive):
    out = subprocess.run(['unrar', 'lb', str(archive)], capture_output=True, text=True, check=True).stdout.split('\n')
    return [m for m in out if m]


def rfuav_xml(archive, member):
    raw = subprocess.run(['unrar', 'p', '-inul', str(archive), member], capture_output=True, check=True).stdout
    from tools.drone_ml.corpus_sources import xml_metadata
    return xml_metadata(raw)


def rfuav(args):
    names = dict(RFUAV_DJI)
    if args.rfuav_negatives:
        names = {p.stem: (p.stem, 'hobby_link') for p in sorted((ROOT / 'RFUAV').glob('*.rar')) if p.stem not in RFUAV_DJI}
    for name, (model, family) in names.items():
        archive = ROOT / 'RFUAV' / (name + '.rar')
        members = rar_list(archive)
        packs = {}
        for m in members:
            mm = re.search(r'(pack\d+)_(\d+)-(\d+)s\.iq$', m)
            if mm:
                packs.setdefault((str(Path(m).parent), mm[1]), []).append((int(mm[2]), m))
        for (folder, pack), slices in sorted(packs.items()):
            xmls = [m for m in members if m.endswith('.xml') and str(Path(m).parent) == folder]
            meta = rfuav_xml(archive, xmls[0]) if xmls else None
            if not meta:
                continue
            slices.sort()
            slices = slices[:args.max_slices]
            tag = re.sub(r'\W+', '_', folder.split('/')[-1]) + '_' + pack
            yield dict(dataset='rfuav', run='rfuav_%s_%s' % (re.sub(r'\W+', '_', name), tag), model=model, family=family,
                       rate=meta['SampleRate'], centre=meta['CenterFrequency'], aircraft='rfuav:' + name,
                       receiver='rfuav', serial=meta.get('SerialNumber'),
                       pieces=[Piece(rar_member(archive, m), float(t0), Path(m).name) for t0, m in slices])


def dronerfb(args):
    directory = ROOT / 'DroneRFb-DIR/extracted/twin_droneRF'
    per = {}
    for line in (directory / 'train_labels.txt').read_text().splitlines():
        name = line.split()[0]
        m = re.fullmatch(r'([A-Z])(\d)_(IN|OUT)(?:_(S\d))?_slice_(\d+)\.mat', name)
        if not m or m[1] not in DIR:
            continue
        per.setdefault(m[1] + m[2], []).append(name)
    for aircraft, files in sorted(per.items()):
        for name in sorted(files)[:args.limit_per_group]:
            path = directory / 'train' / name
            yield dict(dataset='dronerfb', run='dronerfb_' + Path(name).stem, model=DIR[aircraft[0]], family='ocusync',
                       rate=80e6, centre=2440e6, aircraft='dronerfb:' + aircraft, receiver='dronerfb',
                       pieces=[Piece(mat_iq(path, 'iq_rows'), 0.0, name)])


def drff(args):
    base = ROOT / 'DRFF-R2/DRFF-R2'
    groups = {}
    for path in sorted(base.rglob('*.mat')):
        if locked_name(path):
            continue
        rel = path.relative_to(base)
        top = rel.parts[0]
        if top == 'dataset2-drone_mixed':
            units = re.findall(r'(mavicAir2s|mavicAir2|mini4PRO|mini5PRO|mavic3|mini3pro|mavic_air2s)_(\d)', rel.parts[1], re.I)
            key = ('mixed', rel.parts[1])
            truth = sorted({'%s_%s' % (u.lower().replace('mavic_air2s', 'mavicair2s'), n) for u, n in units})
            if any(locked_name(u) for u in truth):
                continue
            groups.setdefault(key, []).append((path, truth))
        elif top == 'dataset3-single_drone_hover':
            m = re.match(r'(mavicAir2s|mavicAir2|mini4PRO|mini5PRO|mavic3|mini3pro)_(\d)_hover_(c\d)_(u\d)_(d\d)', path.stem)
            if m:
                groups.setdefault(('hover', m[1] + '_' + m[2]), []).append((path, [m[1].lower() + '_' + m[2]]))
        elif top == 'dataset7-environment':
            groups.setdefault(('environment', path.stem), []).append((path, []))
    for (kind, name), items in sorted(groups.items()):
        for path, truth in items[:args.limit_per_group]:
            stem = re.sub(r'\W+', '_', str(path.relative_to(base).with_suffix('')))
            first = truth[0].split('_')[0] if truth else ''
            model = 'mixed' if kind == 'mixed' else DRFF.get(re.sub(r'_?\d$', '', name), name) if kind == 'hover' else 'not_a_drone'
            yield dict(dataset='drff', run='drff_' + stem, model=model, family='ocusync' if truth else 'not_a_drone',
                       truth_units=truth, kind=kind, rate=100e6, centre=5745e6, aircraft='drff:' + '+'.join(truth),
                       receiver='drff:' + (re.search(r'_(u\d)_', path.name)[1] if re.search(r'_(u\d)_', path.name) else '?'),
                       pieces=[Piece(mat_iq(path, 'rf0'), 0.0, path.name)])


def iso_seconds(text):
    from datetime import datetime
    return datetime.fromisoformat(text).timestamp()


def own(args):
    """Our X310 sessions at 20 MSps: the Air 3S pilot (1 s segments on several centres) and the
    60 min no-drone background (0.5 s captures every 5 s on 11 live centres; negatives)."""
    sessions = [(Path('/media/sudeep/Storage/x310_sessions/air3s-pilot-20261110-083613'), 'Air 3S', 'ocusync', 'segments'),
                (Path('/media/sudeep/Storage/drone_universal_classifier/background_x310/2026-11-13/nodrone-60min-20261113-093454'),
                 'not_a_drone', 'not_a_drone', 'captures')]
    for d, model, family, key in sessions:
        s = json.loads((d / 'session.json').read_text())
        items = [c for c in s[key] if c.get('status', 'ok') == 'ok']
        t_first = iso_seconds(items[0]['host_start'])
        pieces = [Piece(sc16_file(d / c['file']), iso_seconds(c['host_start']) - t_first, c['file'], float(c['centre_hz']))
                  for c in items[:args.max_slices]]
        yield dict(dataset='own', run='own_' + d.name, model=model, family=family, rate=float(s['rate_hz']),
                   centre=float(items[0]['centre_hz']), aircraft='own:' + d.name, receiver='own_x310', pieces=pieces)


SOURCES = dict(tampere=tampere, leuven=leuven, rfuav=rfuav, dronerfb=dronerfb, drff=drff, own=own)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--datasets', default='tampere,leuven,rfuav,dronerfb,drff')
    ap.add_argument('--limit-per-group', type=int, default=6)
    ap.add_argument('--max-slices', type=int, default=20)
    ap.add_argument('--capture-s', type=float, default=0.5)
    ap.add_argument('--view-step', type=float, default=0)
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--threads', type=int, default=6)
    ap.add_argument('--no-droneid', dest='droneid', action='store_false')
    ap.add_argument('--only', default='')
    ap.add_argument('--rfuav-negatives', action='store_true')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--extra', default='', help='more drone_corpus_check options, e.g. "--symbol-grid"')
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    recs = []
    for name in args.datasets.split(','):
        for rec in SOURCES[name](args):
            if args.only and not re.search(args.only, rec['run']):
                continue
            recs.append(rec)
    if args.list:
        for r in recs:
            print(r['run'], r['model'], r['family'], r['rate'], r['centre'], len(r['pieces']))
        print(len(recs), 'recordings')
        return
    lock = threading.Lock()
    manifest = open(out / 'manifest.jsonl', 'a')

    def do(rec):
        target = out / rec['dataset'] / (rec['run'] + '.jsonl')
        if target.exists():
            return rec['run'], 'exists'
        target.parent.mkdir(parents=True, exist_ok=True)
        lines, notes, seq = [], [], 0
        for piece in rec['pieces']:
            got, note = run_tool(piece, rec, out, args, seq)
            seq += sum(1 for l in got if '"kind":"link"' in l.replace(' ', ''))
            lines += got
            notes.append(note)
        tmp = target.with_suffix('.part')
        tmp.write_text('\n'.join(lines) + ('\n' if lines else ''))
        tmp.rename(target)
        info = {k: v for k, v in rec.items() if k != 'pieces'}
        info.update(pieces=[p.label for p in rec['pieces']], time0=[p.time0 for p in rec['pieces']], lines=len(lines),
                    capture_s=args.capture_s, droneid=args.droneid)
        with lock:
            manifest.write(json.dumps(info) + '\n'); manifest.flush()
        return rec['run'], '; '.join(notes)[-200:]

    with cf.ThreadPoolExecutor(args.jobs) as pool:
        for fut in cf.as_completed([pool.submit(do, r) for r in recs]):
            try:
                run, note = fut.result()
                print(run, note, flush=True)
            except Exception as e:  # keep going; report
                print('FAILED', e, flush=True)


if __name__ == '__main__':
    main()
