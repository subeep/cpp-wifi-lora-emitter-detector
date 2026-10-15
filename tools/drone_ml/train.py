"""CPU-first offline training; development smoke cannot claim unknown validation."""
import argparse
import json
import math
from pathlib import Path
import random
import zipfile
import numpy as np
import torch
import scipy
from torch import nn
from torch.utils.data import DataLoader, TensorDataset, WeightedRandomSampler

from .common import new_output, sha256, write_json
from .model import SpectrogramCNN
from .preprocess import from_metadata
from .splits import assign


def fit_normalization(images):
    mean, scale = float(np.mean(images, dtype=np.float64)), float(np.std(images, dtype=np.float64))
    if not np.isfinite([mean, scale]).all() or scale < 1e-6:
        raise ValueError('training tensor has no usable variance')
    return mean, scale


def load_images(path):
    # Inspect uncompressed size and NPY header before allocating a compressed tensor.
    with zipfile.ZipFile(path) as archive:
        if archive.namelist() != ['images.npy']:
            raise ValueError('tensor archive must contain only images.npy')
        entry = archive.getinfo('images.npy')
        if entry.file_size > 256 * 1024 * 1024:
            raise ValueError('expanded tensor exceeds memory budget')
        with archive.open(entry) as f:
            version = np.lib.format.read_magic(f)
            if version != (1, 0):
                raise ValueError('unsupported NPY version')
            shape, fortran, dtype = np.lib.format.read_array_header_1_0(f)
            if (len(shape) != 3 or any(n <= 0 for n in shape) or math.prod(shape) > 64_000_000
                    or dtype != np.dtype('float32') or fortran):
                raise ValueError('unsupported tensor shape or dtype')
    with np.load(path, allow_pickle=False) as data:
        return data['images']


def thresholds(probabilities, distances, labels, groups, classes):
    predicted = probabilities.argmax(1)
    policy = []
    for k in range(classes):
        mask = (labels == k) & (predicted == k)
        # Multiple independent groups matter, not thousands of adjacent windows.
        if mask.sum() < 5 or len(set(np.asarray(groups)[mask])) < 2:
            raise ValueError('each class needs correct calibration examples from at least two groups')
        policy.append(dict(min_probability=float(np.quantile(probabilities[mask, k], .05)),
                           max_distance=float(np.quantile(distances[mask, k], .95))))
    return policy


def accept(probabilities, distances, policy):
    predicted = probabilities.argmax(1)
    sorted_scores = np.sort(probabilities, axis=1)
    margins = sorted_scores[:, -1] - sorted_scores[:, -2]
    allowed = np.array([(p[k] >= policy[k]['min_probability'] and d[k] <= policy[k]['max_distance'])
                        and margin >= policy[k].get('min_margin', 0)
                        for p, d, k, margin in zip(probabilities, distances, predicted, margins)])
    return np.where(allowed, predicted, -1)


def metrics(predictions, truth, records, classes):
    result = dict(windows=len(truth), groups=len({r['split_group'] for r in records}),
                  acceptance_fraction=float(np.mean(predictions >= 0)))
    known = truth >= 0
    if known.any():
        result['correct_and_accepted_fraction'] = float(np.mean(predictions[known] == truth[known]))
        result['per_class'] = {c: dict(windows=int(np.sum(truth == k)),
            correct_and_accepted_fraction=float(np.mean(predictions[truth == k] == k)))
            for k, c in enumerate(classes) if np.any(truth == k)}
    if (~known).any():
        result['unknown_false_accept_fraction'] = float(np.mean(predictions[~known] >= 0))
    return result


