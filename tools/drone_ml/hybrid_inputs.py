"""Prepare aligned, causal STFT/DSP research views from replay-derived IQ.

No labels or trained checkpoint are produced. Frequency views overlap and are
correlated derivatives of their parent recording, never independent examples.
"""
import argparse
import math
import time
from pathlib import Path

import numpy as np
from scipy.signal import firwin, upfirdn

from .common import read_source, sha256, stamp, new_output, write_json
from .recognition_review import load_json
from .integration_replay import Worker
from .preprocess import STFTConfig, spectrogram

RATE = 20000000
FEATURES = ('o4_cp_correlation', 'o4_lag_prominence', 'o4_cyclic_correlation',
            'o4_cyclic_prominence', 'o4_harmonic_correlation', 'clipped_fraction',
            'zc_correlation', 'occupied_bandwidth_fraction', 'burst_duty_cycle')
O4_KEYS = ('cp_correlation', 'lag_prominence', 'cyclic_correlation', 'cyclic_prominence', 'harmonic_correlation')
TILE = 160000


def frequency_views(rate):
    if type(rate) is not int or rate % RATE or not RATE <= rate <= 200000000:
        raise ValueError('integer decimation from 20..200 MSps required')
    if rate == RATE: return [0.]
    extent = .45*rate-RATE/2
    count = math.ceil(2*extent/16000000)+1
    if count % 2 == 0: count += 1
    return np.linspace(-extent, extent, count).tolist()


