"""E4: DroneRFa into the B1 view-store format (same views, same files, same metadata as views-v1).

DroneRFa (Yu et al., J. Electronics & Information Technology 46(4), 2024; ScienceDB
10.57760/sciencedb.18475; CC BY-NC-SA 4.0). One RAR5 archive of 631 MATLAB v7.3 files, read here
member by member without unpacking the archive: each member's header and data are copied into a
one-member RAR in /dev/shm and unpacked there, so `unrar` checks the member's CRC; a member that
fails is reported (`members.jsonl`, state crc_error) and nothing of it is used.

Recording facts (paper, sections 2-3): NI USRP-2955, 100 MS/s, two channels recorded at once,
RF0 at 2440 MHz and RF1 at 5800 MHz (FrSky X20 and Taranis Plus: RF0 915 MHz, RF1 2440 MHz);
gain 50 dB; omnidirectional VERT2450 antenna. File name T<model>_D<distance>_S<segment>, distance
D00 20-40 m, D01 40-80 m, D10 80-150 m outdoors (a small town in Hangzhou), no D indoors (about 2 m).
S0xxx: link starts at 915 MHz or 2.4 GHz; S1xxx: switched to 2.4 or 5.8 GHz where the drone can.
Each file holds 1.5e8 samples per channel, written in 0.1 s blocks of 1e7 samples with a gap in
time between blocks (the first ~25 samples of each block are zero). Samples are int16 / 32768.

Views: per channel, three 8.2 ms windows, each inside one block (blocks 2, 7 and 12, from sample
100000 of the block, never across a block boundary), each cut into the same guarded 20 MSps views
as B1 (`universal_views.deployment_view`: 7 offsets at 100 MS/s). 915 MHz channels are not used
(the app does not watch that band). Labels are recording context only; presence is decided later
by B2 from each view's own measurements.

  python -m tools.drone_ml.universal_dronerfa --archive .../DroneRFa.rar.part \
      --layout true_members.json --output <store> [--workers 3]
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import threading
import zlib
import h5py
import numpy as np
from .universal_views import LENGTH, RATE, VERSION as VIEW_VERSION, deployment_view, offsets

VERSION = 'universal_dronerfa_v1'
DATASET = 'DroneRFa'
SOURCE_RATE = 100_000_000
BLOCK, BLOCKS_USED, BLOCK_SKIP = 10_000_000, (2, 7, 12), 100_000
PAD = round(SOURCE_RATE * .0001)
WINDOW = round(SOURCE_RATE * .008) + 2 * PAD
# Model code -> (class name, proposed signal type). Names follow the corpus's existing names;
# the proposed type is checked per view by B2 (own-family timing), never assumed.
CODES = {
    'T0000': ('not_a_drone', 'not_a_drone'),
    'T0001': ('Phantom 3', 'lightbridge'), 'T0010': ('Phantom 4 Pro', 'lightbridge'),
    'T0011': ('Matrice 200', 'lightbridge'), 'T0100': ('Matrice 100', 'lightbridge'),
    'T0101': ('Air 2S', 'dji_ofdm'), 'T0110': ('Mini 3 Pro', 'dji_ofdm'),
    'T0111': ('Inspire 2', 'lightbridge'), 'T1000': ('Mavic Pro', 'dji_ofdm'),
    'T1001': ('Mini 2', 'dji_ofdm'), 'T1010': ('Mavic 3', 'dji_ofdm'),
    'T1011': ('Matrice 300', 'dji_ofdm'), 'T1100': ('Phantom 4 RTK', 'dji_ofdm'),
    'T1101': ('Matrice 30T', 'dji_ofdm'), 'T1110': ('Avata', 'dji_ofdm'),
    'T1111': ('DJI link module (self-built)', 'dji_ofdm'), 'T10000': ('Matrice 600 Pro', 'lightbridge'),
    'T10001': ('VBar', 'hopping_controller'), 'T10010': ('FrSky X20', 'hopping_controller'),
    'T10011': ('Futaba T6IZ', 'hopping_controller'), 'T10100': ('Taranis Plus', 'hopping_controller'),
    'T10101': ('RadioLink AT9S', 'hopping_controller'), 'T10110': ('Futaba T14SG', 'hopping_controller'),
    'T10111': ('Skydroid T12', 'hopping_controller'), 'T11000': ('Skydroid T10', 'hopping_controller'),
}
LOW_BAND_CODES = {'T10010', 'T10100'}            # RF0 at 915 MHz, RF1 at 2440 MHz
NAME = re.compile(r'^DroneRFa/(T[01]+)(?:_(D\d\d))?_(S[01]+)\.mat$')
SIG = b'Rar!\x1a\x07\x01\x00'
_blk = lambda body: struct.pack('<I', zlib.crc32(body)) + body
MAIN, END = _blk(b'\x03\x01\x00\x00'), _blk(b'\x03\x05\x00\x00')


def parse_name(name):
    m = NAME.match(name)
    if not m or m[1] not in CODES:
        return None
    return dict(code=m[1], distance=m[2], segment=m[3])


def channels(code):
    """(channel, centre Hz) pairs that are used; 915 MHz is not."""
    return [('RF1', 2440e6)] if code in LOW_BAND_CODES else [('RF0', 2440e6), ('RF1', 5800e6)]


def recording_id(archive_name, member, channel):
    return hashlib.sha256(f'{DATASET}\0{archive_name}\0{member}\0{channel}'.encode()).hexdigest()[:24]


def source_record(archive, member, info, channel, centre):
    model, signal = CODES[info['code']]
    session = f"{info['code']}_{info['distance'] or 'indoor'}"
    return dict(recording_id=recording_id(Path(archive).name.removesuffix('.part'), member['name'], channel),
                dataset=DATASET, source=str(archive), member=member['name'], member_crc32=member['crc'],
                channel=channel, model=model, signal_type=signal,
                aircraft=None if model == 'not_a_drone' else f'{DATASET}:{model}',
                receiver='NI USRP-2955', rate_hz=SOURCE_RATE, center_hz=centre, readable=True, flags=[],
                code=info['code'], distance=info['distance'], segment=info['segment'], session=session,
                setting='indoor' if info['distance'] is None else 'outdoor', metadata_basis='DroneRFa paper table 3')


def unpack(archive, member, workdir):
    """Copy one member into a one-member RAR and unpack it; returns the .mat path, or None on a CRC error."""
    carved = Path(workdir) / 'one.rar'
    with open(archive, 'rb') as f, open(carved, 'wb') as o:
        o.write(SIG + MAIN); f.seek(member['header']); left = member['end'] - member['header']
        while left:
            b = f.read(min(left, 64 << 20))
            if not b:
                raise IOError('short archive read')
            o.write(b); left -= len(b)
        o.write(END)
    r = subprocess.run(['unrar', 'x', '-idq', '-o+', str(carved), str(workdir) + '/'], capture_output=True, text=True)
    carved.unlink()
    path = Path(workdir) / member['name']
    if r.returncode != 0 or not path.exists() or path.stat().st_size != member['unpacked']:
        return None, (r.stdout + r.stderr).strip()[-200:]
    return path, ''


def blocks_for(samples):
    """Blocks to cut views from. Most files hold 15 blocks; a few are shorter (12 blocks), where the
    last block stands in for any block past the end."""
    n = samples // BLOCK
    if n < len(BLOCKS_USED):
        raise ValueError(f'only {n} blocks')
    return tuple(sorted({min(b, n - 1) for b in BLOCKS_USED}))


def views_of(h, channel):
    i, q = h[channel + '_I'], h[channel + '_Q']
    if i.shape != q.shape or len(i.shape) != 2 or i.shape[0] != 1:
        raise ValueError(f'unexpected {channel} layout {i.shape}')
    views, windows = [], []
    for b in blocks_for(i.shape[1]):
        start = b * BLOCK + BLOCK_SKIP
        x = np.asarray(i[0, start:start + WINDOW], np.float64) + 1j * np.asarray(q[0, start:start + WINDOW], np.float64)
        if not np.isfinite(x).all() or np.count_nonzero(x) < .99 * len(x):
            raise ValueError(f'{channel} block {b}: nonfinite or zero-filled window')
        for off in offsets(SOURCE_RATE):
            views.append(deployment_view(x, SOURCE_RATE, off))
            windows.append(dict(source_start_sample=start + PAD, block=b, offset_hz=off))
    return np.stack(views), windows


def write_views(path, rows, member, output):
    with h5py.File(path, 'r') as h:
        cut = [views_of(h, row['channel']) for row in rows]      # every channel first: no half-written member
    for row, (stack, windows) in zip(rows, cut):
        for w in windows:
            w['center_hz'] = row['center_hz'] + w['offset_hz']
        npy = output / (row['recording_id'] + '.npy')
        with npy.with_suffix('.tmp').open('wb') as f:
            np.save(f, stack, allow_pickle=False)
        npy.with_suffix('.tmp').replace(npy)
        meta = dict(source_record=row, source_signature=[member['crc'], member['unpacked']], windows=windows,
                    rate_hz=RATE, samples_per_window=LENGTH, view_version=VIEW_VERSION, dronerfa_version=VERSION,
                    output_bytes=npy.stat().st_size, iq_sha256=hashlib.sha256(stack.tobytes()).hexdigest(),
                    presence_labels_assigned=False)
        (output / (row['recording_id'] + '.json')).write_text(json.dumps(meta, indent=2) + '\n')


def process(archive, member, output, lock, log):
    info = parse_name(member['name'])
    rows = [source_record(archive, member, info, ch, c) for ch, c in channels(info['code'])]
    if all((output / (r['recording_id'] + '.json')).exists() for r in rows):
        return 'done'
    with tempfile.TemporaryDirectory(dir='/dev/shm', prefix='drfa_') as work:
        path, message = unpack(archive, member, work)
        if path is None:
            state = dict(name=member['name'], state='crc_error', header=member['header'], end=member['end'], message=message)
        else:
            try:
                write_views(path, rows, member, output)
                state = dict(name=member['name'], state='ok', recordings=[r['recording_id'] for r in rows])
            except ValueError as e:                                     # unpacked fine, but not usable
                state = dict(name=member['name'], state='layout_error', message=str(e))
    with lock, open(log, 'a') as f:
        f.write(json.dumps(state) + '\n')
    return state['state']


def build(archive, layout, output, workers=3):
    output = Path(output); output.mkdir(parents=True, exist_ok=True)
    members = [m for m in json.load(open(layout)) if m.get('type') == 2 and parse_name(m.get('name', ''))]
    log = output / 'members.jsonl'
    (output / 'config.json').write_text(json.dumps(dict(
        version=VERSION, view_version=VIEW_VERSION, dataset=DATASET, archive=str(archive), members=len(members),
        blocks_used=BLOCKS_USED, block_skip=BLOCK_SKIP, offsets_hz=offsets(SOURCE_RATE), codes=CODES,
        code_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()), indent=1) + '\n')
    lock = threading.Lock(); counts = {}
    with ThreadPoolExecutor(workers) as pool:
        for n, s in enumerate(pool.map(lambda m: process(archive, m, output, lock, log), members)):
            counts[s] = counts.get(s, 0) + 1
            if n % 20 == 0:
                print(json.dumps(dict(progress=n + 1, of=len(members), **counts)), flush=True)
    return counts


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--archive', type=Path, required=True); p.add_argument('--layout', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True); p.add_argument('--workers', type=int, default=3)
    a = p.parse_args()
    print(json.dumps(build(a.archive, a.layout, a.output, a.workers)))


if __name__ == '__main__':
    main()