def run(corpus_path, output, epochs=3, batch_size=16, seed=17, development_smoke=False, split_plan=None,
        development_only=False, joint_calibration=False):
    if not 1 <= epochs <= 100 or not 1 <= batch_size <= 128:
        raise ValueError('training resource bounds exceeded')
    corpus_path = Path(corpus_path).resolve()
    corpus = json.loads(corpus_path.read_text())
    if corpus.get('schema') != 'rfmon.drone_ml.corpus.v1':
        raise ValueError('unsupported corpus')
    tensor_path = corpus_path.parent / 'spectrograms.npz'
    if tensor_path.stat().st_size > 256 * 1024 * 1024 or sha256(tensor_path) != corpus['tensor_sha256']:
        raise ValueError('tensor hash or size mismatch')
    images = load_images(tensor_path)
    records = corpus['records']
    if corpus.get('development_only') and not development_only:
        raise ValueError('development corpus requires explicit development-only mode')
    if development_only and development_smoke:
        raise ValueError('development-only and training smoke are separate modes')
    cfg = from_metadata(corpus['preprocessing'])
    cfg.validate()
    shape = (cfg.nfft, 1 + (round(cfg.rate_hz * cfg.duration_seconds) - cfg.nfft) // cfg.hop)
    if (images.ndim != 3 or len(images) != len(records) or not np.isfinite(images).all()
            or images.shape[1:] != shape
            or [r['index'] for r in records] != list(range(len(records)))):
        raise ValueError('tensor/manifest mismatch')
    if development_smoke:
        if split_plan is not None or any(r['partition'] != 'development_only' for r in records):
            raise ValueError('smoke accepts development-only data without split promotion')
        records = [dict(r, partition='train') for r in records]
    else:
        if split_plan is None or any(r['partition'] == 'development_only' for r in records):
            raise ValueError('development-exposed corpus cannot be promoted; independent reviewed corpus required')
        if corpus.get('origin_split_plan_sha256') and sha256(split_plan) != corpus['origin_split_plan_sha256']:
            raise ValueError('split changed after corpus preparation')
        records = assign(records, json.loads(Path(split_plan).read_text()), development_only=development_only)
    classes = sorted({r['label'] for r in records if r['partition'] == 'train'})
    if len(classes) < 2:
        raise ValueError('requires two or more known classes')
    labels = np.array([classes.index(r['label']) if r['label'] in classes else -1 for r in records])
    partitions = np.array([r['partition'] for r in records])
    eligible = np.array([bool(r.get('model_eligible', True)) for r in records])
    selected = np.flatnonzero((partitions == 'train') & eligible)
    if any(not np.any(labels[selected] == k) for k in range(len(classes))):
        raise ValueError('activity gate leaves a known class without training examples')
    mean, scale = fit_normalization(images[selected])
    x = torch.from_numpy(((images - mean) / scale)[:, None])
    y = torch.from_numpy(labels)
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.set_num_threads(2)
    torch.use_deterministic_algorithms(True)
    model = SpectrogramCNN(len(classes))
    optimizer = torch.optim.AdamW(model.parameters(), lr=.001, weight_decay=.001)
    # Each recording group has equal weight inside a class, independent of window count.
    class_groups = {c: {records[i]['split_group'] for i in selected if records[i]['label'] == c}
                    for c in classes}
    group_counts = {g: sum(records[i]['split_group'] == g for i in selected)
                    for groups in class_groups.values() for g in groups}
    weights = [1 / (len(class_groups[records[i]['label']]) * group_counts[records[i]['split_group']]) for i in selected]
    sampler = WeightedRandomSampler(weights, len(selected), replacement=True,
                                    generator=torch.Generator().manual_seed(seed))
    loader = DataLoader(TensorDataset(x[selected], y[selected]), batch_size=batch_size, sampler=sampler)
    history = []
    for epoch in range(epochs):
        model.train()
        total = 0.0
        for bx, by in loader:
            optimizer.zero_grad()
            logits, _ = model(bx)
            loss = nn.functional.cross_entropy(logits, by)
            loss.backward()
            optimizer.step()
            total += float(loss.detach()) * len(by)
        history.append(dict(epoch=epoch + 1, training_loss=total / len(selected)))
        print(json.dumps(history[-1]), flush=True)
    model.eval()
    logits, embeddings = [], []
    with torch.no_grad():
        for batch in x.split(batch_size):
            l, e = model(batch)
            logits.append(l)
            embeddings.append(e)
    probabilities = torch.cat(logits).softmax(1).numpy()
    embedding = torch.cat(embeddings).numpy()
    prototypes = np.stack([embedding[selected[labels[selected] == k]].mean(0) for k in range(len(classes))])
    prototypes /= np.maximum(np.linalg.norm(prototypes, axis=1, keepdims=True), 1e-12)
    distances = np.maximum(0, 1 - embedding @ prototypes.T)
    policy, evaluated, policy_blockers, calibration_search = None, {}, [], None
    if not development_smoke:
        cal = (partitions == 'calibration') & eligible
        try:
            if joint_calibration:
                from .calibration import choose
                calibration = np.isin(partitions, ['calibration', 'unknown_calibration'])
                policy, calibration_search = choose(probabilities[calibration], distances[calibration],
                    labels[calibration], [r['split_group'] for r, m in zip(records, calibration) if m],
                    eligible[calibration], len(classes), accept)
                if policy is None:
                    policy_blockers.append('no_policy_improves_over_abstention_on_group_balanced_calibration')
            else:
                policy = thresholds(probabilities[cal], distances[cal], labels[cal],
                                    [r['split_group'] for r, m in zip(records, cal) if m], len(classes))
        except ValueError as e:
            policy_blockers.append(str(e))
        # Tighten only with development unknowns; final unknowns never select thresholds.
        unknown_cal = partitions == 'unknown_calibration'
        for k, entry in enumerate(policy or [] if not joint_calibration else []):
            scores = probabilities[unknown_cal, k]
            entry['min_probability'] = max(entry['min_probability'], float(np.quantile(scores, .95)) + 1e-6)
        predictions = accept(probabilities, distances, policy) if policy else None
        if policy:
            predictions[~eligible] = -1
        evaluation_partitions = ('calibration', 'unknown_calibration') if development_only else ('calibration', 'unknown_calibration', 'test', 'unknown_test')
        for partition in evaluation_partitions:
            mask = partitions == partition
            rows = [r for r, m in zip(records, mask) if m]
            result = metrics(predictions[mask], labels[mask], rows, classes) if policy else dict(
                windows=int(mask.sum()), groups=len({r['split_group'] for r in rows}),
                rejection_status='unavailable', rejection_blockers=policy_blockers)
            result['activity_eligible_windows'] = int(np.sum(mask & eligible))
            if policy and np.any(mask & eligible):
                result['activity_eligible_metrics'] = metrics(predictions[mask & eligible], labels[mask & eligible],
                    [r for r, m in zip(records, mask & eligible) if m], classes)
            if partition in ('calibration', 'test'):
                closed = probabilities[mask].argmax(1)
                result['closed_set_accuracy'] = float(np.mean(closed == labels[mask]))
                result['confusion_matrix'] = [[int(np.sum((labels[mask] == a) & (closed == b)))
                                               for b in range(len(classes))] for a in range(len(classes))]
                # One encounter vote per source recording; windows are correlated.
                votes = []
                for rid in sorted({r['recording_id'] for r in rows}):
                    indices = [i for i in np.flatnonzero(mask) if records[i]['recording_id'] == rid]
                    guessed = int(probabilities[indices].mean(0).argmax())
                    votes.append(dict(recording_id=rid, truth=classes[int(labels[indices[0]])],
                                      predicted=classes[guessed], correct=bool(guessed == labels[indices[0]])))
                result['recording_votes'] = votes
                result['recording_vote_accuracy'] = float(np.mean([v['correct'] for v in votes]))
            evaluated[partition] = result
    out = new_output(output, [corpus_path.parent, Path('/media/sudeep/Storage/datasets')])
    checkpoint = dict(schema='rfmon.drone_ml.checkpoint.v1', state_dict=model.state_dict(), classes=classes,
                      normalization=dict(mean=mean, scale=scale), preprocessing=corpus['preprocessing'],
                      prototypes=prototypes.tolist(), policy=policy, development_smoke=development_smoke,
                      development_only=development_only, activity_policy=corpus.get('activity_policy'),
                      policy_blockers=policy_blockers)
    torch.save(checkpoint, out / 'model.pt')
    report = dict(schema='rfmon.drone_ml.training.v1', classes=classes, epochs=epochs, seed=seed,
        parameters=sum(p.numel() for p in model.parameters()), device='cpu', history=history,
        corpus_sha256=sha256(corpus_path), split_plan_sha256=sha256(split_plan) if split_plan else None,
        model_sha256=sha256(out / 'model.pt'), normalization=checkpoint['normalization'],
        dependencies=dict(numpy=np.__version__, scipy=scipy.__version__, torch=torch.__version__),
        implementation_sha256={name: sha256(Path(__file__).with_name(name))
                               for name in ('train.py', 'model.py', 'preprocess.py', 'splits.py', 'activity.py', 'calibration.py')},
        development_smoke=development_smoke, unknown_rejection_enabled=policy is not None,
        development_only=development_only, joint_calibration=joint_calibration,
        calibration_search=calibration_search, training_eligible_windows=len(selected),
        release_validation_eligible=False, evaluated=evaluated, calibration_blockers=policy_blockers,
        blockers=['local_receiver_unqualified', 'no_drone_vs_non_drone_validation'] +
                 (['training_only_no_independent_metrics', 'weak_recording_context_labels'] if development_smoke else []))
    write_json(out / 'report.json', report)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--epochs', type=int, default=3)
    p.add_argument('--batch-size', type=int, default=16)
    p.add_argument('--seed', type=int, default=17)
    p.add_argument('--development-smoke', action='store_true')
    p.add_argument('--split-plan', type=Path)
    p.add_argument('--development-only', action='store_true')
    p.add_argument('--joint-calibration', action='store_true')
    a = p.parse_args()
    r = run(a.corpus, a.output, a.epochs, a.batch_size, a.seed, a.development_smoke, a.split_plan,
            a.development_only, a.joint_calibration)
    print(json.dumps(dict(parameters=r['parameters'], unknown_rejection_enabled=r['unknown_rejection_enabled'],
                         release_validation_eligible=False)))


if __name__ == '__main__':
    main()
