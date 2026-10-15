"""Strict adapter for contiguous numerical FFT sequences, never display spectra."""
import json
import math
import numpy as np

from .common import read_source, stamp

MAX_ELEMENTS = 4_000_000


def spectrogram_from_fft(frames, metadata, config):
    """Convert a producer-declared compatible sequence to the trained tensor.

    Metadata is an explicit producer contract, not proof that hardware applied it.
    Source IQ mean/RMS conditioning and frequency-view filtering must precede FFTs.
    Power frames contain squared FFT magnitude, before Hann energy normalization.
    """
    config.validate()
    if (metadata.get('schema') != 'rfmon.drone_ml.fft.v1'
            or metadata.get('preprocessing') != config.metadata()
            or metadata.get('input_conditioning') != 'source_mean_removed_unit_rms_before_view_v1'):
        raise ValueError('FFT preprocessing does not match checkpoint contract')
    rate = metadata.get('source_rate_hz')
    offset = metadata.get('selected_offset_hz')
    if (type(rate) is not int or not config.rate_hz <= rate <= 1_000_000_000
            or type(offset) not in (int, float) or not math.isfinite(offset)
            or round(rate * config.duration_seconds) > 4_000_000):
        raise ValueError('invalid FFT source rate or frequency view')
    if config.auto_frequency_view and rate != config.rate_hz:
        extent = .45 * rate - config.rate_hz / 2
        if extent < 0 or not any(math.isclose(offset, x, abs_tol=1e-6, rel_tol=0)
                                 for x in np.linspace(-extent, extent, 5)):
            raise ValueError('FFT view incompatible with automatic selector')
    elif offset != config.offset_hz:
        raise ValueError('FFT view offset does not match checkpoint')
    if not (rate == config.rate_hz and offset == 0) and abs(offset) + config.rate_hz / 2 > .45 * rate:
        raise ValueError('FFT view crosses guarded source band')
    target = round(config.rate_hz * config.duration_seconds)
    if ((target - config.nfft) // config.hop + 1) * config.nfft > MAX_ELEMENTS:
        raise ValueError('FFT sequence exceeds element budget')
    starts = list(range(0, target - config.nfft + 1, config.hop))
    supplied = metadata.get('frame_start_samples')
    origin = metadata.get('interval_start_sample')
    if (type(origin) is not int or origin < 0 or type(supplied) is not list
            or any(type(x) is not int for x in supplied) or supplied != starts
            or type(metadata.get('dropped_samples')) is not int or metadata['dropped_samples'] != 0):
        raise ValueError('FFT frames must be ordered, contiguous and gap-free')
    frames = np.asarray(frames)
    representation = metadata.get('representation')
    kind = 'c' if representation == 'complex_fft' else 'f' if representation == 'power_fft' else None
    if (kind is None or frames.dtype.kind != kind or frames.shape != (len(starts), config.nfft)
            or frames.size > MAX_ELEMENTS or not np.isfinite(frames).all()):
        raise ValueError('invalid FFT representation, shape, size or values')
    order = metadata.get('bin_order')
    if order not in ('fftshift', 'natural'):
        raise ValueError('unsupported FFT bin ordering')
    scale = metadata.get('forward_scale')
    if type(scale) not in (int, float) or not math.isfinite(scale) or not 1e-12 <= scale <= 1e12:
        raise ValueError('invalid FFT forward scale')
    # Convert to double before arithmetic; input may have finite float32 extremes.
    with np.errstate(over='ignore', invalid='ignore'):
        if kind == 'c':
            power = np.abs(frames.astype(np.complex128) / scale) ** 2
        else:
            if np.any(frames < 0):
                raise ValueError('FFT power must be nonnegative')
            power = frames.astype(np.float64) / scale ** 2
    if not np.isfinite(power).all() or not np.any(power > 0):
        raise ValueError('FFT power is nonfinite or has insufficient evidence')
    if order == 'natural':
        power = np.fft.fftshift(power, axes=1)
    power /= np.sum(np.hanning(config.nfft + 1)[:-1] ** 2)
    image = 10 * np.log10(np.maximum(power, 10 ** (config.floor_db / 10)))
    return image.T.astype(np.float32)


def read_fft(path, metadata_path):
    """Read bounded NPY v1/v2 numeric frames and a JSON sidecar without pickle."""
    before = [stamp(path), stamp(metadata_path)]
    if before[0][2] > MAX_ELEMENTS * 16 + 65536 or before[1][2] > 2_000_000:
        raise ValueError('FFT input file exceeds budget')
    with read_source(metadata_path) as f:
        metadata = json.loads(f.read(2_000_001))
    if not isinstance(metadata, dict):
        raise ValueError('FFT metadata must be an object')
    with read_source(path) as f:
        version = np.lib.format.read_magic(f)
        readers = {(1, 0): np.lib.format.read_array_header_1_0,
                   (2, 0): np.lib.format.read_array_header_2_0}
        if version not in readers:
            raise ValueError('unsupported NPY version')
        shape, fortran, dtype = readers[version](f)
        if (len(shape) != 2 or any(n <= 0 for n in shape) or math.prod(shape) > MAX_ELEMENTS
                or fortran or dtype.kind not in 'fc' or dtype.itemsize > 16
                or f.tell() + math.prod(shape) * dtype.itemsize != before[0][2]):
            raise ValueError('invalid or over-budget FFT NPY array')
        f.seek(0)
        frames = np.load(f, allow_pickle=False)
    if [stamp(path), stamp(metadata_path)] != before:
        raise ValueError('FFT source changed during read')
    return frames, metadata
