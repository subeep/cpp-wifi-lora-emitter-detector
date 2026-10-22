"""Physical-scale complex IQ -> two-sided numerical log-power STFT."""
from dataclasses import asdict, dataclass
from fractions import Fraction
import math
import numpy as np
from scipy.signal import resample_poly


@dataclass(frozen=True)
class STFTConfig:
    rate_hz: int = 20_000_000
    duration_seconds: float = .001
    nfft: int = 256
    hop: int = 128
    offset_hz: float = 0.0
    floor_db: float = -100.0
    auto_frequency_view: bool = False

    def validate(self):
        if (type(self.rate_hz) is not int or not 100_000 <= self.rate_hz <= 200_000_000
                or not math.isfinite(self.duration_seconds) or not .0001 <= self.duration_seconds <= .02
                or type(self.nfft) is not int or not 64 <= self.nfft <= 2048
                or type(self.hop) is not int or not 1 <= self.hop <= self.nfft
                or not math.isfinite(self.offset_hz) or not -160 <= self.floor_db <= -20
                or type(self.auto_frequency_view) is not bool
                or (self.auto_frequency_view and self.offset_hz != 0)
                or round(self.duration_seconds * self.rate_hz) < self.nfft):
            raise ValueError('invalid or over-budget STFT configuration')

    def metadata(self):
        self.validate()
        return dict(**asdict(self), frequency_bin_hz=self.rate_hz / self.nfft,
                    time_hop_seconds=self.hop / self.rate_hz, window='periodic_hann',
                    scaling='mean_removed_unit_rms_log_power', spectrum='two_sided_fftshift',
                    preprocessing_version=2 if self.auto_frequency_view else 1,
                    context='single_contiguous_interval')


def from_metadata(metadata):
    return STFTConfig(**{k: metadata.get(k, f.default) for k, f in STFTConfig.__dataclass_fields__.items()})


def select_offset(iq, rate, config):
    if not config.auto_frequency_view or config.rate_hz == rate:
        return config.offset_hz
    extent = .45 * rate - config.rate_hz / 2
    if extent < 0:
        raise ValueError('source too narrow for automatic guarded view')
    nfft = min(4096, len(iq))
    frames = np.asarray(iq[:len(iq) // nfft * nfft], dtype=np.complex128).reshape(-1, nfft)
    frames = frames - np.mean(frames)
    power = np.mean(np.abs(np.fft.fftshift(np.fft.fft(frames * np.hanning(nfft), axis=1), axes=1)) ** 2, axis=0)
    frequencies = np.fft.fftshift(np.fft.fftfreq(nfft, 1 / rate))
    offsets = np.linspace(-extent, extent, 5)
    scores = [np.sum(power[np.abs(frequencies - offset) <= config.rate_hz * .4]) for offset in offsets]
    return float(offsets[int(np.argmax(scores))])


def fft_frames(iq, source_rate, config=STFTConfig()):
    """Reference producer for compatible FFT sequences; rows are time frames."""
    config.validate()
    if (not math.isfinite(source_rate) or source_rate != int(source_rate)
            or not 0 < source_rate <= 1_000_000_000 or config.rate_hz > source_rate):
        raise ValueError('source rate must be valid; upsampling unavailable')
    native_view = config.offset_hz == 0 and config.rate_hz == source_rate
    if not native_view and abs(config.offset_hz) + config.rate_hz / 2 > .45 * source_rate:
        raise ValueError('requested frequency view crosses guarded source band')
    iq = np.asarray(iq)
    count = round(source_rate * config.duration_seconds)
    if iq.ndim != 1 or iq.size != count or count > 4_000_000 or not np.isfinite(iq).all():
        raise ValueError('IQ must be finite and exactly one bounded contiguous interval')
    # Float64 accumulation avoids overflow in RMS and long oscillator phase.
    iq = iq.astype(np.complex128)
    iq -= np.mean(iq)
    rms = float(np.sqrt(np.mean(np.abs(iq) ** 2)))
    if not math.isfinite(rms) or rms < 1e-12:
        raise ValueError('zero or constant signal has insufficient evidence')
    iq /= rms
    offset_hz = select_offset(iq, source_rate, config)
    if offset_hz:
        iq *= np.exp(-2j * np.pi * offset_hz * np.arange(count) / source_rate)
    ratio = Fraction(config.rate_hz, int(source_rate))
    if max(ratio.numerator, ratio.denominator) > 2000:
        raise ValueError('resampling ratio exceeds filter budget')
    iq = resample_poly(iq, ratio.numerator, ratio.denominator, window=('kaiser', 8.6))
    target = round(config.rate_hz * config.duration_seconds)
    if iq.size < target:
        raise ValueError('short resampled interval')
    if ((target - config.nfft) // config.hop + 1) * config.nfft > 4_000_000:
        raise ValueError('FFT sequence exceeds element budget')
    frames = np.lib.stride_tricks.sliding_window_view(iq[:target], config.nfft)[::config.hop]
    hann = np.hanning(config.nfft + 1)[:-1]
    fft = np.fft.fftshift(np.fft.fft(frames * hann, axis=1), axes=1)
    metadata = dict(schema='rfmon.drone_ml.fft.v1', preprocessing=config.metadata(),
                    representation='complex_fft', bin_order='fftshift', forward_scale=1.0,
                    input_conditioning='source_mean_removed_unit_rms_before_view_v1',
                    source_rate_hz=int(source_rate), selected_offset_hz=offset_hz,
                    frame_start_samples=list(range(0, target - config.nfft + 1, config.hop)),
                    interval_start_sample=0, dropped_samples=0)
    return fft, metadata


def spectrogram(iq, source_rate, config=STFTConfig(), return_metadata=False):
    fft, metadata = fft_frames(iq, source_rate, config)
    hann = np.hanning(config.nfft + 1)[:-1]
    power = np.abs(fft) ** 2 / np.sum(hann ** 2)
    image = np.maximum(10 * np.log10(np.maximum(power, 10 ** (config.floor_db / 10))), config.floor_db)
    if not np.isfinite(image).all():
        raise ValueError('nonfinite STFT')
    # No colour maps, axes, labels, image resize or frequency/time warping.
    image = image.T.astype(np.float32)
    return (image, metadata) if return_metadata else image