def channelize(iq, rate, offset):
    """Causal finite-input view; the caller must handle startup filter support."""
    frequency_views(rate)
    if not math.isfinite(offset) or (rate != RATE and abs(offset)+RATE/2 > .45*rate+.01):
        raise ValueError('frequency view outside guard band')
    if rate == RATE and offset != 0:
        raise ValueError('native-rate view must be centered')
    if iq.ndim != 1 or not 0 < len(iq) <= 40000000 or not np.isfinite(iq).all():
        raise ValueError('bounded finite IQ required')
    factor = rate//RATE
    kernel = np.ones(1, dtype=np.float32) if factor == 1 else firwin(
        20*factor+1, 1/factor, window=('kaiser', 8.6)).astype(np.float32)
    taps = (kernel*np.exp(2j*np.pi*offset*np.arange(len(kernel))/rate)).astype(np.complex64)
    filtered = upfirdn(taps, iq, down=factor)[:len(iq)//factor]
    if offset:
        filtered *= np.exp(-2j*np.pi*offset*np.arange(len(filtered))/RATE).astype(np.complex64)
    return np.asarray(filtered, dtype='<c8'), len(kernel)


def causal_view(iq, rate, offset, tiles):
    """Causal FIR downconversion/decimation with 8 ms real pre-roll.

    Group delay is retained, not removed using future samples. No filter tail
    or zero padding enters the returned context. This is a new preprocessing
    version; old weights must not silently consume it.
    """
    frequency_views(rate)
    if type(tiles) is not int or not 2 <= tiles <= 32 or tiles % 2 or not math.isfinite(offset):
        raise ValueError('unsupported context')
    factor = rate//RATE
    used = (tiles+1)*TILE*factor
    if len(iq) < used or not np.isfinite(iq[:used]).all():
        raise ValueError('short/nonfinite context; padding forbidden')
    # Mix the FIR taps instead of allocating an oscillator at the native rate.
    filtered, taps_count = channelize(iq[:used], rate, offset)
    result = np.asarray(filtered[TILE:(tiles+1)*TILE], dtype='<c8')
    return result, dict(schema='rfmon.drone_ml.causal_view.v1', native_sample_rate_hz=rate,
        view_sample_rate_hz=RATE, offset_hz=offset, source_start_sample=TILE*factor,
        native_samples_per_tile=TILE*factor, tiles=tiles, context_seconds=tiles*.008,
        fir_taps=taps_count, fir_kaiser_beta=8.6, fir_group_delay_native_samples=(taps_count-1)//2,
        future_samples_required=0, original_preroll_samples=TILE*factor,
        filter_group_delay_retained=True, compatible_with_existing_live_checkpoint=False)


def align_features(events, tiles):
    """Join only completed 16 ms windows to their ending 8 ms tile.

    Earlier/odd tiles retain missing masks. No future window is copied back,
    and no feature from another frequency view or continuity epoch is admitted.
    Caller obtains these events exclusively from a fresh worker for this view.
    """
    features = np.zeros((tiles, len(FEATURES)), dtype=np.float32)
    available = np.zeros_like(features, dtype=bool)
    seen = set()
    for e in events:
        start, count = e.get('start_sample'), e.get('sample_count')
        if (e.get('profile') != 'o4_structure_v2' or e.get('continuity_epoch') != 1
                or type(start) is not int or start < 0 or start % (2*TILE)
                or count != 2*TILE or start+count > tiles*TILE):
            raise ValueError('misaligned, foreign-epoch or out-of-context DSP evidence')
        index = (start+count)//TILE-1
        if index in seen: raise ValueError('duplicate DSP window')
        seen.add(index)
        values = np.asarray([e[k] for k in O4_KEYS], dtype=np.float64)
        if not np.isfinite(values).all() or (values < 0).any() or (values[[0,2,4]] > 1.00001).any():
            raise ValueError('invalid measured DSP values')
        features[index, :5] = values; available[index, :5] = True
    return features, available


def prepare_case(case, output, worker_path, all_views=True):
    if case.get('partition') != 'train' or not case.get('collection_group'):
        raise ValueError('reserved or ungrouped derivative rejected before opening')
    prefix = case['model_prefix']; path = Path(prefix['path']); before = stamp(path)
    if sha256(path) != prefix['sha256'] or before[2] != prefix['samples']*8:
        raise ValueError('derived IQ hash/size mismatch')
    with read_source(path) as f: iq = np.frombuffer(f.read(), dtype='<c8')
    rate = int(case['sample_rate_hz']); factor = rate//RATE
    tiles = min(32, (len(iq)//(TILE*factor)-1)//2*2)
    offsets = frequency_views(rate) if all_views else [0.]
    output.mkdir()
    rows = []
    for index, offset in enumerate(offsets):
        started = time.monotonic()
        view, meta = causal_view(iq, rate, offset, tiles)
        images = np.zeros((tiles, 256, 625), dtype=np.float32)
        valid = np.ones(tiles, dtype=bool)
        for t in range(tiles):
            try:
                images[t] = spectrogram(view[t*TILE:(t+1)*TILE], RATE,
                                        STFTConfig(duration_seconds=.008, hop=256))
            except ValueError as error:
                if 'zero or constant signal' not in str(error): raise
                valid[t] = False
        worker = Worker([str(worker_path)], output/f'view_{index:02d}.stderr.log')
        evidence = []; dsp_processing_ms = 0.
        try:
            for at in range(0, len(view), 20*TILE):
                r = worker.request(view[at:at+20*TILE], RATE, case['center_hz']+offset, at)
                if 'error' in r: raise ValueError('DSP worker: '+r['error'])
                evidence.extend(r['o4_evidence']); dsp_processing_ms += r['processing_ms']
        finally:
            worker.close()
        features, masks = align_features(evidence, tiles)
        # ADC clipping is measured on original signed-int16 samples, not on the
        # filtered view (whose amplitude can overshoot without ADC clipping).
        if case['source_provenance']['scaling'] == 'signed_int16_divided_by_32768':
            for end in range(2, tiles+1, 2):
                x = iq[(end-1)*TILE*factor:(end+1)*TILE*factor]
                features[end-1, 5] = np.mean((x.real <= -1) | (x.real >= 32767/32768) |
                                             (x.imag <= -1) | (x.imag >= 32767/32768))
                masks[end-1, 5] = True
        for end in range(2, tiles+1, 2):
            if not valid[end-2:end].all():
                features[end-1] = 0; masks[end-1] = False
        target = output/f'view_{index:02d}.npz'
        starts = np.zeros(tiles, dtype=bool); starts[0] = True
        np.savez(target, tiles=images[:, None], features=features, available=masks,
                 valid=valid, segment_start=starts)
        rows.append(dict(path=str(target), sha256=sha256(target), tensor_shape=list(images[:, None].shape),
            preprocessing=meta, features=list(FEATURES), available_measurements=int(masks.sum()),
            feature_windows=len(evidence), o4_candidate_windows=sum(e['candidate'] for e in evidence),
            valid_tiles=int(valid.sum()), dsp_worker_processing_ms=dsp_processing_ms,
            preparation_wall_ms=(time.monotonic()-started)*1000,
            coverage='overlapping 20 MSps views; nominal 16 MHz interiors; source edges excluded',
            presence_label=None, identity_label=None, training_eligible=False))
        write_json(output/f'view_{index:02d}.dsp.json', evidence)
    if stamp(path) != before: raise ValueError('derived source changed during preparation')
    manifest = dict(schema='rfmon.drone_ml.hybrid_inputs.v1', source_case=case,
        source_sha256=prefix['sha256'], views=rows, extractor_sha256=sha256(__file__),
        native_worker_sha256=sha256(worker_path), supervised_labels_created=False, training_started=False,
        qualified=False, limitations=['Overlapping views share one parent group and are not independent trials.',
            'Publisher context is not a per-tile identity label.', 'ZC/bandwidth/duty features remain missing.',
            'Fresh per-view worker provides features without cross-view state; live scheduling/join is still pending.',
            'Original ADC clipping is wideband receiver quality, not proof that the selected emitter clipped.'])
    write_json(output/'manifest.json', manifest)
    return manifest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--replay', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--worker', type=Path, required=True); p.add_argument('--center-only', action='store_true')
    a = p.parse_args(); cases = load_json(a.replay)
    output = new_output(a.output, [a.replay.parent]).resolve(); results = []
    for index, case in enumerate(cases):
        if 'model_prefix' not in case: raise ValueError('replay derivative missing; resolve before preparation')
        write_json(output/'progress.json', dict(phase='preparing', completed=index, total=len(cases), current=case['context_label']))
        manifest = prepare_case(case, output/f'case_{index:02d}', a.worker.resolve(), not a.center_only)
        results.append(dict(context_label=case['context_label'], collection_group=case['collection_group'],
                            manifest=str(output/f'case_{index:02d}/manifest.json'), views=len(manifest['views'])))
        write_json(output/'index.json', results)
        print(case['context_label'], len(manifest['views']), 'views', flush=True)
    write_json(output/'progress.json', dict(phase='complete', completed=len(cases), total=len(cases), training_started=False))


if __name__ == '__main__':
    main()
