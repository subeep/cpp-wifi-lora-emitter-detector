"""Energy-based presence labels for non-OFDM classes recorded in an anechoic chamber.

User decision of 2026-11-13: "KU Leuven now, rest later". The KU Leuven recordings were made in
a semi-anechoic chamber (dataset README), and the reviewed windows show only the device's own
narrow bursts and noise, so a burst above the window's own quiet level is taken to be the
device. This rule is NOT valid for RFUAV or Tampere, whose recordings hold other traffic; those
classes stay unknown.

It is an overlay on the B2 labels: nothing in `universal_presence` output is rewritten. It reads
the cached measured facts only (no IQ) and writes one file beside them.

States written per window:
  signal_present  - at least one burst over 6 dB above the window's quiet decile (duty cycle > 0)
  source_noise    - no burst and a flat spectrum: chamber noise, usable as an easy negative
  unknown         - anything else (for example a continuous signal with no quiet part)
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import numpy as np
from .universal_features import NAMES
from .universal_inventory import read_json

SOURCE = 'KULeuven_DroneRF'
# Non-OFDM KU Leuven classes. SJRC F11 Pro is typed wifi_drone in classes.json, but its 2.44 GHz
# recordings show the hopping control link (visual review, 13 November), so it is labelled here.
MODELS = {'Spektrum DX4e', 'Taranis Q X7', 'Nine Eagles', 'WLtoys', 'Q205', 'SJRC F11 Pro'}
FLAT_NOISE = .75   # noise-only windows measured 0.80 (with the receiver's spur line) to 0.98
DUTY, FLATNESS = NAMES.index('duty_cycle'), NAMES.index('spectral_flatness')
VERSION = 'universal_presence_energy_v1'


def decide(values):
    """State for one window from its measured-fact vector."""
    if not np.isfinite(values[DUTY]) or not np.isfinite(values[FLATNESS]):
        return 'unknown'
    if values[DUTY] > 0:
        return 'signal_present'
    return 'source_noise' if values[FLATNESS] >= FLAT_NOISE else 'unknown'


def build(store, presence, output):
    store, presence = Path(store), Path(presence)
    labels, counts, missing = {}, defaultdict(Counter), []
    for path in sorted(store.glob('*.json')):
        if len(path.stem) != 24:
            continue
        row = read_json(path)['source_record']
        if row['dataset'] != SOURCE or row['model'] not in MODELS:
            continue
        facts = presence / (path.stem + '.npz')
        if not facts.exists():
            missing.append(path.stem)
            continue
        values = np.load(facts, allow_pickle=False)['values']
        states = [decide(v) for v in values]
        labels[path.stem] = dict(model=row['model'], states=states)
        counts[row['model']].update(states)
    report = dict(schema='rfmon.drone_ml.universal_presence_energy.v1', version=VERSION, source=SOURCE,
                  rule='duty_cycle > 0 -> signal_present; else spectral_flatness >= %.2f -> source_noise; else unknown' % FLAT_NOISE,
                  basis='anechoic recording; user decision 2026-11-13; not valid for other sources',
                  models=sorted(MODELS), records=len(labels), records_without_features=missing,
                  by_class={k: dict(v) for k, v in sorted(counts.items())}, labels=labels)
    Path(output).write_text(json.dumps(report, indent=1) + '\n')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--store', type=Path, required=True); p.add_argument('--presence', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True); a = p.parse_args()
    r = build(a.store, a.presence, a.output)
    print(json.dumps({k: v for k, v in r.items() if k != 'labels'}))


if __name__ == '__main__':
    main()
