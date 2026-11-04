"""Bounded, observation-only RF triage; waveform shape is not emitter identity.

Runs in the optional ML worker, never in acquisition. All finite, varying IQ
continues to the unchanged model. Review flags are hypotheses for labelled
replay, NOT non-drone decisions. No learned/qualified rejection rules exist.
"""
import math
import time

import numpy as np

VERSION = 'rfmon.ml.dsp_triage.v1'
NFFT = 1024
MAX_FRAMES = 32
MAX_SAMPLES = 4_000_000


def assess(iq, rate):
    started = time.perf_counter()
    out = dict(schema=VERSION, mode='observe', route='model',
               assessment='unresolved_activity', review_flag='none',
               reason='waveform_does_not_establish_emitter_identity',
               non_drone_rejection_enabled=False, thresholds_validated=False,
               protocol_evidence='not_associated', metrics={})

    def finish():
        out['processing_ms'] = (time.perf_counter() - started) * 1000
        return out

    x = np.asarray(iq)
    if (x.ndim != 1 or not 1 <= x.size <= MAX_SAMPLES or
            not np.iscomplexobj(x) or not math.isfinite(rate) or rate <= 0):
        raise ValueError('unsupported DSP triage input')
    out.update(samples=int(x.size), sample_rate_hz=float(rate))
    # Full interval checks: strided statistics cannot certify sample validity.
    if not np.isfinite(x).all():
        out.update(route='insufficient_evidence', assessment='invalid_samples',
                   reason='nonfinite_iq')
        return finish()
    if not np.any(x != x[0]):
        out.update(route='insufficient_evidence', assessment='constant_samples',
                   reason='constant_iq_has_no_modulation_evidence')
        return finish()
    if x.size < NFFT:
        out['reason'] = 'too_short_for_triage_features_model_still_required'
        return finish()

    count = min(MAX_FRAMES, x.size // NFFT)
    starts = np.linspace(0, x.size - NFFT, count, dtype=np.int64)
    frames = np.array([x[s:s + NFFT] for s in starts], dtype=np.complex128)
    # Normalize only a private measurement copy; model receives original IQ.
    scale = float(np.max(np.abs(frames)))
    out['metrics']['sampled_peak_magnitude'] = scale
    out.update(nfft=NFFT, frames=count, measured_samples=count * NFFT,
               frame_offsets_samples=starts.tolist(), frequency_bin_hz=rate / NFFT,
               measurement_scope='disjoint_frames_in_submitted_interval')
    if scale == 0:
        out['reason'] = 'sampled_frames_empty_other_samples_vary_model_required'
        return finish()
    frames /= scale
    power = np.abs(frames) ** 2
    psd = np.mean(np.abs(np.fft.fft(frames * np.hanning(NFFT), axis=1)) ** 2, axis=0)
    total = float(psd.sum())
    if total <= 0:
        return finish()
    flatness = float(np.exp(np.mean(np.log(np.maximum(psd, np.finfo(float).tiny)))) / np.mean(psd))
    peak = int(np.argmax(psd))
    peak_three = float(psd[[(peak - 1) % NFFT, peak, (peak + 1) % NFFT]].sum() / total)
    left, right = frames[:, :-1], frames[:, 1:]
    denominator = float(np.sqrt(np.sum(np.abs(left) ** 2) * np.sum(np.abs(right) ** 2)))
    coherence = float(abs(np.sum(np.conj(left) * right)) / denominator) if denominator else 0.
    crest = float(power.max() / power.mean())
    out['metrics'].update(spectral_flatness=flatness, peak_three_bin_fraction=peak_three,
                          lag_one_coherence=coherence, peak_to_mean_power=crest)
    # Deliberately permissive review hypotheses, never suppressive decisions.
    # Wideband/drone mixtures can satisfy either: they MUST still reach ML.
    if peak_three > .8 and coherence > .9:
        out['review_flag'] = 'tone_like_review_only'
    elif flatness > .85 and coherence < .1 and crest < 15:
        out['review_flag'] = 'noise_like_review_only'
    return finish()


def predict_with_triage(engine, iq, rate, offset=0, *, include_stft=False, enabled=True):
    if not enabled:
        return engine.predict_iq(iq, rate, offset, include_stft=include_stft)
    try:
        triage = assess(iq, rate)
    except Exception as exc:
        # Optional diagnostics must not suppress otherwise usable observations.
        triage = dict(schema=VERSION, mode='observe', route='model',
                      assessment='measurement_failed', review_flag='none',
                      reason='triage_failed_open', error=type(exc).__name__,
                      non_drone_rejection_enabled=False, thresholds_validated=False,
                      processing_ms=0., metrics={})
    if triage['route'] == 'insufficient_evidence':
        result = dict(status='insufficient_evidence', reason=triage['reason'],
                      accepted_class=None)
        triage['model_invoked'] = False
    else:
        result = engine.predict_iq(iq, rate, offset, include_stft=include_stft)
        triage['model_invoked'] = True  # Engine may itself reject preprocessing.
    result['dsp_triage'] = triage
    return result
