"""B4: window targets and disjoint splits for the first universal classifier.

Scope (user decision 2026-11-13): DJI OFDM and Lightbridge windows whose own timing is measured,
KU Leuven non-OFDM windows labelled by energy, confirmed X310 background, and KU Leuven chamber
noise as an easy negative. Unknown windows are not used.

Partitions (a recording's windows always stay together):
  train, val   - used for fitting and model selection
  test         - multi-aircraft classes: an aircraft never trained on (unseen-aircraft claim)
  unit_test    - single-unit classes: recordings never trained on (unit-match claim only)
  cross_test   - the same model from another dataset than the one trained on
  open_test    - whole models left out of training: must come out as their type, with no model
  bg_test      - the held-out confirmed background session
Reads metadata and labels only; no IQ.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
from .universal_inventory import read_json

VERSION = 'universal_splits_v3'
# Whole models kept out of training. Tiny Lightbridge classes double as the unseen-model test of
# that type; Mini 3 Pro is the unseen-model test for DJI OFDM naming.
# v2 (user decision 14 Nov): Air 3S, the user's own aircraft on the deployment receiver, is trained.
# v3 (E4, 16 Nov): DroneRFa adds Phantom 4 Pro and Matrice 100 recordings, so both are trained
# (Phantom 4 Pro is the user's own aircraft; its X310 recordings become its cross-receiver test).
# Mini 3 Pro and Mavic Pro stay the unseen DJI OFDM models; Phantom 4, Phantom 4 Pro+ and
# Matrice 210 the unseen Lightbridge models.
OPEN_MODELS = {'Mini 3 Pro', 'Taranis Q X7', 'Mavic Pro', 'Matrice 210', 'Phantom 4', 'Phantom 4 Pro+'}
DROPPED_MODELS = {'Q205'}                        # no window with a signal
# The datasets each cross-dataset model is trained on; its other sources go to cross_test.
# v3: Inspire 2 trains on KU Leuven and DroneRFa (two receivers), Tampere stays cross_test.
HOME_SOURCE = {'Mini 3': {'DroneRFb-DIR'}, 'Mini 4 Pro': {'DRFF-R2'}, 'Inspire 2': {'KULeuven_DroneRF', 'DroneRFa'},
               'Phantom 4 Pro': {'DroneRFa'}, 'Matrice 100': {'DroneRFa'}}
# DroneRFa (E4): one aircraft per model. Outdoor files are split by flight distance (each distance
# is a separate flight): D00 (20-40 m) train, D01 (40-80 m) val, D10 (80-150 m) unit_test. Indoor
# files (about 2 m, one session) by segment: S0000-S0100 and S1000-S1001 train, S0101 and S1010
# val, S0110, S0111 and S1011 unit_test.
DRONERFA_DISTANCE = {'D00': 'train', 'D01': 'val', 'D10': 'unit_test'}
DRONERFA_INDOOR = {'S0000': 'train', 'S0001': 'train', 'S0010': 'train', 'S0011': 'train', 'S0100': 'train',
                   'S0101': 'val', 'S0110': 'unit_test', 'S0111': 'unit_test',
                   'S1000': 'train', 'S1001': 'train', 'S1010': 'val', 'S1011': 'unit_test', 'S1100': 'unit_test'}
# Single-unit models split by recording session, not by file: files of one session are near copies.
# Air 3S (one unit, 23 Oct): two air3s_whole sessions train, the third validates, and the separate
# air3s_o4 session (other day-part, other recorder) is the held-out test.
SESSION_PARTITION = {'Air 3S': {'air3s10': 'train', 'air3s40': 'train', 'air3s20': 'val', 'air3s_o4': 'unit_test'}}
ENERGY_TYPE = 'hopping_controller'               # KU Leuven non-OFDM classes, incl. SJRC F11 Pro (review 13 Nov)


def bucket(text, salt):
    return int(hashlib.sha256((salt + '|' + text).encode()).hexdigest()[:8], 16) / 0xffffffff


def unit_number(aircraft):
    m = re.search(r'(\d+)$', aircraft or '')
    return int(m.group(1)) if m else None


def partition(row, multi_aircraft):
    """Partition of one recording."""
    model, dataset = row['model'], row['dataset']
    if model == 'not_a_drone':
        if dataset == 'DroneRFa':                   # the dataset's own background class (town, Wi-Fi, Bluetooth;
            return 'val' if row['segment'] in ('S0111', 'S1111') else 'train'   # 16 files, all D00)
        if row.get('background_partition') == 'held_out':
            return 'bg_test'
        return 'val' if bucket(row['source'], 'bg') < .15 else 'train'
    if model in OPEN_MODELS:
        return 'open_test'
    if model in SESSION_PARTITION:
        hits = [part for name, part in SESSION_PARTITION[model].items() if name in Path(row['source']).parts]
        if len(hits) != 1:
            raise ValueError(f'{model}: no single session partition for {row["source"]}')
        return hits[0]
    if model in HOME_SOURCE and dataset not in HOME_SOURCE[model]:
        return 'cross_test'
    if dataset == 'DroneRFa':                       # one aircraft per model: split by flight / segment
        return DRONERFA_DISTANCE[row['distance']] if row.get('distance') else DRONERFA_INDOOR[row['segment']]
    if model in multi_aircraft:
        n = unit_number(row.get('aircraft'))
        if n is None:
            raise ValueError('multi-aircraft class without a unit number: ' + str(row.get('aircraft')))
        if dataset == 'DRFF-R2':                    # units 1-2 train, 3 validation, 4 test
            return 'train' if n <= 2 else 'val' if n == 3 else 'test'
        if n == 3:                                  # DroneRFb-DIR: aircraft 3 is the test aircraft
            return 'test'
        return 'val' if bucket(row['source'], 'dir-val') < .12 else 'train'   # by file: DIR has few sessions
    b = bucket(row['source'] + '|' + str(row.get('member')), 'unit')   # single unit: split by recording
    return 'unit_test' if b < .25 else 'val' if b < .37 else 'train'


def targets(row, labels, overlay, presence_filter=True):
    """(signal type, model) per window, or None when the window is not used.

    presence_filter=False is the C3 ablation: every window of a drone recording is labelled as
    that drone, whether or not a signal was measured in it. Background recordings are unchanged.
    """
    out = []
    energy = overlay.get(row['recording_id'])
    if not presence_filter and row['model'] != 'not_a_drone':
        kind = ENERGY_TYPE if energy is not None else row['signal_type'] if row['signal_type'] in ('dji_ofdm', 'lightbridge') else None
        return [(kind, row['model']) if kind else None for _ in labels]
    for i, decision in enumerate(labels):
        if energy is not None:
            state = energy['states'][i]
            out.append((ENERGY_TYPE, row['model']) if state == 'signal_present'
                       else ('not_a_drone', 'chamber_noise') if state == 'source_noise' else None)
        elif decision['state'] == 'signal_present' and row['signal_type'] in ('dji_ofdm', 'lightbridge'):
            out.append((row['signal_type'], row['model']))
        elif decision['state'] == 'background':
            out.append(('not_a_drone', 'dronerfa_background' if row['dataset'] == 'DroneRFa' else 'x310_background'))
        else:
            out.append(None)
    return out


def build(store, presence, overlay_path, classes_path, output, presence_filter=True):
    store, presence = Path(store), Path(presence)
    overlay = read_json(overlay_path)['labels']
    multi = {c['model'] for c in read_json(classes_path)['classes'] if c['reporting_flag'] == 'multi_aircraft'}
    windows, per = [], defaultdict(Counter)
    groups = defaultdict(lambda: defaultdict(set))       # model -> partition -> identities
    for path in sorted(store.glob('*.json')):
        if len(path.stem) != 24:
            continue
        row = read_json(path)['source_record']
        if row['model'] in DROPPED_MODELS:
            continue
        labels = read_json(presence / (path.stem + '.json'))['labels']
        part = None
        for index, target in enumerate(targets(row, labels, overlay, presence_filter)):
            if target is None:
                continue
            part = part or partition(row, multi)
            windows.append(dict(recording_id=path.stem, view_index=index, type=target[0], model=target[1],
                                partition=part, dataset=row['dataset'], aircraft=row.get('aircraft')))
            per[part][target[0] + ' / ' + target[1]] += 1
        if part and row['model'] != 'not_a_drone':
            identity = row.get('aircraft') if row['model'] in multi and part in ('train', 'val', 'test') else row['source'] + '|' + str(row.get('member'))
            groups[row['model']][part].add(identity)
    check(windows, groups, multi)
    report = dict(schema='rfmon.drone_ml.universal_splits.v1', version=VERSION, open_models=sorted(OPEN_MODELS),
                  dronerfa=dict(distance=DRONERFA_DISTANCE, indoor=DRONERFA_INDOOR),
                  session_partition=SESSION_PARTITION, presence_filter=presence_filter,
                  dropped_models=sorted(DROPPED_MODELS), home_source={k: sorted(v) for k, v in HOME_SOURCE.items()}, multi_aircraft=sorted(multi),
                  windows=len(windows), per_partition={k: dict(sorted(v.items())) for k, v in sorted(per.items())},
                  note='unit_test is recordings of the training unit; only test supports an unseen-aircraft claim',
                  rows=windows)
    Path(output).write_text(json.dumps(report) + '\n')
    return report


def check(windows, groups, multi):
    """No aircraft (multi-aircraft classes) or recording (other classes) on both sides."""
    for model, parts in groups.items():
        fit = parts.get('train', set()) | parts.get('val', set())
        for held in ('test', 'unit_test', 'cross_test', 'open_test'):
            if fit & parts.get(held, set()):
                raise ValueError(f'{model}: identity in both fitting data and {held}')
    by_record = defaultdict(set)
    for w in windows:
        by_record[w['recording_id']].add(w['partition'])
    if any(len(v) > 1 for v in by_record.values()):
        raise ValueError('a recording is split across partitions')
    trained = {w['model'] for w in windows if w['partition'] == 'train'}
    if trained & OPEN_MODELS:
        raise ValueError('an open-test model is in training')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('store', 'presence', 'overlay', 'classes', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--no-presence-filter', action='store_true', help='C3 ablation: label every window of a drone recording')
    a = p.parse_args()
    r = build(a.store, a.presence, a.overlay, a.classes, a.output, not a.no_presence_filter)
    print(json.dumps({k: v for k, v in r.items() if k != 'rows'}, indent=1))


if __name__ == '__main__':
    main()
