"""Numerical structured-RF activity proxy; never drone/source ground truth."""
import numpy as np

POLICY = dict(version=1, block_frames=8, contrast_db=6.0, minimum_structured_fraction=.1,
              core_fraction=.75, interpretation='structured_rf_proxy_not_drone_truth')


def assess(image, policy=POLICY):
    image = np.asarray(image)
    if image.ndim != 2 or min(image.shape) < 8 or not np.isfinite(image).all():
        raise ValueError('activity requires a finite frequency/time image')
    if policy != POLICY:
        raise ValueError('unsupported activity policy')
    margin = image.shape[0] // 8
    power = np.power(10.0, image[margin:-margin].astype(np.float64) / 10)
    blocks = power.shape[1] // 8
    if blocks < 1:
        raise ValueError('insufficient temporal support for activity')
    block_power = power[:, :blocks * 8].reshape(power.shape[0], blocks, 8).mean(2)
    db = 10 * np.log10(np.maximum(block_power, 1e-16))
    contrast = np.quantile(db, .95, axis=0) - np.quantile(db, .5, axis=0)
    energy = block_power.mean(0)
    temporal_db = 10 * np.log10(np.maximum(energy, 1e-16) / max(float(np.quantile(energy, .1)), 1e-16))
    structured = float(np.mean((contrast >= policy['contrast_db']) | (temporal_db >= policy['contrast_db'])))
    flatness = np.exp(np.mean(np.log(np.maximum(block_power, 1e-16)), axis=0)) / np.maximum(block_power.mean(0), 1e-16)
    return dict(eligible=structured >= policy['minimum_structured_fraction'],
                structured_fraction=structured, median_contrast_db=float(np.median(contrast)),
                peak_contrast_db=float(np.max(contrast)), mean_spectral_flatness=float(np.mean(flatness)),
                temporal_contrast_db=float(np.quantile(temporal_db, .95)),
                blocks=blocks, interpretation=policy['interpretation'])


def rank(activity):
    return activity['structured_fraction'], activity['median_contrast_db']
