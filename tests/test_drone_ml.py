"""Standalone ML guards and physical-scale DSP; synthetic tests are not RF accuracy."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import numpy as np
import torch
from tools.drone_ml.audit import inventory
from tools.drone_ml.common import new_output, sha256, stamp
from tools.drone_ml.model import SpectrogramCNN
from tools.drone_ml.prepare import prepare
from tools.drone_ml.predict import predict
from tools.drone_ml.preprocess import STFTConfig, from_metadata, select_offset, spectrogram
from tools.drone_ml.splits import assign
from tools.drone_ml.train import accept, fit_normalization, load_images, run, thresholds


def tone(rate, hz, seconds=.001):
    return np.exp(2j * np.pi * hz * np.arange(round(rate * seconds)) / rate).astype(np.complex64)


def split_fixture():
    records, groups = [], {}
    for label in ('a', 'b'):
        for partition in ('train', 'calibration', 'test'):
            g = label + ':' + partition
            records.append(dict(split_group=g, label=label, recording_id=g,
                                source_sha256=g, original_source_group=g, offset_samples=0, samples=100))
            groups[g] = dict(partition=partition, label_evidence='synthetic_fixture', independence_evidence='synthetic_fixture')
    for label, partition in (('c', 'unknown_calibration'), ('d', 'unknown_test')):
        g = label + ':' + partition
        records.append(dict(split_group=g, label=label, recording_id=g, source_sha256=g,
                            original_source_group=g, offset_samples=0, samples=100))
        groups[g] = dict(partition=partition, label_evidence='synthetic_fixture', independence_evidence='synthetic_fixture')
    return records, dict(schema='rfmon.drone_ml.split.v1', groups=groups)


class DSPTests(unittest.TestCase):
    def test_shape_and_positive_frequency(self):
        image = spectrogram(tone(100_000_000, 2_031_250), 100_000_000)
        self.assertEqual(image.shape, (256, 155))
        self.assertEqual(int(image.mean(1).argmax()), 128 + 26)

    def test_negative_frequency_preserved(self):
        image = spectrogram(tone(120_000_000, -2_031_250), 120_000_000)
        self.assertEqual(int(image.mean(1).argmax()), 128 - 26)

    def test_gain_invariance(self):
        iq = tone(100_000_000, 2_000_000)
        # Float32 gain multiplication perturbs bins near the numerical noise floor.
        np.testing.assert_allclose(spectrogram(iq, 100_000_000), spectrogram(iq * 20, 100_000_000), atol=.05)

    def test_frequency_translation(self):
        image = spectrogram(tone(100_000_000, 22_031_250), 100_000_000, STFTConfig(offset_hz=20_000_000))
        self.assertEqual(int(image.mean(1).argmax()), 128 + 26)

    def test_native_rate_allowed(self):
        self.assertEqual(spectrogram(tone(20_000_000, 2_000_000), 20_000_000).shape, (256, 155))

    def test_automatic_view_preserves_outside_center_signal(self):
        iq = tone(100_000_000, 22_000_000)
        cfg = STFTConfig(auto_frequency_view=True)
        self.assertEqual(select_offset(iq, 100_000_000, cfg), 17_500_000)
        self.assertEqual(spectrogram(iq, 100_000_000, cfg).shape, (256, 155))
        with self.assertRaises(ValueError):
            STFTConfig(auto_frequency_view=True, offset_hz=1).validate()

    def test_legacy_preprocessing_metadata(self):
        metadata = STFTConfig().metadata()
        del metadata['auto_frequency_view']
        self.assertFalse(from_metadata(metadata).auto_frequency_view)

    def test_antialias_filter(self):
        iq = tone(100_000_000, 2_031_250) + tone(100_000_000, 32_031_250)
        image = spectrogram(iq, 100_000_000)
        self.assertGreater(float(image[154, 20:-20].mean() - image[26, 20:-20].mean()), 40)

    def test_nonfinite_zero_and_short_rejected(self):
        for iq in (np.zeros(100_000, dtype=complex), np.ones(100_000, dtype=complex),
                   np.full(100_000, np.nan, dtype=complex), tone(100_000_000, 2e6)[:-1]):
            with self.assertRaises(ValueError):
                spectrogram(iq, 100_000_000)

    def test_bad_geometry_rejected(self):
        for cfg in (STFTConfig(offset_hz=45e6), STFTConfig(hop=0), STFTConfig(duration_seconds=1)):
            with self.assertRaises(ValueError):
                spectrogram(tone(100_000_000, 2e6), 100_000_000, cfg)


class SplitTests(unittest.TestCase):
    def test_explicit_disjoint_plan(self):
        rows, plan = split_fixture()
        self.assertEqual(len(assign(rows, plan)), 8)

    def test_shared_source_leak_rejected(self):
        for key in ('recording_id', 'source_sha256', 'original_source_group'):
            rows, plan = split_fixture()
            rows[1][key] = rows[0][key]
            with self.assertRaises(ValueError):
                assign(rows, plan)

    def test_unknown_class_leak_rejected(self):
        rows, plan = split_fixture()
        rows[-1]['label'] = 'a'
        with self.assertRaises(ValueError):
            assign(rows, plan)

    def test_calibration_unknown_not_final_unknown(self):
        rows, plan = split_fixture()
        rows[-1]['label'] = rows[-2]['label']
        with self.assertRaises(ValueError):
            assign(rows, plan)

    def test_missing_evidence_rejected(self):
        rows, plan = split_fixture()
        plan['groups']['a:train']['independence_evidence'] = None
        with self.assertRaises(ValueError):
            assign(rows, plan)

    def test_missing_group_rejected(self):
        rows, plan = split_fixture()
        del plan['groups']['a:test']
        with self.assertRaises(ValueError):
            assign(rows, plan)


class PipelineTests(unittest.TestCase):
    def test_inventory_is_excluded_and_preserves_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            folder = root / 'KULeuven_DroneRF'
            folder.mkdir()
            archive = folder / 'test.zip'
            with zipfile.ZipFile(archive, 'w') as f:
                f.writestr('test.mat', b'not IQ')
            before = stamp(archive)
            report = inventory(root)
            self.assertEqual(stamp(archive), before)
            self.assertEqual(report['summary']['train_eligible'], 0)
            self.assertEqual(report['records'][0]['partition'], 'excluded')
            self.assertFalse(report['records'][0]['archive_crc_verified'])

    def test_source_output_isolation(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                new_output(Path(tmp) / 'nested', [tmp])

    def test_normalization_training_only(self):
        train = np.array([1, 2, 3], dtype=np.float32)
        self.assertEqual(fit_normalization(train)[0], 2)
        with self.assertRaises(ValueError):
            fit_normalization(np.ones(5))

    def test_calibration_requires_independent_groups(self):
        probabilities = np.tile([.9, .1], (10, 1))
        distances = np.tile([.1, .9], (10, 1))
        with self.assertRaises(ValueError):
            thresholds(probabilities, distances, np.zeros(10), ['same'] * 10, 1)
        policy = thresholds(probabilities, distances, np.zeros(10), ['x'] * 5 + ['y'] * 5, 1)
        self.assertEqual(accept(np.array([[.95, .05], [.5, .5]]), np.array([[.05, .9], [.5, .9]]), policy).tolist(), [0, -1])

    def test_model_shapes_and_small_size(self):
        torch.set_num_threads(2)
        model = SpectrogramCNN(6)
        logits, embedding = model(torch.zeros(2, 1, 256, 155))
        self.assertEqual(tuple(logits.shape), (2, 6))
        self.assertEqual(tuple(embedding.shape), (2, 64))
        self.assertLess(sum(p.numel() for p in model.parameters()), 300_000)

    def test_tensor_dtype_and_members_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'a.npz'
            np.savez_compressed(path, images=np.zeros((2, 4, 4), dtype=np.float64))
            with self.assertRaises(ValueError):
                load_images(path)
            np.savez_compressed(path, images=np.zeros((2, 4, 4), dtype=np.float32), unrelated=np.ones(3))
            with self.assertRaises(ValueError):
                load_images(path)

    def test_end_to_end_smoke_and_no_validation_promotion(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            refs = root / 'references'
            refs.mkdir()
            rows = []
            for name, hz in (('mini2vid.zip', 2e6), ('inspire2Vid.zip', -2e6)):
                iq = tone(100_000_000, hz, .004)
                path = refs / (name + '.cf32')
                path.write_bytes(iq.astype('<c8').tobytes())
                rows.append(dict(source='/publisher/KULeuven_DroneRF/' + name, path=str(path),
                    format='cf32_le', samples=len(iq), sample_rate_hz=100_000_000,
                    capture_center_hz=2_440_000_000, sha256=sha256(path),
                    partition='development_only', split_group=name))
            manifest = refs / 'manifest.json'
            manifest.write_text(json.dumps(dict(schema='local_link_references.v1', records=rows)))
            before = [stamp(Path(r['path'])) for r in rows]
            corpus = root / 'corpus'
            prepare(manifest, corpus, windows=2)
            self.assertEqual([stamp(Path(r['path'])) for r in rows], before)
            report = run(corpus / 'corpus.json', root / 'model', epochs=1, batch_size=2, development_smoke=True)
            self.assertFalse(report['unknown_rejection_enabled'])
            self.assertEqual(report['evaluated'], {})
            result = predict(root / 'model/model.pt', rows[0]['path'], 'cf32_le', 100_000_000)
            self.assertEqual(result['status'], 'development_prediction_only')
            self.assertIsNone(result['accepted_class'])
            with self.assertRaises(ValueError):
                run(corpus / 'corpus.json', root / 'forbidden', split_plan=root / 'nonexistent.json')
            broken = copy.deepcopy(rows)
            broken[0]['sha256'] = '0' * 64
            manifest.write_text(json.dumps(dict(schema='local_link_references.v1', records=broken)))
            with self.assertRaises(ValueError):
                prepare(manifest, root / 'broken', windows=2)

    def test_reviewed_synthetic_open_set_workflow(self):
        # Exercise calibration/test plumbing only; these are generated software fixtures.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            corpus = root / 'corpus'
            corpus.mkdir()
            rows, groups, images = [], {}, []
            rng = np.random.default_rng(8)
            for label, parts in (('a', [('train', 0), ('calibration', 0), ('calibration', 1), ('test', 0)]),
                                 ('b', [('train', 0), ('calibration', 0), ('calibration', 1), ('test', 0)]),
                                 ('c', [('unknown_calibration', 0)]), ('d', [('unknown_test', 0)])):
                for part, group_number in parts:
                    g = label + ':' + part + ':' + str(group_number)
                    groups[g] = dict(partition=part, label_evidence='synthetic_test', independence_evidence='synthetic_test')
                    for j in range(6):
                        image = rng.normal(0, .01, (64, 30)).astype(np.float32)
                        if label in ('a', 'b'):
                            image[(slice(8, 16) if label == 'a' else slice(48, 56)), :] += 5
                        else:
                            image += rng.normal(0, 1, image.shape).astype(np.float32)
                        images.append(image)
                        rows.append(dict(index=len(rows), partition='unassigned', recording_id=g,
                            split_group=g, source_sha256=g, original_source_group=g, offset_samples=j * 100,
                            samples=100, label=label))
            np.savez_compressed(corpus / 'spectrograms.npz', images=np.stack(images))
            (corpus / 'corpus.json').write_text(json.dumps(dict(schema='rfmon.drone_ml.corpus.v1',
                tensor_sha256=sha256(corpus / 'spectrograms.npz'), records=rows,
                preprocessing=STFTConfig(rate_hz=1_000_000, nfft=64, hop=32).metadata())))
            split = root / 'splits.json'
            split.write_text(json.dumps(dict(schema='rfmon.drone_ml.split.v1', groups=groups)))
            report = run(corpus / 'corpus.json', root / 'model', epochs=4, batch_size=12, split_plan=split)
            self.assertTrue(report['unknown_rejection_enabled'])
            self.assertIn('unknown_false_accept_fraction', report['evaluated']['unknown_test'])
            self.assertFalse(report['release_validation_eligible'])
            # Exercise the development runner on a corpus that physically omits test tensors.
            dev = root / 'development'
            dev.mkdir()
            chosen = [i for i, row in enumerate(rows) if groups[row['split_group']]['partition']
                      in ('train', 'calibration', 'unknown_calibration')]
            dev_rows = [dict(rows[i], index=j, model_eligible=True) for j, i in enumerate(chosen)]
            np.savez_compressed(dev / 'spectrograms.npz', images=np.stack([images[i] for i in chosen]))
            dev_json = dict(schema='rfmon.drone_ml.corpus.v1', development_only=True,
                tensor_sha256=sha256(dev / 'spectrograms.npz'), records=dev_rows,
                preprocessing=STFTConfig(rate_hz=1_000_000, nfft=64, hop=32).metadata())
            (dev / 'corpus.json').write_text(json.dumps(dev_json))
            dev_split = root / 'dev_split.json'
            dev_split.write_text(json.dumps(dict(schema='rfmon.drone_ml.split.v1',
                groups={g: d for g, d in groups.items() if g in {r['split_group'] for r in dev_rows}})))
            dev_report = run(dev / 'corpus.json', root / 'dev_model', epochs=4, batch_size=12,
                split_plan=dev_split, development_only=True, joint_calibration=True)
            self.assertEqual(set(dev_report['evaluated']), {'calibration', 'unknown_calibration'})
            self.assertIsNotNone(dev_report['calibration_search'])
            with self.assertRaisesRegex(ValueError, 'explicit development-only'):
                run(dev / 'corpus.json', root / 'illegal', split_plan=dev_split)
            frozen = json.loads((corpus / 'corpus.json').read_text())
            frozen['origin_split_plan_sha256'] = '0' * 64
            (corpus / 'corpus.json').write_text(json.dumps(frozen))
            with self.assertRaisesRegex(ValueError, 'split changed'):
                run(corpus / 'corpus.json', root / 'changed-split', epochs=1, split_plan=split)


if __name__ == '__main__':
    unittest.main()
