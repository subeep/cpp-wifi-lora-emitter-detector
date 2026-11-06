"""Score frozen carrier templates on later anchors in the same recording."""
import argparse
import json
from collections import defaultdict
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly


def extract(path, anchor, entries, manifest, offsets=(4, 5), complex_grid=False):
    n = manifest["fft_size"]
    cp = manifest["prefix_samples"]
    period = manifest["period_samples"]
    # Match the manifest producer's input padding and rational resampler phase.
    begin = int(np.floor(anchor + (min(manifest["offsets"]) * period - cp) * 75 / 56)) - 3128
    starts = {k: entries[k]["fft_start_sample_448"] for k in (0, *offsets)}
    count = int(np.ceil((max(starts.values()) + n + 64) * 75 / 56))
    if begin < 0 or round((anchor - begin) * 56 / 75) != starts[0]:
        raise ValueError("Anchor and local FFT coordinates disagree")
    with path.open("rb") as stream:
        stream.seek(begin * 8)
        raw = np.fromfile(stream, "<c8", count=count)
    if len(raw) != count or not np.isfinite(raw).all():
        raise ValueError("Incomplete IQ window")
    x = resample_poly(raw, 56, 75)
    i = starts[0]
    omega = np.angle(np.vdot(x[i-cp:i], x[i+n-cp:i+n])) / n
    bins = np.asarray(manifest["carrier_bins"]) + n // 2
    grid = {}
    for offset, start in starts.items():
        index = start + np.arange(n)
        grid[offset] = np.fft.fftshift(np.fft.fft(x[index] * np.exp(-1j * omega * index)))[bins]
    if complex_grid:
        return [grid[k] * grid[0].conj() for k in offsets]
    return [np.angle(grid[k] * grid[0].conj()) for k in offsets]


def score(phase, template, pilots, targets):
    residual = phase - template
    rotation = np.angle(np.exp(1j * residual[:, pilots]).mean(axis=1))
    return (np.abs(np.angle(np.exp(1j * (residual[:, targets] - rotation[:, None])))) < np.pi / 4).mean(axis=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--cross-recording", action="store_true", help="Score each source template on later anchors from other files")
    args = parser.parse_args()
    m = json.loads(args.manifest.read_text())
    if (m["source_sample_rate_hz"], m["sample_rate_hz"]) != (60000000, 44800000):
        raise ValueError("Unexpected sampling grid")
    groups = defaultdict(lambda: defaultdict(dict))
    for e in m["entries"]:
        if e["passes_gate"]:
            groups[e["path"]][e["source_offset_sample"]][e["offset"]] = e
    recordings = {}
    for path, anchors in sorted(groups.items()):
        ids = sorted(a for a, entries in anchors.items() if {0, 4, 5} <= entries.keys())
        if len(ids) < 6:
            continue
        phase = np.array([extract(Path(path), a, anchors[a], m) for a in ids])
        recordings[path] = (ids, phase)
    for path, (ids, phase) in recordings.items():
        split = len(ids) // 2
        resultant = np.exp(1j * phase[:split, 0]).mean(axis=0)
        ranked = np.argsort(-abs(resultant))[:128]
        pilots, targets = ranked[::2], ranked[1::2]
        template = np.angle(resultant)
        shuffled = template.copy()
        shuffled[targets] = np.random.default_rng(42).permutation(template[targets])
        for destination, (destination_ids, observations) in recordings.items():
            if (destination != path) != args.cross_recording:
                continue
            first = len(destination_ids) // 2
            later = observations[first:]
            scores = {
                "offset4": score(later[:, 0], template, pilots, targets),
                "offset5": score(later[:, 1], template, pilots, targets),
                "permuted_target_template": score(later[:, 0], shuffled, pilots, targets),
            }
            print(json.dumps(dict(path=path, training=ids[:split], destination=destination, held_out=destination_ids[first:],
                                  scores={k: v.tolist() for k, v in scores.items()},
                                  mean={k: float(v.mean()) for k, v in scores.items()})))



if __name__ == "__main__":
    main()
